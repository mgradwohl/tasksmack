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
#include <sddl.h> // ConvertSidToStringSidW
// clang-format on

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

/// A SID of no account on this machine, standing in for the signed-in user where this test's own account must not be.
constexpr std::string_view OTHER_USER = "S-1-5-21-1000000001-1000000002-1000000003-1001";

/// The SID @p text describes (LocalFree'd), or null.
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

/// This test's own account as an SDDL SID string.
[[nodiscard]] std::string ownSidText()
{
    std::vector<BYTE> sid = currentUserSid();
    if (sid.empty())
    {
        return {};
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid.data(), &text) == FALSE)
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

/// newFileReaders() of the descriptor @p sddl describes, for the user @p user (an SDDL SID; empty: none).
[[nodiscard]] DumpFolderAccess readersOf(std::string_view sddl, std::string_view user = OTHER_USER)
{
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(sddl);
    EXPECT_NE(descriptor, nullptr) << sddl;
    const auto sid = sidFrom(user);
    return newFileReaders(descriptor.get(), user.empty() ? nullptr : sid.get());
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

TEST(WindowsDumpFolderTest, ReadersOfADescriptorFlagLessPrivilegedReadsNewFilesInherit)
{
    // Administrators and SYSTEM only.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)"), DumpFolderAccess::AdministratorsOnly);
    // Users, Everyone, Authenticated Users or INTERACTIVE with an inherited read.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OI;FR;;;WD)"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICIIO;GR;;;AU)"), DumpFolderAccess::ReadableByUser) << "inherit-only still inherits";
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;IU)"), DumpFolderAccess::ReadableByUser);
    // The list right is the read right's bit: flagged too, conservatively.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;0x1;;;BU)"), DumpFolderAccess::ReadableByUser);
    // Not inherited by a new file (none, or folders only), no read, or a deny: not flagged.
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;;FR;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;CI;FR;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FW;;;BU)"), DumpFolderAccess::AdministratorsOnly);
    EXPECT_EQ(readersOf("O:BAD:P(D;OICI;FA;;;BU)(A;OICI;FA;;;BA)"), DumpFolderAccess::AdministratorsOnly);
    // The signed-in user's own SID, as an ACE or as the owner (who can change the DACL); not looked for without one.
    const std::string other(OTHER_USER);
    EXPECT_EQ(readersOf(std::format("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FR;;;{})", other)), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", other)), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(readersOf(std::format("O:{}D:P(A;OICI;FA;;;BA)", other), ""), DumpFolderAccess::AdministratorsOnly);
    // No DACL at all: everyone has every access.
    EXPECT_EQ(readersOf("O:BAD:NO_ACCESS_CONTROL"), DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(newFileReaders(nullptr, nullptr), DumpFolderAccess::Unknown);
}

// Real folders, made with explicit SDDL: this test's own account keeps a non-inherited right to open and remove them,
// and owns them, so the "signed-in user" here is another SID.
TEST(WindowsDumpFolderTest, TheCheckFlagsAFolderUsersCanReadAndPassesAnAdministratorsOnlyOne)
{
    const ScratchFolder scratch;
    const std::string me = ownSidText();
    ASSERT_FALSE(me.empty());
    const std::filesystem::path adminsOnly = scratch.path() / L"admins-only";
    const std::filesystem::path usersRead = scratch.path() / L"users-read";
    ASSERT_TRUE(makeFolderWith(adminsOnly, std::format("D:P(A;;FA;;;{})(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", me))) << GetLastError();
    ASSERT_TRUE(makeFolderWith(usersRead, std::format("D:P(A;;FA;;;{})(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)", me))) << GetLastError();
    const auto other = sidFrom(OTHER_USER);
    ASSERT_NE(other, nullptr);

    auto heldAdmins = openHeldDumpFolder(adminsOnly);
    ASSERT_TRUE(heldAdmins.has_value()) << heldAdmins.error();
    EXPECT_EQ(heldFolderReaders(heldAdmins->get(), other.get()), DumpFolderAccess::AdministratorsOnly);
    auto heldUsers = openHeldDumpFolder(usersRead);
    ASSERT_TRUE(heldUsers.has_value()) << heldUsers.error();
    EXPECT_EQ(heldFolderReaders(heldUsers->get(), other.get()), DumpFolderAccess::ReadableByUser);

    // This account as the signed-in user owns both, so both are flagged; checkDumpFolder() uses this account.
    EXPECT_EQ(checkDumpFolder(adminsOnly).access, DumpFolderAccess::ReadableByUser);
    EXPECT_EQ(checkDumpFolder(usersRead).access, DumpFolderAccess::ReadableByUser);
    EXPECT_TRUE(checkDumpFolder(usersRead).problem.empty());
}

} // namespace Platform::Windows
