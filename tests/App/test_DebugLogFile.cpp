// Tests for App::DebugLogFile (#4105): where tasksmack-debug.log is, and that a session whose environment may be the
// less-privileged side's writes it only to %ProgramData%\TaskSmack\Logs, made or taken over for Administrators and
// SYSTEM, with no junction, link or hard link on the way. The folders are seams: every case runs for real in a temp
// folder of this test's own, standing in for %ProgramData%, with junctions and hard links any user can make. Nothing
// here is elevated, and no real user or system folder is touched.

#include "App/DebugLogFile.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>   // GetNamedSecurityInfoW, SetNamedSecurityInfoW
#include <sddl.h>     // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <winioctl.h> // FSCTL_SET_REPARSE_POINT, for the junctions these tests make
// clang-format on

#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#else
#include <unistd.h>
#endif

namespace
{

/// A folder of this test's own under the temp folder; removed at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
#ifdef _WIN32
        : m_Path(std::filesystem::temp_directory_path() / std::format("TaskSmack.DebugLogFileTest.{}", GetCurrentProcessId()))
#else
        : m_Path(std::filesystem::temp_directory_path() / std::format("TaskSmack.DebugLogFileTest.{}", getpid()))
#endif
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
        std::filesystem::create_directories(m_Path / "Temp", error);
        std::filesystem::create_directories(m_Path / "ProgramData", error);
        EXPECT_FALSE(error) << error.message();
        // As Windows names it (a temp folder under an 8.3 name, say), so the folders' final paths are the paths asked for.
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

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return m_Path;
    }
    [[nodiscard]] std::filesystem::path temp() const
    {
        return m_Path / "Temp";
    }
    [[nodiscard]] std::filesystem::path programData() const
    {
        return m_Path / "ProgramData";
    }

  private:
    std::filesystem::path m_Path;
};

[[nodiscard]] std::string contentsOf(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes @p line through @p sink, as the app's logger does.
void logThrough(const spdlog::sink_ptr& sink, const std::string& line)
{
    spdlog::logger logger("DebugLogFileTest", sink);
    logger.info("{}", line);
    logger.flush();
}

/// open()'s calls on @p scratch: its Temp and ProgramData folders, and, on Windows, security this unelevated test can
/// set and still work in (Authenticated Users too, and no owner: only an elevated token may make Administrators one).
[[nodiscard]] App::DebugLogFile::Functions functionsIn(const ScratchFolder& scratch)
{
    return {
        .tempFolder = [temp = scratch.temp()] { return temp; },
        .programDataFolder = [programData = scratch.programData()] { return std::optional(programData); },
        .folderSecurity = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;AU)",
    };
}

// Not less privileged: the temp folder's tasksmack-debug.log by its path, emptied, as before #4105; nothing in ProgramData.
TEST(DebugLogFileTest, AnOwnEnvironmentOpensTheTempFoldersLogByItsPath)
{
    const ScratchFolder scratch;
    const std::filesystem::path log = scratch.temp() / App::DebugLogFile::FILE_NAME;
    std::ofstream(log, std::ios::binary) << "the last run's log\n";
    const auto opened = App::DebugLogFile::open(false, functionsIn(scratch));
    ASSERT_TRUE(opened.has_value()) << opened.error();
    EXPECT_EQ(opened->path, log);
    logThrough(opened->sink, "this run");
    const std::string text = contentsOf(log);
    EXPECT_TRUE(text.contains("this run")) << text;
    EXPECT_FALSE(text.contains("the last run's log")) << "emptied first";
    EXPECT_TRUE(std::filesystem::is_empty(scratch.programData()));
}

TEST(DebugLogFileTest, ATempFolderThatCantBeFoundSaysWhy)
{
    App::DebugLogFile::Functions functions;
    functions.tempFolder = [] -> std::filesystem::path
    {
        throw std::filesystem::filesystem_error("no temp folder", std::error_code());
    };
    const auto opened = App::DebugLogFile::open(false, functions);
    ASSERT_FALSE(opened.has_value());
    EXPECT_TRUE(opened.error().contains("no temp folder")) << opened.error();
}

