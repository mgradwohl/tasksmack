/// @file test_WindowsDumpFolder.cpp
/// @brief An elevated dump's held folder (#4164): a junction swapped in after the pick is refused, the file is made
/// relative to the held handle, and the folder-access check reads the owner and DACL. On temp folders of the test's own,
/// with explicit SDDL; nothing here elevates or touches a real user folder.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/WindowsDumpFolder.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessDump.h"
#include "Support/Junction.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h> // GetNamedSecurityInfoW
#include <sddl.h>   // ConvertSidToStringSidW, ConvertStringSidToSidW
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// A folder of this test's own under the temp folder, as Windows names it (canonical, so its final path matches);
/// removed at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() /
                 std::format(L"TaskSmack.DumpFolderTest.{}.{}", GetCurrentProcessId(), GetTickCount64()))
    {
        std::error_code error;
        std::filesystem::create_directories(m_Path, error);
        EXPECT_FALSE(error) << error.message();
        if (std::filesystem::path real = std::filesystem::canonical(m_Path, error); !error)
        {
            m_Path = std::move(real);
        }
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~ScratchFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ScratchFolder(ScratchFolder&&) = delete;
    ScratchFolder& operator=(ScratchFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// A SID of no account on this machine: a user the allowlist doesn't know.
constexpr std::string_view OTHER_USER = "S-1-5-21-1000000001-1000000002-1000000003-1001";
/// TrustedInstaller's service SID.
constexpr std::string_view TRUSTED_INSTALLER = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
/// A held folder's dump file security this unelevated test can set (#4188): ADMIN_DUMP_FILE_SECURITY's protected DACL,
/// with OWNER RIGHTS so the file's owner (this account) can still remove it, and no owner, which only an elevated token may
/// make Administrators.
constexpr std::string_view TEST_FILE_SECURITY = "D:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;FA;;;OW)";
/// The default dump folder's security this unelevated test can set (#4188): ADMIN_DUMP_FOLDER_SECURITY's, with
/// Authenticated Users so the test can work in and remove what it made, and no owner.
constexpr std::string_view TEST_FOLDER_SECURITY = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;AU)";

/// The SID @p text describes (an SDDL alias such as "BU", or S-1-...), LocalFree'd; null when it doesn't parse.
[[nodiscard]] std::unique_ptr<void, decltype(&::LocalFree)> sidFrom(std::string_view text)
{
    PSID sid = nullptr;
    const std::wstring wide(text.begin(), text.end());
    if (ConvertStringSidToSidW(wide.c_str(), &sid) == FALSE)
    {
        return {nullptr, &::LocalFree};
    }
    return {sid, &::LocalFree};
}
/// This test's own account (its token user) as an SDDL SID string; empty when it can't be read.
[[nodiscard]] std::string ownSidText()
{
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE)
    {
        return {};
    }
    const UniqueHandle token(rawToken);
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    std::vector<BYTE> buffer(size);
    if (size == 0 || GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return {};
    }
    LPWSTR text = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - GetTokenInformation fills a TOKEN_USER
    if (ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &text) == FALSE)
    {
        return {};
    }
    const std::unique_ptr<wchar_t, decltype(&::LocalFree)> owned(text, &::LocalFree);
    std::string narrow;
    for (const wchar_t* at = owned.get(); *at != L'\0'; ++at)
    {
        narrow.push_back(static_cast<char>(*at)); // a SID string is ASCII
    }
    return narrow;
}

/// folderReaders() of the descriptor @p sddl describes.
[[nodiscard]] DumpFolderAccess readersOf(std::string_view sddl)
{
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(sddl);
    EXPECT_NE(descriptor, nullptr) << sddl;
    return folderReaders(descriptor.get());
}
/// Folder @p folder made with the security descriptor @p sddl.
[[nodiscard]] bool makeFolderWith(const std::filesystem::path& folder, std::string_view sddl)
{
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(sddl);
    if (descriptor == nullptr)
    {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
        .bInheritHandle = FALSE,
    };
    return CreateDirectoryW(folder.c_str(), &attributes) != FALSE;
}

[[nodiscard]] bool isEmptyFolder(const std::filesystem::path& folder)
{
    std::error_code error;
    return std::filesystem::is_empty(folder, error) && !error;
}

} // namespace

