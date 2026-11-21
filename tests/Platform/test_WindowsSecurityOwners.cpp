/// @file test_WindowsSecurityOwners.cpp
/// @brief Who owns a handle, and the owners the elevated checks trust (#4368): the named predicates over owners read
/// from security descriptors built from SDDL, ownerSidOf() on real handles (a folder this test made, owned by its own
/// account, and the Windows folder, TrustedInstaller's), and each caller's own set of trusted owners over those same
/// handles, which differ on purpose. Windows only: nothing here elevates, and nothing outside the test's own temp folder
/// is written.

#include "Platform/Windows/ElevatedRestart.h"
#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/WindowsDumpFolder.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsSecurityOwners.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h> // ConvertStringSecurityDescriptorToSecurityDescriptorW
// clang-format on

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// A security descriptor built from SDDL, LocalFree'd when it dies; null when the SDDL doesn't parse.
using Descriptor = std::unique_ptr<void, decltype(&::LocalFree)>;

[[nodiscard]] Descriptor descriptorFrom(const std::wstring& sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
    {
        return {nullptr, &::LocalFree};
    }
    return {descriptor, &::LocalFree};
}

/// @p descriptor's owner SID (pointing into it); null when it names none.
[[nodiscard]] PSID ownerOf(const Descriptor& descriptor)
{
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    if (GetSecurityDescriptorOwner(descriptor.get(), &owner, &defaulted) == FALSE)
    {
        return nullptr;
    }
    return owner;
}

/// This test's own account, as an SDDL SID string.
[[nodiscard]] std::wstring ownSid()
{
    return currentUserSidText().value_or(std::wstring{});
}

/// One owner, and what each predicate says of it.
struct OwnerRow
{
    std::wstring owner; ///< SDDL: an alias (BA, SY) or an S-1-... SID; "me" is this test's own account
    bool administrators = false;
    bool administratorsOrSystem = false;
    bool trustedInstaller = false;
    bool tokenUser = false;
};

// Every predicate over every owner, each from a descriptor built from SDDL: Administrators, SYSTEM, TrustedInstaller, this
// account, another user, and the well-known groups and services none of them is.
TEST(WindowsSecurityOwnersTest, EachPredicateNamesOnlyItsOwnOwners)
{
    const std::wstring me = ownSid();
    ASSERT_FALSE(me.empty());
    const std::vector<OwnerRow> rows{
        {.owner = L"BA", .administrators = true, .administratorsOrSystem = true},
        {.owner = L"SY", .administratorsOrSystem = true},
        {.owner = std::wstring(TRUSTED_INSTALLER_SID), .trustedInstaller = true},
        {.owner = me, .tokenUser = true},
        {.owner = L"S-1-5-21-1000000001-1000000002-1000000003-1001"}, // a user of no account here
        {.owner = L"BU"},                                             // BUILTIN\Users
        {.owner = L"AU"},                                             // Authenticated Users
        {.owner = L"WD"},                                             // Everyone
        {.owner = L"CO"},                                             // CREATOR OWNER
        {.owner = L"LS"},                                             // LocalService
        {.owner = L"NS"},                                             // NetworkService
        {.owner = L"S-1-5-80-0"},                                     // NT SERVICE\ALL SERVICES, not TrustedInstaller
        {.owner = L"S-1-5-32-547"},                                   // BUILTIN\Power Users, not Administrators
    };
    for (const OwnerRow& row : rows)
    {
        const Descriptor descriptor = descriptorFrom(std::format(L"O:{}D:(A;;FA;;;BA)", row.owner));
        ASSERT_NE(descriptor, nullptr) << GetLastError();
        const PSID owner = ownerOf(descriptor);
        ASSERT_NE(owner, nullptr);
        const std::string shown(row.owner.begin(), row.owner.end()); // SDDL is ASCII
        EXPECT_EQ(isAdministrators(owner), row.administrators) << shown;
        EXPECT_EQ(isAdministratorsOrSystem(owner), row.administratorsOrSystem) << shown;
        EXPECT_EQ(isTrustedInstaller(owner), std::optional(row.trustedInstaller)) << shown;
        EXPECT_EQ(isTrustedInstallerConvertedOnce(owner), std::optional(row.trustedInstaller)) << shown;
        const auto tokenUser = isTokenUser(owner);
        ASSERT_TRUE(tokenUser.has_value()) << tokenUser.error();
        EXPECT_EQ(*tokenUser, row.tokenUser) << shown;
    }
}

// The SID converted once gives the same answer every call: it is kept, not converted again.
TEST(WindowsSecurityOwnersTest, TheTrustedInstallerSidConvertedOnceKeepsAnswering)
{
    const Descriptor descriptor = descriptorFrom(std::format(L"O:{}", TRUSTED_INSTALLER_SID));
    ASSERT_NE(descriptor, nullptr) << GetLastError();
    for (int call = 0; call < 3; ++call)
    {
        EXPECT_EQ(isTrustedInstallerConvertedOnce(ownerOf(descriptor)), std::optional(true)) << call;
    }
}