TEST(DebugLogFileTest, TheElevatedLogFolderIsTaskSmackLogsUnderProgramData)
{
    EXPECT_EQ(App::DebugLogFile::elevatedLogFolder(std::filesystem::path("C:") / "ProgramData"),
              std::filesystem::path("C:") / "ProgramData" / "TaskSmack" / "Logs");
}

#ifdef _WIN32

TEST(DebugLogFileTest, ProgramDataComesFromItsKnownFolder)
{
    const auto programData = App::DebugLogFile::systemProgramDataFolder();
    ASSERT_TRUE(programData.has_value());
    const std::filesystem::path folder = programData.value_or(std::filesystem::path());
    EXPECT_TRUE(folder.is_absolute()) << folder.string();
}

TEST(DebugLogFileTest, TheElevatedLogsSecurityIsAdministratorsAndSystemOnly)
{
    EXPECT_EQ(App::DebugLogFile::ADMIN_SECURITY, "O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
}

/// The SDDL of folder @p folder's DACL, as Windows has it.
[[nodiscard]] std::string daclOf(const std::filesystem::path& folder)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) !=
        ERROR_SUCCESS)
    {
        return "(unreadable)";
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> owned(descriptor, &::LocalFree);
    LPSTR text = nullptr;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorA(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr) ==
        FALSE)
    {
        return "(unconvertible)";
    }
    std::string sddl(text);
    LocalFree(text);
    // SetSecurityInfo() marks a DACL it sets auto-inherited (AI), which changes nothing for what it grants.
    if (sddl.starts_with("D:PAI("))
    {
        sddl.erase(3, 2);
    }
    return sddl;
}

/// Gives @p folder back to whoever runs the test (it is theirs), so the fixture can remove it.
void grantBack(const std::filesystem::path& folder)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_NE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr), FALSE);
    const std::unique_ptr<void, decltype(&::LocalFree)> owned(descriptor, &::LocalFree);
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    ASSERT_NE(GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted), FALSE);
    std::wstring path = folder.native();
    EXPECT_EQ(SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr),
              static_cast<DWORD>(ERROR_SUCCESS));
}

/// ADMIN_SECURITY's DACL, with no owner (only an elevated token may make Administrators the owner) and access for the
/// folder's owner (OWNER RIGHTS), which this unelevated test's account is: without it the test couldn't open what it made
/// to read back. Everything else is the real thing: protected, nothing inherited, Administrators and SYSTEM full control.
constexpr std::string_view ADMINS_SYSTEM_AND_OWNER = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)";

/// This test process's own user SID, as an owned buffer; empty when it can't be read.
[[nodiscard]] std::vector<std::byte> currentUserSid()
{
    DWORD size = 0;
    GetTokenInformation(GetCurrentProcessToken(), TokenUser, nullptr, 0, &size);
    if (size == 0)
    {
        return {};
    }
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(GetCurrentProcessToken(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return {};
    }
    return buffer;
}

/// Sets @p folder's owner to @p sid; whether it took. Any token may make itself a folder's owner; only an elevated one
/// may set it to Administrators, so a test that needs that skips when this returns false.
[[nodiscard]] bool setOwner(const std::filesystem::path& folder, PSID sid)
{
    std::wstring path = folder.native();
    return SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, sid, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

/// Whether this token can set @p folder's owner to BUILTIN\Administrators (an elevated token can): the gate for the
/// in-place takeover tests, which only an elevated run reaches.
[[nodiscard]] bool canOwnAsAdministrators(const std::filesystem::path& folder)
{
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators{};
    auto size = static_cast<DWORD>(administrators.size());
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size) == FALSE)
    {
        return false;
    }
    return setOwner(folder, administrators.data());
}