TEST(WindowsDumpFolderTest, APlainFolderIsHeldAtItsOwnPath)
{
    const ScratchFolder scratch;
    auto held = openHeldDumpFolder(scratch.path());
    ASSERT_TRUE(held.has_value()) << held.error();
    EXPECT_EQ(finalPathOf(held->get()), std::optional(scratch.path().native()));
    // A trailing separator names the same folder.
    EXPECT_TRUE(openHeldDumpFolder(scratch.path().native() + L"\\").has_value());

    EXPECT_FALSE(openHeldDumpFolder({}).has_value());
    EXPECT_FALSE(openHeldDumpFolder(L"relative\\dumps").has_value()) << "only an absolute folder";
    const auto missing = openHeldDumpFolder(scratch.path() / L"absent");
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(missing.error().contains("can't be opened")) << missing.error();
}

TEST(WindowsDumpFolderTest, SamePathTextIgnoresCaseOnly)
{
    EXPECT_TRUE(samePathText(LR"(C:\Dumps)", LR"(c:\dUMPS)"));
    EXPECT_FALSE(samePathText(LR"(C:\Dumps)", LR"(C:\Dumps2)"));
    EXPECT_FALSE(samePathText(LR"(C:\Dumps)", LR"(D:\Dumps)"));
}

// The issue's attack (#4164): the folder picked is swapped for a junction before the dump is written. It is opened
// itself, never followed, and refused; nothing is written where it points.
TEST(WindowsDumpFolderTest, AJunctionSwappedInAfterThePickIsRefused)
{
    const ScratchFolder scratch;
    const std::filesystem::path picked = scratch.path() / L"picked";
    const std::filesystem::path elsewhere = scratch.path() / L"elsewhere";
    ASSERT_TRUE(std::filesystem::create_directory(picked));
    ASSERT_TRUE(std::filesystem::create_directory(elsewhere));
    EXPECT_TRUE(openHeldDumpFolder(picked).has_value()) << "a plain folder when it was picked";

    // Swapped: the folder is now an empty one turned into a junction to another.
    std::filesystem::remove(picked);
    ASSERT_TRUE(std::filesystem::create_directory(picked));
    const std::optional<std::string> notMade = TestSupport::makeJunction(picked, elsewhere);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});

    const auto refused = openHeldDumpFolder(picked);
    ASSERT_FALSE(refused.has_value()) << "the junction was taken for the folder";
    EXPECT_TRUE(refused.error().contains("is a junction, link or other reparse point")) << refused.error();
    EXPECT_EQ(checkDumpFolder(picked).access, DumpFolderAccess::Refused);
    EXPECT_EQ(checkDumpFolder(picked).problem, refused.error());

    // The dump itself refuses it, before anything is made: this process, through a handle that can only be waited on.
    const UniqueHandle waitOnly(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    ASSERT_TRUE(waitOnly);
    const DumpSource held{
        .captureSnapshot = nullptr,
        .liveReason = "test",
        .processMachine = nullptr,
        .holdFolder = true,
        .fileSecurity = TEST_FILE_SECURITY,
    };
    const ProcessDumpResult result =
        writeProcessDump(waitOnly.get(), static_cast<std::int32_t>(GetCurrentProcessId()), picked, std::stop_token{}, held);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), refused.error());
    EXPECT_TRUE(isEmptyFolder(elsewhere)) << "nothing written behind the junction";

    ASSERT_TRUE(TestSupport::removeJunction(picked)) << "an empty folder again, so removing it removes nothing behind it";
}

// A junction higher up the path: the folder is plain, but not where its path says, so it is refused too.
TEST(WindowsDumpFolderTest, AJunctionOnThePathIsRefused)
{
    const ScratchFolder scratch;
    const std::filesystem::path real = scratch.path() / L"real";
    const std::filesystem::path link = scratch.path() / L"link";
    ASSERT_TRUE(std::filesystem::create_directories(real / L"dumps"));
    ASSERT_TRUE(std::filesystem::create_directory(link));
    const std::optional<std::string> notMade = TestSupport::makeJunction(link, real);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});

    const auto refused = openHeldDumpFolder(link / L"dumps");
    ASSERT_FALSE(refused.has_value()) << "followed the junction on its path";
    EXPECT_TRUE(refused.error().contains("isn't where its path says")) << refused.error();
    EXPECT_TRUE(openHeldDumpFolder(real / L"dumps").has_value()) << "its own path is fine";

    ASSERT_TRUE(TestSupport::removeJunction(link));
}

