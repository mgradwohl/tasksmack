/// @file test_WindowsSecurityOwners.cpp
/// @brief Who owns a handle, and the owners the elevated checks trust (#4368): the named predicates over owners read
/// from security descriptors built from SDDL, the static TrustedInstaller SID against its text (#4699), ownerSidOf() on
/// real handles (a folder this test made, owned by its own account, and the Windows folder, TrustedInstaller's), and each
/// caller's own set of trusted owners over those same handles. Windows only: nothing here elevates, and nothing outside the test's own temp
/// folder is written.

#include "Platform/Windows/ElevatedRestart.h"
#include "Platform/Windows/HandleRelativeFile.h"
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
#include <sddl.h> // ConvertStringSecurityDescriptorToSecurityDescriptorW, ConvertStringSidToSidW, ConvertSidToStringSidW
// clang-format on

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

/// NT SERVICE\TrustedInstaller's service SID as text: the test's own copy, which the static SID is checked against.
constexpr std::wstring_view TRUSTED_INSTALLER_SID = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

/// A SID ConvertStringSidToSidW made, LocalFree'd when it dies; null when the text doesn't convert.
using ConvertedSid = std::unique_ptr<void, decltype(&::LocalFree)>;

[[nodiscard]] ConvertedSid sidFrom(std::wstring_view text)
{
    const std::wstring terminated(text);
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(terminated.c_str(), &sid) == FALSE)
    {
        return {nullptr, &::LocalFree};
    }
    return {sid, &::LocalFree};
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
        EXPECT_EQ(isTrustedInstaller(owner), row.trustedInstaller) << shown;
        const auto tokenUser = isTokenUser(owner);
        ASSERT_TRUE(tokenUser.has_value()) << tokenUser.error();
        EXPECT_EQ(*tokenUser, row.tokenUser) << shown;
    }
}

// The static TrustedInstaller SID, built from its parts (#4699), is a valid SID and the one its text converts to, and
// it reads back as that text: six sub-authorities, nothing past them.
TEST(WindowsSecurityOwnersTest, TheStaticTrustedInstallerSidIsTheOneItsTextNames)
{
    const PSID built = trustedInstallerSid();
    ASSERT_NE(built, nullptr);
    EXPECT_NE(IsValidSid(built), FALSE);
    EXPECT_EQ(GetLengthSid(built), static_cast<DWORD>(SECURITY_SID_SIZE(6)));
    EXPECT_EQ(*GetSidSubAuthorityCount(built), 6);
    const ConvertedSid converted = sidFrom(TRUSTED_INSTALLER_SID);
    ASSERT_NE(converted, nullptr) << GetLastError();
    EXPECT_NE(EqualSid(built, converted.get()), FALSE);
    LPWSTR text = nullptr;
    ASSERT_NE(ConvertSidToStringSidW(built, &text), FALSE) << GetLastError();
    const std::unique_ptr<wchar_t, decltype(&::LocalFree)> owned(text, &::LocalFree);
    EXPECT_EQ(std::wstring_view(owned.get()), TRUSTED_INSTALLER_SID);
    // The same SID every call: one constant, never rebuilt.
    EXPECT_EQ(trustedInstallerSid(), built);
}

// The predicate is false for what isn't a SID at all, and for SIDs one part away from TrustedInstaller's.
TEST(WindowsSecurityOwnersTest, OnlyTrustedInstallersOwnSidIsTrustedInstaller)
{
    EXPECT_FALSE(isTrustedInstaller(nullptr));
    alignas(SID) std::array<BYTE, SECURITY_SID_SIZE(6)> zeroed{}; // revision 0: no SID
    EXPECT_FALSE(isTrustedInstaller(zeroed.data()));
    for (const std::wstring_view almost : {
             L"S-1-5-80-956008885-3418522649-1831038044-1853292631",              // one sub-authority short
             L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464-1", // one more
             L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478465",   // the last one off by one
             L"S-1-5-81-956008885-3418522649-1831038044-1853292631-2271478464",   // not a service SID
             L"S-1-16-80-956008885-3418522649-1831038044-1853292631-2271478464",  // another authority
         })
    {
        const ConvertedSid sid = sidFrom(almost);
        ASSERT_NE(sid, nullptr) << GetLastError();
        EXPECT_FALSE(isTrustedInstaller(sid.get())) << std::string(almost.begin(), almost.end()); // SID text is ASCII
    }
    const ConvertedSid exact = sidFrom(TRUSTED_INSTALLER_SID);
    ASSERT_NE(exact, nullptr) << GetLastError();
    EXPECT_TRUE(isTrustedInstaller(exact.get()));
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
    EXPECT_FALSE(isTrustedInstaller(owner->sid));
}

// Each caller's set of trusted owners, over the same two real folders. TrustedInstaller is privileged to
// trusted by HandleRelativeFile's ownerIsSystemOrAdministratorsOrTrustedInstaller(), which secureChildFolder(),
// makeAdminDumpFolder() and ElevatedRestart's checkConfigDir() (through ConfigDirFunctions::ownerIsPrivileged, #4715) share
// (#4699: HandleRelativeFile's own check once left it out); the own
// config folder's ownerIsOwnAccountOrAdministrators() takes this account instead.
TEST(WindowsSecurityOwnersTest, EachCallerKeepsItsOwnSetOfTrustedOwners)
{
    const OwnFolder own;
    const UniqueHandle mine = openToRead(own.path());
    ASSERT_TRUE(mine) << GetLastError();
    EXPECT_EQ(ElevatedRestart::ConfigDirFunctions{}.ownerIsPrivileged(mine.get()), std::optional(false));
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(mine.get()), std::optional(false));
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(mine.get()), std::optional(true));

    const UniqueHandle windows = openToRead(windowsFolder());
    ASSERT_TRUE(windows) << GetLastError();
    const auto owner = ownerSidOf(windows.get());
    ASSERT_TRUE(owner.has_value()) << owner.error();
    ASSERT_TRUE(isTrustedInstaller(owner->sid)) << "the Windows folder is TrustedInstaller's";
    EXPECT_EQ(ElevatedRestart::ConfigDirFunctions{}.ownerIsPrivileged(windows.get()), std::optional(true));
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(windows.get()), std::optional(true));
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(windows.get()), std::optional(false));

    // And none of them reads a handle that can't be asked as trusted.
    EXPECT_EQ(ElevatedRestart::ConfigDirFunctions{}.ownerIsPrivileged(nullptr), std::nullopt);
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(nullptr), std::nullopt);
    EXPECT_EQ(ElevatedRestart::ownerIsOwnAccountOrAdministrators(nullptr), std::nullopt);
}

} // namespace
} // namespace Platform::Windows