/// Whether an entry named like TaskSmack.untrusted-* sits next to @p folder: a folder renamed aside.
[[nodiscard]] bool hasUntrustedSibling(const std::filesystem::path& folder)
{
    const std::wstring prefix = folder.filename().wstring() + L".untrusted-";
    const std::filesystem::directory_iterator entries(folder.parent_path());
    return std::ranges::any_of(entries,
                               [&prefix](const std::filesystem::directory_entry& entry)
                               { return entry.path().filename().wstring().starts_with(prefix); });
}

// A fresh folder is made with exactly ADMIN_SECURITY's DACL, inheriting nothing from the folder it is in.
TEST(DebugLogFileTest, AFreshFolderIsMadeForAdministratorsAndSystemOnly)
{
    const ScratchFolder scratch;
    const std::filesystem::path folder = scratch.programData() / "TaskSmack";
    const auto secured = App::DebugLogFile::secureFolder(folder, ADMINS_SYSTEM_AND_OWNER);
    ASSERT_TRUE(secured.has_value()) << secured.error();
    EXPECT_EQ(daclOf(folder), ADMINS_SYSTEM_AND_OWNER);
    EXPECT_FALSE(hasUntrustedSibling(folder)) << "nothing was there to move aside";
    grantBack(folder);
}

// A user-owned folder that is there already is renamed aside and a fresh one made in its place (#4138 review): it is
// never taken over where a handle the user still holds could re-grant them. The owner is set to this test's account
// explicitly, so the path is the same whether the run is elevated (where a created folder is Administrators-owned) or not.
TEST(DebugLogFileTest, AUserOwnedFolderIsRenamedAsideAndRemade)
{
    const ScratchFolder scratch;
    const std::filesystem::path folder = scratch.programData() / "TaskSmack";
    std::filesystem::create_directory(folder);
    auto sid = currentUserSid();
    ASSERT_FALSE(sid.empty());
    ASSERT_TRUE(setOwner(folder, reinterpret_cast<TOKEN_USER*>(sid.data())->User.Sid)) << "this account can own its own folder";
    std::ofstream(folder / "planted.txt", std::ios::binary) << "the user's own file\n";
    const auto secured = App::DebugLogFile::secureFolder(folder, ADMINS_SYSTEM_AND_OWNER);
    ASSERT_TRUE(secured.has_value()) << secured.error();
    EXPECT_EQ(daclOf(folder), ADMINS_SYSTEM_AND_OWNER) << "the fresh one, not the user's";
    EXPECT_FALSE(std::filesystem::exists(folder / "planted.txt")) << "a fresh folder, not the user's";
    EXPECT_TRUE(hasUntrustedSibling(folder)) << "the user's folder moved aside, not reused";
    grantBack(folder);
}

// The success path: the log is written in ProgramData, replaced on the next run (never reused), and nothing in TEMP.
TEST(DebugLogFileTest, TheElevatedLogIsWrittenInProgramDataAndReplacedEachRun)
{
    const ScratchFolder scratch;
    const auto functions = functionsIn(scratch);
    const std::filesystem::path log = App::DebugLogFile::elevatedLogFolder(scratch.programData()) / App::DebugLogFile::FILE_NAME;
    {
        auto first = App::DebugLogFile::open(true, functions);
        ASSERT_TRUE(first.has_value()) << first.error();
        logThrough(first->sink, "the last run, longer than this one");
    }
    auto opened = App::DebugLogFile::open(true, functions);
    ASSERT_TRUE(opened.has_value()) << opened.error();
    EXPECT_EQ(opened->path, log);
    logThrough(opened->sink, "this run");
    logThrough(opened->sink, "and more");
    opened->sink.reset(); // closes the file
    const std::string text = contentsOf(log);
    EXPECT_TRUE(text.contains("this run")) << text;
    EXPECT_FALSE(text.contains("the last run")) << "replaced, not appended: " << text;
    EXPECT_LT(text.find("this run"), text.find("and more")) << "written in order";
    EXPECT_FALSE(std::filesystem::exists(scratch.temp() / App::DebugLogFile::FILE_NAME)) << "nothing in TEMP";
}

