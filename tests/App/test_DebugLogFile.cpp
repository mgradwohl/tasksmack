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

/// ADMIN_SECURITY's DACL, with no owner (only an elevated token may make Administrators one), and access for the folder's
/// owner (OWNER RIGHTS), which this unelevated test is: without it the test couldn't open the folder it made to check it. Everything else
/// is the real thing: protected, nothing inherited, Administrators and SYSTEM full control.
constexpr std::string_view ADMINS_SYSTEM_AND_OWNER = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)";

// A folder made with it has exactly that DACL, and inherits nothing from the folder it is in.
TEST(DebugLogFileTest, AFolderIsMadeWithAProtectedAdministratorsAndSystemDacl)
{
    const ScratchFolder scratch;
    const std::filesystem::path folder = scratch.programData() / "TaskSmack";
    const auto secured = App::DebugLogFile::secureFolder(folder, ADMINS_SYSTEM_AND_OWNER);
    ASSERT_TRUE(secured.has_value()) << secured.error();
    EXPECT_EQ(daclOf(folder), ADMINS_SYSTEM_AND_OWNER);
    grantBack(folder);
}

// A folder someone else made first (Users may create in %ProgramData%) is taken over: its DACL replaced, not kept.
TEST(DebugLogFileTest, AFolderThatIsThereAlreadyIsTakenOver)
{
    const ScratchFolder scratch;
    const std::filesystem::path folder = scratch.programData() / "TaskSmack";
    std::filesystem::create_directory(folder);
    const auto secured = App::DebugLogFile::secureFolder(folder, ADMINS_SYSTEM_AND_OWNER);
    ASSERT_TRUE(secured.has_value()) << secured.error();
    EXPECT_EQ(daclOf(folder), ADMINS_SYSTEM_AND_OWNER);
    grantBack(folder);
}

TEST(DebugLogFileTest, TheElevatedLogIsWrittenInProgramDataAndEmptiedFirst)
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
    EXPECT_FALSE(text.contains("the last run")) << "emptied first: " << text;
    EXPECT_LT(text.find("this run"), text.find("and more")) << "written in order";
    EXPECT_FALSE(std::filesystem::exists(scratch.temp() / App::DebugLogFile::FILE_NAME)) << "nothing in TEMP";
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
    EXPECT_NE(DeviceIoControl(
                  folder.get(), FSCTL_DELETE_REPARSE_POINT, &header, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, nullptr, 0, &returned, nullptr),
              FALSE)
        << "the junction at " << link.string() << " is left: error " << GetLastError();
}

// %ProgramData%\TaskSmack, or its Logs, a junction a user made first (#4105's \RPC Control, in a form any test can
// make): refused, and nothing behind it is touched, its DACL included.
TEST(DebugLogFileTest, AJunctionForEitherLogFolderIsRefused)
{
    for (const bool logsLinked : {false, true})
    {
        const ScratchFolder scratch;
        const std::filesystem::path target = scratch.root() / "elsewhere";
        std::filesystem::create_directory(target);
        const std::filesystem::path victim = target / App::DebugLogFile::FILE_NAME;
        std::ofstream(victim, std::ios::binary) << "a file the user may not change\n";
        const std::string daclBefore = daclOf(target);
        const std::filesystem::path logs = App::DebugLogFile::elevatedLogFolder(scratch.programData());
        const std::filesystem::path link = logsLinked ? logs : logs.parent_path();
        std::filesystem::create_directories(link);
        const auto made = makeJunction(link, target);
        ASSERT_FALSE(made.has_value()) << made.value_or(std::string());

        const auto opened = App::DebugLogFile::open(true, functionsIn(scratch));
        removeJunction(link);
        if (logsLinked)
        {
            grantBack(logs.parent_path()); // secured by the run, before Logs was refused
        }
        ASSERT_FALSE(opened.has_value()) << (logsLinked ? "Logs" : "TaskSmack");
        EXPECT_TRUE(opened.error().contains("is a junction or link")) << opened.error();
        EXPECT_EQ(contentsOf(victim), "a file the user may not change\n") << "neither emptied nor written";
        EXPECT_EQ(daclOf(target), daclBefore) << "the folder behind the junction isn't secured either";
    }
}

// tasksmack-debug.log hard-linked to another file before the run: refused, and that file is neither emptied nor written.
TEST(DebugLogFileTest, ALogHardLinkedToAnotherFileIsRefused)
{
    const ScratchFolder scratch;
    const std::filesystem::path victim = scratch.root() / "precious.txt";
    std::ofstream(victim, std::ios::binary) << "a file the user may not change\n";
    const std::filesystem::path logs = App::DebugLogFile::elevatedLogFolder(scratch.programData());
    std::filesystem::create_directories(logs);
    const std::filesystem::path log = logs / App::DebugLogFile::FILE_NAME;
    ASSERT_NE(CreateHardLinkW(log.c_str(), victim.c_str(), nullptr), FALSE) << GetLastError();

    const auto opened = App::DebugLogFile::open(true, functionsIn(scratch));
    ASSERT_FALSE(opened.has_value());
    EXPECT_TRUE(opened.error().contains("is hard-linked to another file")) << opened.error();
    EXPECT_EQ(contentsOf(victim), "a file the user may not change\n") << "neither emptied nor written";
}

#endif // _WIN32

} // namespace