// The file is made relative to the held folder, and a dump that doesn't finish is deleted through its own handle: a
// cancel already requested leaves the folder empty.
TEST(WindowsDumpFolderTest, TheFileIsMadeInTheHeldFolderAndACancelLeavesNone)
{
    const ScratchFolder scratch;
    const UniqueHandle waitOnly(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    ASSERT_TRUE(waitOnly);
    const DumpSource held{
        .captureSnapshot = nullptr,
        .liveReason = "test",
        .processMachine = nullptr,
        .holdFolder = true,
        .fileSecurity = TEST_FILE_SECURITY,
    };
    std::stop_source stop;
    stop.request_stop();
    const ProcessDumpResult result =
        writeProcessDump(waitOnly.get(), static_cast<std::int32_t>(GetCurrentProcessId()), scratch.path(), stop.get_token(), held);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().starts_with("Cancelled")) << result.error() << ": the file was made, then the cancel deleted it";
    EXPECT_TRUE(isEmptyFolder(scratch.path()));
}

TEST(WindowsDumpFolderTest, AnAllowlistTrustsOnlyAdministratorsSystemAndTrustedInstaller)
{
    // Administrators, SYSTEM and TrustedInstaller only; CREATOR OWNER in an inherit-only ACE (whoever makes a subfolder).
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf(std::format("O:SYD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;{})", TRUSTED_INSTALLER)), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)"), DumpFolderAccess::AdministratorsOnly);
    // Any other SID that can list the folder, open its files (traverse) or read it: the well-known groups, and an
    // unknown user (the one who didn't type the credentials).
    for (const std::string_view sid : {"BU", "WD", "AU", "IU", "BG", "S-1-5-21-1000000001-1000000002-1000000003-1001"})
    {
        EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;BA)(A;;FR;;;{})", sid)), DumpFolderAccess::ReadableByUser) << sid;
    }
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;0x1;;;BU)"), DumpFolderAccess::ReadableByUser) << "FILE_LIST_DIRECTORY";
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;0x20;;;BU)"), DumpFolderAccess::ReadableByUser) << "FILE_TRAVERSE";
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;GR;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;GX;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;GA;;;BU)"), DumpFolderAccess::ReadableByUser);
    // Control of the folder itself (WRITE_DAC, WRITE_OWNER) for anyone else: they could grant themselves the rest.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;WD;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;BA)(A;;WO;;;{})", OTHER_USER)), DumpFolderAccess::ReadableByUser);
    // A container-inherit ACE reaches the folders made in it, inherit-only or not.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CIIO;FR;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CIIO;WD;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GR;;;AU)"), DumpFolderAccess::ReadableByUser);
    // An object ACE: any such right, on the folder or its subfolders, is flagged whatever its SID.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(OA;CIIO;WD;bf967aba-0de6-11d0-a285-00aa003049e2;;BA)"), DumpFolderAccess::ReadableByUser);
    // What the hint doesn't measure, now the dump file is admin-only (#4188): what only files inherit (the other files
    // made there; never TaskSmack's own), rights that neither list, traverse, read nor control, and denies.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;FR;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;WD;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;FW;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;0x2;;;BU)"), DumpFolderAccess::AdministratorsOnly) << "FILE_ADD_FILE alone";
    EXPECT_EQ(readersOf("O:BAD:P(D;OICI;FA;;;BU)(A;OICI;FA;;;BA)"), DumpFolderAccess::AdministratorsOnly);
    // An owner other than Administrators or SYSTEM, even with an administrators-only DACL.
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", OTHER_USER)), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", TRUSTED_INSTALLER)), DumpFolderAccess::ReadableByUser);
    // No DACL at all: everyone has every access. An empty DACL, or one with no inheritable ACE, grants no one else
    // anything. Unknown is only an unreadable descriptor.
    EXPECT_EQ(readersOf("O:BAD:NO_ACCESS_CONTROL"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;;FA;;;BA)(A;CI;FA;;;SY)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("D:P(A;;FA;;;BA)"), DumpFolderAccess::Unknown) << "no owner to read";
    EXPECT_EQ(folderReaders(nullptr), DumpFolderAccess::Unknown);
}