// Finding 1: a log file a user planted, with a DACL of their own, is replaced, not reused: the run's log is a fresh file
// with ADMIN_SECURITY, so the user neither owns it nor can read it.
TEST(DebugLogFileTest, APlantedLogFileWithItsOwnDaclIsReplaced)
{
    const ScratchFolder scratch;
    const auto functions = functionsIn(scratch);
    const std::filesystem::path logs = App::DebugLogFile::elevatedLogFolder(scratch.programData());
    std::filesystem::create_directories(logs);
    const std::filesystem::path log = logs / App::DebugLogFile::FILE_NAME;
    // The user's own file, a DACL that is theirs alone, from before the run.
    std::ofstream(log, std::ios::binary) << "the user's planted log\n";
    {
        PSECURITY_DESCRIPTOR planted = nullptr;
        ASSERT_NE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;AU)", SDDL_REVISION_1, &planted, nullptr), FALSE);
        const std::unique_ptr<void, decltype(&::LocalFree)> owned(planted, &::LocalFree);
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        ASSERT_NE(GetSecurityDescriptorDacl(planted, &present, &dacl, &defaulted), FALSE);
        std::wstring path = log.native();
        ASSERT_EQ(SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr),
                  static_cast<DWORD>(ERROR_SUCCESS));
    }

    auto opened = App::DebugLogFile::open(true, functions);
    ASSERT_TRUE(opened.has_value()) << opened.error();
    logThrough(opened->sink, "this run");
    opened->sink.reset();
    const std::string fileDacl = daclOf(log);
    // The planted DACL granted only Authenticated Users; the fresh file grants Administrators and SYSTEM, which it didn't.
    EXPECT_TRUE(fileDacl.contains(";BA)")) << "replaced with the secured DACL: " << fileDacl;
    EXPECT_TRUE(fileDacl.contains(";SY)")) << fileDacl;
    EXPECT_TRUE(contentsOf(log).contains("this run")) << "replaced, not the planted bytes";
}

TEST(DebugLogFileTest, AnUnknownProgramDataFolderOpensNothing)
{
    const ScratchFolder scratch;
    auto functions = functionsIn(scratch);
    functions.programDataFolder = []
    {
        return std::optional<std::filesystem::path>();
    };
    const auto opened = App::DebugLogFile::open(true, functions);
    ASSERT_FALSE(opened.has_value());
    EXPECT_TRUE(opened.error().contains("ProgramData folder isn't known")) << opened.error();
    EXPECT_FALSE(std::filesystem::exists(scratch.temp() / App::DebugLogFile::FILE_NAME)) << "no fallback to TEMP";
}

// The reported path comes from the held folder's final path (the finalPath seam), not a name re-walked from the drive.
TEST(DebugLogFileTest, TheReportedPathUsesTheHeldFoldersFinalPath)
{
    const ScratchFolder scratch;
    auto functions = functionsIn(scratch);
    functions.finalPath = [](void*) -> std::wstring
    {
        return LR"(X:\Held\Logs)";
    };
    const auto opened = App::DebugLogFile::open(true, functions);
    ASSERT_TRUE(opened.has_value()) << opened.error();
    EXPECT_EQ(opened->path, std::filesystem::path(LR"(X:\Held\Logs)") / App::DebugLogFile::FILE_NAME);
}

/// A mount point's reparse data (ntifs.h's REPARSE_DATA_BUFFER, MountPointReparseBuffer), which user-mode headers
/// don't declare. As test_PinnedFolder.cpp makes one.
struct MountPointReparseData
{
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    std::array<wchar_t, MAX_PATH * 2> PathBuffer;
};

[[nodiscard]] Platform::Windows::UniqueHandle openForReparse(const std::filesystem::path& link)
{
    return Platform::Windows::UniqueHandle(CreateFileW(link.c_str(),
                                                       GENERIC_WRITE,
                                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                       nullptr,
                                                       OPEN_EXISTING,
                                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                       nullptr));
}

