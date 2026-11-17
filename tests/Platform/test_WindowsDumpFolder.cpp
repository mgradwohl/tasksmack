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
#include <sddl.h> // ConvertSidToStringSidW, ConvertStringSidToSidW
// clang-format on

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
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

/// newFileReaders() of the descriptor @p sddl describes, for a token whose default owner is trusted where @p defaultOwnerTrusted.
[[nodiscard]] DumpFolderAccess readersOf(std::string_view sddl, bool defaultOwnerTrusted = true)
{
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(sddl);
    EXPECT_NE(descriptor, nullptr) << sddl;
    return newFileReaders(descriptor.get(), defaultOwnerTrusted);
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
    const DumpSource held{.captureSnapshot = nullptr, .liveReason = "test", .processMachine = nullptr, .holdFolder = true};
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
    const DumpSource held{.captureSnapshot = nullptr, .liveReason = "test", .processMachine = nullptr, .holdFolder = true};
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
    // Administrators, SYSTEM and TrustedInstaller only; CREATOR OWNER too when a new file's owner is Administrators.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf(std::format("O:SYD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;{})", TRUSTED_INSTALLER)), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)", false), DumpFolderAccess::ReadableByUser)
        << "CREATOR OWNER is the user when the token's default owner is";
    // A new file's own owner is the token's default owner, who always gets READ_CONTROL and WRITE_DAC: an
    // administrators-only folder is flagged when that isn't Administrators or SYSTEM.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", false), DumpFolderAccess::ReadableByUser);
    // Inherited control of a new file (WRITE_DAC, WRITE_OWNER) lets its holder grant itself a read; so does inherited
    // execute, which maps the file.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;WD;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;WO;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;FX;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;GX;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OIIO;GA;;;CO)(A;OIIO;WD;;;CO)"), DumpFolderAccess::AdministratorsOnly)
        << "CREATOR OWNER is the trusted owner";
    // An object ACE: any inherited read or control is flagged, whatever its SID.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(OA;OIIO;WD;bf967aba-0de6-11d0-a285-00aa003049e2;;BA)"), DumpFolderAccess::ReadableByUser);
    // Any other SID with an inherited read: the well-known groups, an unknown user (the one who didn't type the
    // credentials), and inherit-only ACEs, which still reach a new file.
    for (const std::string_view sid : {"BU", "WD", "AU", "IU", "BG", "S-1-5-21-1000000001-1000000002-1000000003-1001"})
    {
        EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;BA)(A;OI;FR;;;{})", sid)), DumpFolderAccess::ReadableByUser) << sid;
    }
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GR;;;AU)"), DumpFolderAccess::ReadableByUser);
    // The list right is the read right's bit: flagged too, conservatively.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;0x1;;;BU)"), DumpFolderAccess::ReadableByUser);
    // Not inherited by a new file, no read, or a deny: not flagged for reading.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;FR;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CI;FR;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;FW;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(D;OICI;FA;;;BU)(A;OICI;FA;;;BA)"), DumpFolderAccess::AdministratorsOnly);
    // Control of the folder itself (WRITE_DAC, WRITE_OWNER or GENERIC_ALL) for anyone else: they could grant themselves a read.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;WD;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;BA)(A;;WO;;;{})", OTHER_USER)), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CI;GA;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CIIO;WD;;;BU)"), DumpFolderAccess::AdministratorsOnly)
        << "subfolders only, not this one";
    // An owner other than Administrators or SYSTEM, even with an administrators-only DACL.
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", OTHER_USER)), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", TRUSTED_INSTALLER)), DumpFolderAccess::ReadableByUser);
    // No DACL at all: everyone has every access. No allow ACE a new file inherits: its DACL is the token's, unread.
    EXPECT_EQ(readersOf("O:BAD:NO_ACCESS_CONTROL"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;;FA;;;BA)(A;CI;FA;;;SY)"), DumpFolderAccess::Unknown);
    EXPECT_EQ(readersOf("O:BAD:P"), DumpFolderAccess::Unknown);
    EXPECT_EQ(newFileReaders(nullptr, true), DumpFolderAccess::Unknown);
}

// An allow ACE type the check doesn't read (a compound ACE here) fails closed: Unknown, never AdministratorsOnly.
TEST(WindowsDumpFolderTest, AnAllowAceTypeItDoesntReadIsUnknown)
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
    allowed->Mask = FILE_GENERIC_WRITE; // no read: still Unknown, as the type isn't read at all
    ASSERT_NE(CopySid(sidSize, &allowed->SidStart, users.get()), FALSE);
    std::vector<BYTE> aclBuffer(sizeof(ACL) + aceSize + 64);
    auto* acl = reinterpret_cast<PACL>(aclBuffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    ASSERT_NE(InitializeAcl(acl, static_cast<DWORD>(aclBuffer.size()), ACL_REVISION3), FALSE) << GetLastError();
    ASSERT_NE(AddAce(acl, ACL_REVISION3, MAXDWORD, ace.data(), aceSize), FALSE) << GetLastError();
    SECURITY_DESCRIPTOR descriptor{};
    ASSERT_NE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION), FALSE);
    ASSERT_NE(SetSecurityDescriptorOwner(&descriptor, admins.get(), FALSE), FALSE);
    ASSERT_NE(SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE), FALSE);
    EXPECT_EQ(newFileReaders(&descriptor, true), DumpFolderAccess::Unknown);
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
    const std::string usersReadDacl = "D:P(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)";
    const std::filesystem::path adminsOnly = scratch.path() / L"admins-only";
    const std::filesystem::path usersRead = scratch.path() / L"users-read";
    ASSERT_TRUE(makeFolderWith(adminsOnly, std::format("O:{0}D:P(A;;FA;;;{0})(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", me))) << GetLastError();
    ASSERT_TRUE(makeFolderWith(usersRead, std::format("O:{0}D:P(A;;FA;;;{0})(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)", me))) << GetLastError();

    auto heldAdmins = openHeldDumpFolder(adminsOnly);
    ASSERT_TRUE(heldAdmins.has_value()) << heldAdmins.error();
    EXPECT_EQ(heldFolderReaders(heldAdmins->get(), true), DumpFolderAccess::ReadableByUser) << "owned by this account";
    auto heldUsers = openHeldDumpFolder(usersRead);
    ASSERT_TRUE(heldUsers.has_value()) << heldUsers.error();
    EXPECT_EQ(heldFolderReaders(heldUsers->get(), true), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(checkDumpFolder(usersRead).access, DumpFolderAccess::ReadableByUser);
    EXPECT_TRUE(checkDumpFolder(usersRead).problem.empty());

    // The same DACLs with Administrators as the owner, as an elevated TaskSmack would make them: Users is flagged, and
    // the administrators-only folder passes.
    EXPECT_EQ(readersOf("O:BA" + usersReadDacl), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BA" + adminsOnlyDacl), DumpFolderAccess::AdministratorsOnly);
}
} // namespace Platform::Windows