// An allow ACE type the check doesn't read (a compound ACE here) fails toward the hint: ReadableByUser, never
// AdministratorsOnly. Unknown stays for a descriptor that can't be read.
TEST(WindowsDumpFolderTest, AnAllowAceTypeItDoesntReadIsFlagged)
{
    const auto users = sidFrom("BU");
    const auto admins = sidFrom("BA");
    ASSERT_NE(users, nullptr);
    ASSERT_NE(admins, nullptr);
    const DWORD sidSize = GetLengthSid(users.get());
    // ACCESS_ALLOWED_ACE's layout under another type: header, mask, then the SID.
    const DWORD aceSize = static_cast<DWORD>(offsetof(ACCESS_ALLOWED_ACE, SidStart)) + sidSize;
    std::vector<BYTE> ace(aceSize);
    auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    allowed->Header.AceType = ACCESS_ALLOWED_COMPOUND_ACE_TYPE;
    allowed->Header.AceFlags = OBJECT_INHERIT_ACE;
    allowed->Header.AceSize = static_cast<WORD>(aceSize);
    allowed->Mask = FILE_GENERIC_WRITE; // nothing the hint measures: still flagged, as the type isn't read at all
    ASSERT_NE(CopySid(sidSize, &allowed->SidStart, users.get()), FALSE);
    std::vector<BYTE> aclBuffer(sizeof(ACL) + aceSize + 64);
    auto* acl = reinterpret_cast<PACL>(aclBuffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    ASSERT_NE(InitializeAcl(acl, static_cast<DWORD>(aclBuffer.size()), ACL_REVISION3), FALSE) << GetLastError();
    ASSERT_NE(AddAce(acl, ACL_REVISION3, MAXDWORD, ace.data(), aceSize), FALSE) << GetLastError();
    SECURITY_DESCRIPTOR descriptor{};
    ASSERT_NE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION), FALSE);
    ASSERT_NE(SetSecurityDescriptorOwner(&descriptor, admins.get(), FALSE), FALSE);
    ASSERT_NE(SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE), FALSE);
    EXPECT_EQ(folderReaders(&descriptor), DumpFolderAccess::ReadableByUser);
}
// The reviewer's case (#4164): a standard user elevates with an administrator's credentials and picks their own
// Documents. The folder's ACEs name only SYSTEM, Administrators and that user, whom no denylist would know.
TEST(WindowsDumpFolderTest, AnotherUsersOwnFolderIsFlaggedThoughItNamesNoWellKnownGroup)
{
    EXPECT_EQ(readersOf(std::format("O:{0}D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;{0})", OTHER_USER)),
              DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;{})", OTHER_USER)),
              DumpFolderAccess::ReadableByUser)
        << "flagged by its ACE alone, even owned by Administrators";
}

TEST(WindowsDumpFolderTest, OnlyACloudFilesReparsePointIsAcceptedForADump)
{
    constexpr DWORD FOLDER = FILE_ATTRIBUTE_DIRECTORY;
    constexpr DWORD REPARSE_FOLDER = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    EXPECT_TRUE(acceptedDumpFolder(FOLDER, 0));
    EXPECT_FALSE(acceptedDumpFolder(FILE_ATTRIBUTE_NORMAL, 0)) << "a file";
    EXPECT_TRUE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_CLOUD));
    EXPECT_TRUE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_CLOUD_1));
    EXPECT_TRUE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_CLOUD_F));
    EXPECT_FALSE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_MOUNT_POINT)) << "a junction";
    EXPECT_FALSE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_SYMLINK));
    EXPECT_FALSE(acceptedDumpFolder(REPARSE_FOLDER, IO_REPARSE_TAG_APPEXECLINK));
    EXPECT_FALSE(acceptedDumpFolder(REPARSE_FOLDER, 0)) << "an unknown tag";
}