/// Turns the empty folder @p link into a junction to folder @p target, as any user may with a folder of their own.
/// Why not, when it can't.
[[nodiscard]] std::optional<std::string> makeJunction(const std::filesystem::path& link, const std::filesystem::path& target)
{
    const Platform::Windows::UniqueHandle folder = openForReparse(link);
    if (!folder)
    {
        return std::format("can't open {} to write: error {}", link.string(), GetLastError());
    }
    const std::wstring substitute = LR"(\??\)" + target.native();
    MountPointReparseData data{};
    if (substitute.size() + 2 > data.PathBuffer.size())
    {
        return std::string("the target's path is too long");
    }
    data.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    data.SubstituteNameLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.PrintNameOffset = static_cast<WORD>(data.SubstituteNameLength + sizeof(wchar_t));
    std::ranges::copy(substitute, data.PathBuffer.begin());
    // The four name fields, the substitute name and its terminator, and an empty print name with its terminator.
    data.ReparseDataLength = static_cast<WORD>((4 * sizeof(WORD)) + data.SubstituteNameLength + (2 * sizeof(wchar_t)));
    const auto size = static_cast<DWORD>(offsetof(MountPointReparseData, SubstituteNameOffset) + data.ReparseDataLength);
    DWORD returned = 0;
    if (DeviceIoControl(folder.get(), FSCTL_SET_REPARSE_POINT, &data, size, nullptr, 0, &returned, nullptr) == FALSE)
    {
        return std::format("FSCTL_SET_REPARSE_POINT failed: error {}", GetLastError());
    }
    return std::nullopt;
}

/// Undoes makeJunction(), so removing @p link removes nothing behind it.
void removeJunction(const std::filesystem::path& link)
{
    const Platform::Windows::UniqueHandle folder = openForReparse(link);
    REPARSE_GUID_DATA_BUFFER header{};
    header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    DWORD returned = 0;
    DeviceIoControl(
        folder.get(), FSCTL_DELETE_REPARSE_POINT, &header, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, nullptr, 0, &returned, nullptr);
}

// %ProgramData%\TaskSmack a junction a user made first (#4105's \RPC Control, in a form any test can make): it is moved
// aside through its own handle, never followed, a fresh TaskSmack is made, and the log is written. The folder behind the
// junction is neither made over nor written (#4138 review: a link is moved aside, not refused).
TEST(DebugLogFileTest, ATaskSmackFolderThatIsAJunctionIsMovedAside)
{
    const ScratchFolder scratch;
    const std::filesystem::path target = scratch.root() / "elsewhere";
    std::filesystem::create_directory(target);
    std::ofstream(target / App::DebugLogFile::FILE_NAME, std::ios::binary) << "a file the user may not change\n";
    const std::string daclBefore = daclOf(target);
    const std::filesystem::path taskSmack = scratch.programData() / "TaskSmack";
    std::filesystem::create_directory(taskSmack);
    const auto made = makeJunction(taskSmack, target);
    ASSERT_FALSE(made.has_value()) << made.value_or(std::string());

    auto opened = App::DebugLogFile::open(true, functionsIn(scratch));
    // Take the junction out of the path before any cleanup, so remove_all can't follow it into the target.
    for (std::filesystem::directory_iterator it(scratch.programData()), end; it != end; ++it)
    {
        if (it->path().filename().wstring().contains(L".untrusted-") && std::filesystem::is_symlink(it->path()))
        {
            removeJunction(it->path());
        }
    }
    ASSERT_TRUE(opened.has_value()) << opened.error();
    logThrough(opened->sink, "this run");
    opened->sink.reset();
    EXPECT_TRUE(
        contentsOf(App::DebugLogFile::elevatedLogFolder(scratch.programData()) / App::DebugLogFile::FILE_NAME).contains("this run"));
    EXPECT_EQ(contentsOf(target / App::DebugLogFile::FILE_NAME), "a file the user may not change\n") << "untouched";
    EXPECT_EQ(daclOf(target), daclBefore) << "the folder behind the junction isn't secured either";
}