// A handle that can't be asked has no owner, with the reason: nothing reads it as anyone's.
TEST(WindowsSecurityOwnersTest, AHandleThatCantBeAskedHasNoOwner)
{
    const auto none = ownerSidOf(nullptr);
    ASSERT_FALSE(none.has_value());
    EXPECT_NE(none.error(), static_cast<DWORD>(ERROR_SUCCESS));
    // (Not INVALID_HANDLE_VALUE: that is also GetCurrentProcess()'s pseudo-handle, which GetSecurityInfo reads as this process.)
}

/// A folder of this test's own under the temp folder, owned by this account explicitly (an elevated run's would
/// otherwise be Administrators', its default owner); removed at the end.
class OwnFolder
{
  public:
    OwnFolder()
        : m_Path(std::filesystem::temp_directory_path() /
                 std::format(L"TaskSmack.SecurityOwnersTest.{}.{}", GetCurrentProcessId(), GetTickCount64()))
    {
        const Descriptor security = descriptorFrom(std::format(L"O:{0}D:P(A;OICI;FA;;;{0})", ownSid()));
        EXPECT_NE(security, nullptr) << GetLastError();
        SECURITY_ATTRIBUTES attributes{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = security.get(),
            .bInheritHandle = FALSE,
        };
        EXPECT_NE(CreateDirectoryW(m_Path.c_str(), &attributes), FALSE) << GetLastError();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~OwnFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
    }
    OwnFolder(const OwnFolder&) = delete;
    OwnFolder& operator=(const OwnFolder&) = delete;
    OwnFolder(OwnFolder&&) = delete;
    OwnFolder& operator=(OwnFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// @p folder opened for READ_CONTROL only, as the owner checks read it; never written.
[[nodiscard]] UniqueHandle openToRead(const std::filesystem::path& folder)
{
    return UniqueHandle(CreateFileW(folder.c_str(),
                                    READ_CONTROL,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr));
}

/// The Windows folder, TrustedInstaller's on every Windows install.
[[nodiscard]] std::filesystem::path windowsFolder()
{
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    EXPECT_NE(length, 0U) << GetLastError();
    return {windows.data()};
}

// ownerSidOf() reads a real handle's owner: this account's own folder is the token user's, no administrator's.
TEST(WindowsSecurityOwnersTest, AFolderThisAccountOwnsIsTheTokenUsers)
{
    const OwnFolder folder;
    const UniqueHandle held = openToRead(folder.path());
    ASSERT_TRUE(held) << GetLastError();
    const auto owner = ownerSidOf(held.get());
    ASSERT_TRUE(owner.has_value()) << owner.error();
    EXPECT_NE(owner->descriptor, nullptr);
    const auto tokenUser = isTokenUser(owner->sid);
    ASSERT_TRUE(tokenUser.has_value()) << tokenUser.error();
    EXPECT_TRUE(*tokenUser);
    EXPECT_FALSE(isAdministratorsOrSystem(owner->sid));
    EXPECT_EQ(isTrustedInstaller(owner->sid), std::optional(false));
}

// Each caller's set of trusted owners, over the same two real folders. They differ on purpose and are kept so (#4368):
// TrustedInstaller is privileged to ElevatedRestart::ownerIsPrivileged() and trusted by
// ownerIsSystemOrAdministratorsOrTrustedInstaller(), but HandleRelativeFile's ownerIsAdministratorsOrSystem() never
// trusted it, and the own config folder's ownerIsOwnAccountOrAdministrators() takes this account instead.
TEST(WindowsSecurityOwnersTest, EachCallerKeepsItsOwnSetOfTrustedOwners)
{
    const OwnFolder own;
    const UniqueHandle mine = openToRead(own.path());
    ASSERT_TRUE(mine) << GetLastError();
    EXPECT_EQ(ElevatedRestart::ownerIsPrivileged(mine.get()), std::optional(false));
    EXPECT_EQ(ownerIsAdministratorsOrSystem(mine.get()), std::optional(false));
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(mine.get()), std::optional(false));
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(mine.get()), std::optional(true));

    const UniqueHandle windows = openToRead(windowsFolder());
    ASSERT_TRUE(windows) << GetLastError();
    const auto owner = ownerSidOf(windows.get());
    ASSERT_TRUE(owner.has_value()) << owner.error();
    ASSERT_EQ(isTrustedInstaller(owner->sid), std::optional(true)) << "the Windows folder is TrustedInstaller's";
    EXPECT_EQ(ElevatedRestart::ownerIsPrivileged(windows.get()), std::optional(true));
    EXPECT_EQ(ownerIsAdministratorsOrSystem(windows.get()), std::optional(false));
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(windows.get()), std::optional(true));
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(windows.get()), std::optional(false));

    // And none of them reads a handle that can't be asked as trusted.
    EXPECT_EQ(ElevatedRestart::ownerIsPrivileged(nullptr), std::nullopt);
    EXPECT_EQ(ownerIsAdministratorsOrSystem(nullptr), std::nullopt);
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(nullptr), std::nullopt);
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(nullptr), std::nullopt);
}

} // namespace
} // namespace Platform::Windows