// Real folders, made with explicit SDDL and an explicit owner (this test's account, which can't make Administrators
// the owner unelevated), so the result doesn't depend on the runner's token. As the allowlist says, both are flagged:
// this account owns them, and it keeps full control of them so it can remove them. The Users folder is flagged even
// when the owner is set aside, by its inherited read; the administrators-only DACL passes the same read.
TEST(WindowsDumpFolderTest, TheCheckReadsARealFoldersOwnerAndDacl)
{
    const ScratchFolder scratch;
    const std::string me = ownSidText();
    ASSERT_FALSE(me.empty());
    const std::string adminsOnlyDacl = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";
    const std::string usersReadDacl = "D:P(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)"; // Users may list it
    const std::filesystem::path adminsOnly = scratch.path() / L"admins-only";
    const std::filesystem::path usersRead = scratch.path() / L"users-read";
    ASSERT_TRUE(makeFolderWith(adminsOnly, std::format("O:{0}D:P(A;;FA;;;{0})(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", me))) << GetLastError();
    ASSERT_TRUE(makeFolderWith(usersRead, std::format("O:{0}D:P(A;;FA;;;{0})(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)", me))) << GetLastError();

    auto heldAdmins = openHeldDumpFolder(adminsOnly);
    ASSERT_TRUE(heldAdmins.has_value()) << heldAdmins.error();
    EXPECT_EQ(heldFolderReaders(heldAdmins->get()), DumpFolderAccess::ReadableByUser) << "owned by this account";
    auto heldUsers = openHeldDumpFolder(usersRead);
    ASSERT_TRUE(heldUsers.has_value()) << heldUsers.error();
    EXPECT_EQ(heldFolderReaders(heldUsers->get()), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(checkDumpFolder(usersRead).access, DumpFolderAccess::ReadableByUser);
    EXPECT_TRUE(checkDumpFolder(usersRead).problem.empty());

    // The same DACLs with Administrators as the owner, as an elevated TaskSmack would make them: Users is flagged, and
    // the administrators-only folder passes.
    EXPECT_EQ(readersOf("O:BA" + usersReadDacl), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BA" + adminsOnlyDacl), DumpFolderAccess::AdministratorsOnly);
}

namespace
{
/// The string SID of @p sid; empty when it can't be converted.
[[nodiscard]] std::wstring sidText(PSID sid)
{
    LPWSTR text = nullptr;
    if (sid == nullptr || ConvertSidToStringSidW(sid, &text) == FALSE)
    {
        return {};
    }
    const std::unique_ptr<wchar_t, decltype(&::LocalFree)> owned(text, &::LocalFree);
    return {owned.get()};
}

/// An allow ACE as read back: its SID, flags and mask.
struct ReadAce
{
    std::wstring sid;
    BYTE flags = 0;
    ACCESS_MASK mask = 0;
};

/// @p descriptor's owner, whether its DACL is protected, and its ACEs (allow ACEs only; anything else is recorded with an
/// empty SID).
struct ReadSecurity
{
    std::wstring owner;
    bool daclProtected = false;
    std::vector<ReadAce> aces;
};

[[nodiscard]] ReadSecurity readSecurity(PSECURITY_DESCRIPTOR descriptor)
{
    ReadSecurity read;
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    if (GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) != FALSE)
    {
        read.owner = sidText(owner);
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE)
    {
        read.daclProtected = (control & SE_DACL_PROTECTED) != 0;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE || present == FALSE || dacl == nullptr)
    {
        return read;
    }
    for (DWORD index = 0; index < dacl->AceCount; ++index)
    {
        void* ace = nullptr;
        if (GetAce(dacl, index, &ace) == FALSE)
        {
            break;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        ReadAce entry{.sid = {}, .flags = header->AceFlags, .mask = 0};
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
        {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            entry.sid = sidText(&allowed->SidStart);
            entry.mask = allowed->Mask;
        }
        read.aces.push_back(std::move(entry));
    }
    return read;
}

/// The security of the file or folder at @p path, as Windows has it.
[[nodiscard]] ReadSecurity readSecurityOf(const std::filesystem::path& path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(),
                              SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              &descriptor) != ERROR_SUCCESS)
    {
        return {};
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> owned(descriptor, &::LocalFree);
    return readSecurity(descriptor);
}

constexpr std::wstring_view ADMINISTRATORS_SID = L"S-1-5-32-544";
constexpr std::wstring_view SYSTEM_SID = L"S-1-5-18";
constexpr std::wstring_view AUTHENTICATED_USERS_SID = L"S-1-5-11";

/// makeAdminDumpFolder()'s options for this unelevated test (TEST_FOLDER_SECURITY), trusting a folder there already as
/// an Administrators-owned one TaskSmack made would be, so a second call takes it over in place, and the scratch
/// "ProgramData", which this account owns, as the real one's SYSTEM owner is.
[[nodiscard]] AdminDumpFolderOptions testFolderOptions()
{
    return {
        .folderSecurity = std::string(TEST_FOLDER_SECURITY),
        .ownerTrusted = [](HANDLE /*folder*/) { return std::optional(true); },
        .rootOwnerTrusted = [](HANDLE /*folder*/) { return std::optional(true); },
    };
}

/// Sets the DACL of @p path to the one @p sddl describes, unprotected (so it also inherits from its parent); false when
/// Windows refuses.
[[nodiscard]] bool setUnprotectedDaclOf(const std::filesystem::path& path, std::string_view sddl)
{
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(sddl);
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (descriptor == nullptr || GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted) == FALSE || dacl == nullptr)
    {
        return false;
    }
    std::wstring name = path.native(); // SetNamedSecurityInfoW takes a non-const name
    return SetNamedSecurityInfoW(name.data(),
                                 SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr,
                                 nullptr,
                                 dacl,
                                 nullptr) == ERROR_SUCCESS;
}
} // namespace