// An Administrators-owned folder this run can open for WRITE_DAC is taken over in place: its DACL is reset and its
// contents kept. Only an elevated run can make a folder Administrators-owned, so it is skipped otherwise.
TEST(DebugLogFileTest, AnAdministratorsOwnedFolderIsTakenOverInPlace)
{
    const ScratchFolder scratch;
    const std::filesystem::path folder = scratch.programData() / "TaskSmack";
    std::filesystem::create_directory(folder);
    std::ofstream(folder / "kept.txt", std::ios::binary) << "made by a trusted run\n";
    if (!canOwnAsAdministrators(folder))
    {
        GTEST_SKIP() << "this token can't own a folder as Administrators (not elevated)";
    }
    const auto secured = App::DebugLogFile::secureFolder(folder, ADMINS_SYSTEM_AND_OWNER);
    ASSERT_TRUE(secured.has_value()) << secured.error();
    EXPECT_EQ(daclOf(folder), ADMINS_SYSTEM_AND_OWNER);
    EXPECT_TRUE(std::filesystem::exists(folder / "kept.txt")) << "taken over in place, not remade";
    EXPECT_FALSE(hasUntrustedSibling(folder)) << "nothing moved aside";
    grantBack(folder);
}

// An Administrators-owned TaskSmack holding a user-owned Logs (#4138 review): TaskSmack is taken over in place, Logs is
// moved aside and remade, and the log is written. Elevated-only, since TaskSmack must be Administrators-owned.
TEST(DebugLogFileTest, AnAdminTaskSmackWithAUserOwnedLogsTakesOverAndRemakesLogs)
{
    const ScratchFolder scratch;
    const std::filesystem::path taskSmack = scratch.programData() / "TaskSmack";
    const std::filesystem::path logs = taskSmack / "Logs";
    std::filesystem::create_directories(logs);
    auto sid = currentUserSid();
    ASSERT_FALSE(sid.empty());
    ASSERT_TRUE(setOwner(logs, reinterpret_cast<TOKEN_USER*>(sid.data())->User.Sid));
    if (!canOwnAsAdministrators(taskSmack))
    {
        GTEST_SKIP() << "this token can't own TaskSmack as Administrators (not elevated)";
    }
    auto opened = App::DebugLogFile::open(true, functionsIn(scratch));
    ASSERT_TRUE(opened.has_value()) << opened.error();
    logThrough(opened->sink, "this run");
    opened->sink.reset();
    EXPECT_TRUE(contentsOf(logs / App::DebugLogFile::FILE_NAME).contains("this run"));
    EXPECT_TRUE(hasUntrustedSibling(logs)) << "the user-owned Logs was moved aside";
    grantBack(taskSmack);
}

// A log hard-linked to another file before the run: replaced, not reused (finding 1 again), and the other file keeps its
// bytes (deleting one hard link leaves the other whole).
TEST(DebugLogFileTest, AHardLinkedLogIsReplacedAndTheOtherFileKept)
{
    const ScratchFolder scratch;
    const std::filesystem::path victim = scratch.root() / "precious.txt";
    std::ofstream(victim, std::ios::binary) << "a file the user may not change\n";
    const std::filesystem::path logs = App::DebugLogFile::elevatedLogFolder(scratch.programData());
    std::filesystem::create_directories(logs);
    const std::filesystem::path log = logs / App::DebugLogFile::FILE_NAME;
    ASSERT_NE(CreateHardLinkW(log.c_str(), victim.c_str(), nullptr), FALSE) << GetLastError();

    auto opened = App::DebugLogFile::open(true, functionsIn(scratch));
    ASSERT_TRUE(opened.has_value()) << opened.error();
    logThrough(opened->sink, "this run");
    opened->sink.reset();
    EXPECT_EQ(contentsOf(victim), "a file the user may not change\n") << "the other hard link is untouched";
    EXPECT_TRUE(contentsOf(log).contains("this run")) << "a fresh file, not the planted one";
}

#endif // _WIN32

} // namespace