// #4188: an elevated dump file is admin-only in any folder: owned by Administrators, with a protected DACL for
// Administrators and SYSTEM only, nothing inherited. The default folder's security is the elevated log's, and it passes
// the folder check as AdministratorsOnly, so its confirm has no warning.
TEST(WindowsDumpFolderTest, TheDumpFileAndDefaultFolderAreAdministratorsAndSystemOnly)
{
    EXPECT_EQ(ADMIN_DUMP_FILE_SECURITY, "O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)");
    EXPECT_EQ(ADMIN_DUMP_FOLDER_SECURITY, "O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");

    const OwnedSecurityDescriptor file = securityDescriptorFrom(ADMIN_DUMP_FILE_SECURITY);
    ASSERT_NE(file, nullptr);
    const ReadSecurity read = readSecurity(file.get());
    EXPECT_EQ(read.owner, ADMINISTRATORS_SID);
    EXPECT_TRUE(read.daclProtected) << "nothing inherited from the folder";
    ASSERT_EQ(read.aces.size(), 2U);
    EXPECT_EQ(read.aces[0].sid, ADMINISTRATORS_SID);
    EXPECT_EQ(read.aces[1].sid, SYSTEM_SID);
    for (const ReadAce& ace : read.aces)
    {
        EXPECT_EQ(ace.mask, static_cast<ACCESS_MASK>(FILE_ALL_ACCESS));
        EXPECT_EQ(ace.flags, 0) << "a file's ACEs: not inherited, nothing to pass on";
    }

    // The folder check reads only the folder's own owner and DACL, never the token's default owner: the default folder
    // passes, so its confirm has no hint, whoever runs it.
    EXPECT_EQ(readersOf(ADMIN_DUMP_FOLDER_SECURITY), DumpFolderAccess::AdministratorsOnly) << "no warning for the default folder";
}

// #4188: a held folder's dump file gets the security it is given, not the folder's: read back from a real file made in
// a folder whose inheritable ACEs would give Users a read.
TEST(WindowsDumpFolderTest, TheHeldFoldersDumpFileGetsItsOwnProtectedDacl)
{
    const ScratchFolder scratch;
    const std::string me = ownSidText();
    ASSERT_FALSE(me.empty());
    const std::filesystem::path folder = scratch.path() / L"users-read";
    ASSERT_TRUE(makeFolderWith(folder, std::format("O:{0}D:P(A;OICI;FA;;;{0})(A;OICI;FR;;;BU)", me))) << GetLastError();

    // A file made in the held folder with the dump's security, as createDumpFileInHeldFolder() makes it.
    auto held = openHeldDumpFolder(folder);
    ASSERT_TRUE(held.has_value()) << held.error();
    const OwnedSecurityDescriptor security = securityDescriptorFrom(TEST_FILE_SECURITY);
    ASSERT_NE(security, nullptr);
    {
        auto made = openRelative(held->get(),
                                 L"made.dmp",
                                 RelativeOpen{
                                     .access = GENERIC_WRITE | SYNCHRONIZE,
                                     .share = 0,
                                     .disposition = FILE_CREATE,
                                     .kind = EntryKind::File,
                                     .security = security.get(),
                                 });
        ASSERT_TRUE(made.has_value()) << ntStatusText(made.error());
    }
    const ReadSecurity read = readSecurityOf(folder / L"made.dmp");
    EXPECT_TRUE(read.daclProtected);
    ASSERT_EQ(read.aces.size(), 3U) << "the folder's Users read wasn't inherited";
    EXPECT_EQ(read.aces[0].sid, ADMINISTRATORS_SID);
    EXPECT_EQ(read.aces[1].sid, SYSTEM_SID);
    EXPECT_EQ(read.aces[2].sid, L"S-1-3-4") << "OWNER RIGHTS, so this test can remove it";
    for (const ReadAce& ace : read.aces)
    {
        EXPECT_EQ(ace.flags & INHERITED_ACE, 0);
        EXPECT_EQ(ace.mask, static_cast<ACCESS_MASK>(FILE_ALL_ACCESS));
    }
}

// #4188: the elevated default folder, made in a scratch "ProgramData": TaskSmack\Dumps, each with the given protected
// DACL, held by its final path; a second call takes it over in place; and a junction planted for it is moved aside
// rather than followed.
TEST(WindowsDumpFolderTest, TheDefaultFolderIsMadeUnderProgramDataWithItsOwnSecurity)
{
    const ScratchFolder scratch;
    const std::filesystem::path programData = scratch.path() / L"ProgramData";
    ASSERT_TRUE(std::filesystem::create_directory(programData));

    const auto made = makeAdminDumpFolder(programData, testFolderOptions());
    ASSERT_TRUE(made.has_value()) << made.error();
    EXPECT_EQ(*made, programData / L"TaskSmack" / L"Dumps");
    for (const std::filesystem::path& folder : {programData / L"TaskSmack", *made})
    {
        const ReadSecurity read = readSecurityOf(folder);
        EXPECT_TRUE(read.daclProtected) << folder.string() << ": nothing inherited from ProgramData";
        ASSERT_EQ(read.aces.size(), 3U) << folder.string();
        EXPECT_EQ(read.aces[0].sid, ADMINISTRATORS_SID);
        EXPECT_EQ(read.aces[1].sid, SYSTEM_SID);
        EXPECT_EQ(read.aces[2].sid, AUTHENTICATED_USERS_SID);
    }
    // The dump's own open takes it: a plain folder at its own path. Authenticated Users, which only this test adds, is
    // flagged; the real security passes (TheDumpFileAndDefaultFolderAreAdministratorsAndSystemOnly).
    EXPECT_TRUE(openHeldDumpFolder(*made).has_value());
    EXPECT_EQ(checkDumpFolder(*made).access, DumpFolderAccess::ReadableByUser);

    // Taken over in place: a DACL widened since, unprotected and with a Users read, is reset to the given one.
    ASSERT_TRUE(setUnprotectedDaclOf(*made, "D:(A;OICI;FA;;;AU)(A;;FR;;;BU)"));
    EXPECT_FALSE(readSecurityOf(*made).daclProtected);
    const auto again = makeAdminDumpFolder(programData, testFolderOptions());
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_EQ(*again, *made);
    {
        const ReadSecurity reset = readSecurityOf(*again);
        EXPECT_TRUE(reset.daclProtected) << "the takeover protects its DACL again";
        ASSERT_EQ(reset.aces.size(), 3U) << "the Users read is gone";
        EXPECT_EQ(reset.aces[0].sid, ADMINISTRATORS_SID);
        EXPECT_EQ(reset.aces[1].sid, SYSTEM_SID);
        EXPECT_EQ(reset.aces[2].sid, AUTHENTICATED_USERS_SID);
    }

    // A junction where Dumps should be: not a plain folder, so moved aside even though its owner is trusted, and a fresh
    // folder made; nothing is written where it points.
    const std::filesystem::path elsewhere = scratch.path() / L"elsewhere";
    ASSERT_TRUE(std::filesystem::create_directory(elsewhere));
    std::filesystem::remove(*made);
    ASSERT_TRUE(std::filesystem::create_directory(*made));
    const std::optional<std::string> notMade = TestSupport::makeJunction(*made, elsewhere);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});
    const auto remade = makeAdminDumpFolder(programData, testFolderOptions());
    ASSERT_TRUE(remade.has_value()) << remade.error();
    EXPECT_EQ(*remade, *made);
    EXPECT_TRUE(openHeldDumpFolder(*remade).has_value()) << "a plain folder again, not the junction";
    EXPECT_TRUE(isEmptyFolder(elsewhere));
    for (const auto& entry : std::filesystem::directory_iterator(programData / L"TaskSmack"))
    {
        if (entry.path().filename().native().starts_with(L"Dumps.untrusted-"))
        {
            EXPECT_TRUE(TestSupport::removeJunction(entry.path())) << "the moved-aside junction";
        }
    }
}

TEST(WindowsDumpFolderTest, TheDefaultFolderIsRefusedWithoutAPlainProgramData)
{
    const ScratchFolder scratch;
    EXPECT_FALSE(makeAdminDumpFolder({}, testFolderOptions()).has_value());
    EXPECT_FALSE(makeAdminDumpFolder(L"relative\\ProgramData", testFolderOptions()).has_value());
    const auto missing = makeAdminDumpFolder(scratch.path() / L"absent", testFolderOptions());
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(missing.error().contains("can't be opened")) << missing.error();
    AdminDumpFolderOptions badSecurity = testFolderOptions();
    badSecurity.folderSecurity = "not SDDL";
    const auto unbuilt = makeAdminDumpFolder(scratch.path(), badSecurity);
    ASSERT_FALSE(unbuilt.has_value());
    EXPECT_TRUE(unbuilt.error().contains("security")) << unbuilt.error();

    // A junction for ProgramData itself is refused, not followed.
    const std::filesystem::path real = scratch.path() / L"real";
    const std::filesystem::path link = scratch.path() / L"link";
    ASSERT_TRUE(std::filesystem::create_directory(real));
    ASSERT_TRUE(std::filesystem::create_directory(link));
    const std::optional<std::string> notMade = TestSupport::makeJunction(link, real);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});
    const auto refused = makeAdminDumpFolder(link, testFolderOptions());
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().contains("isn't a plain folder")) << refused.error();
    EXPECT_TRUE(isEmptyFolder(real));
    ASSERT_TRUE(TestSupport::removeJunction(link));
}

// #4188 review: %ProgramData% is the trust root, so its owner must be SYSTEM, TrustedInstaller or Administrators. A
// scratch one this account owns is refused by the real check, and nothing is made in it.
TEST(WindowsDumpFolderTest, TheDefaultFolderIsRefusedUnderAProgramDataOwnedByAUser)
{
    const ScratchFolder scratch;
    // Owned by this account explicitly, which any token may set: an elevated runner's own folders would otherwise be
    // owned by Administrators (its default owner), which the check trusts.
    const std::string me = ownSidText();
    ASSERT_FALSE(me.empty());
    const std::filesystem::path userOwned = scratch.path() / L"ProgramData";
    ASSERT_TRUE(makeFolderWith(userOwned, std::format("O:{0}D:P(A;OICI;FA;;;{0})", me))) << GetLastError();
    AdminDumpFolderOptions realRootCheck = testFolderOptions();
    realRootCheck.rootOwnerTrusted = {};
    const auto refused = makeAdminDumpFolder(userOwned, realRootCheck);
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().contains("isn't owned by SYSTEM, TrustedInstaller or Administrators")) << refused.error();
    EXPECT_TRUE(isEmptyFolder(userOwned));

    const auto ownScratch = openFolderByPath(userOwned, READ_CONTROL);
    ASSERT_TRUE(ownScratch.has_value()) << ownScratch.error();
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(ownScratch->get()), std::optional(false));
    // The Windows folder is TrustedInstaller's: trusted. Read only, never written.
    std::array<wchar_t, MAX_PATH> windows{};
    ASSERT_NE(GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())), 0U);
    const auto windowsFolder = openFolderByPath(std::filesystem::path(windows.data()), READ_CONTROL);
    ASSERT_TRUE(windowsFolder.has_value()) << windowsFolder.error();
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(windowsFolder->get()), std::optional(true));
}
} // namespace Platform::Windows
