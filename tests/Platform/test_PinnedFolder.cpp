// Tests for PinnedFolder (#3774): every file TaskSmack opens or creates in a handed-over config folder is opened
// relative to the held folder's handle and checked to be inside it. The refusals run on fakes (a folder that becomes
// a reparse point after the pin, an open whose final path is elsewhere); the opens themselves run for real in a
// folder of this test's own under the temp folder.

#include "Core/ConfigDirOverride.h"
#include "Platform/Windows/PinnedFolder.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <winioctl.h> // FSCTL_SET_REPARSE_POINT, for the mount points these tests make
// clang-format on

namespace Platform::Windows
{
namespace
{

constexpr std::wstring_view HELD_PATH = LR"(C:\Users\pat\AppData\Roaming\TaskSmack)";

/// What the fakes answer.
struct FakeFolder
{
    HANDLE held = nullptr; ///< The held folder's handle
    DWORD heldAttributes = FILE_ATTRIBUTE_DIRECTORY;
    int heldReadsBeforeReparse = -1; ///< After this many reads of the held folder, it reports a reparse point; -1 never
    int heldReads = 0;
    int creates = 0;              ///< NtCreateFile calls
    std::wstring openedPath{};    // NOLINT(readability-redundant-member-init) - empty: inside the folder, as asked
    std::vector<HANDLE> opened{}; // NOLINT(readability-redundant-member-init) - the handles NtCreateFile gave out
    std::wstring lastName{};      // NOLINT(readability-redundant-member-init) - the name NtCreateFile was given
    HANDLE lastRoot = nullptr;    ///< The RootDirectory NtCreateFile was given
};

[[nodiscard]] FakeFolder& fake()
{
    static FakeFolder folder;
    return folder;
}

NTSTATUS NTAPI fakeNtCreateFile(PHANDLE handle,
                                ACCESS_MASK /*access*/,
                                POBJECT_ATTRIBUTES attributes,
                                PIO_STATUS_BLOCK /*status*/,
                                PLARGE_INTEGER /*allocationSize*/,
                                ULONG /*fileAttributes*/,
                                ULONG /*share*/,
                                ULONG /*disposition*/,
                                ULONG /*options*/,
                                PVOID /*eaBuffer*/,
                                ULONG /*eaLength*/)
{
    ++fake().creates;
    fake().lastRoot = attributes->RootDirectory;
    fake().lastName.assign(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    *handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // a real handle of this process's own, closed as a file's would be
    fake().opened.push_back(*handle);
    return 0;
}

BOOL WINAPI fakeFileInformation(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info)
{
    *info = BY_HANDLE_FILE_INFORMATION{};
    info->nNumberOfLinks = 1;
    if (handle == fake().held)
    {
        const bool reparse = fake().heldReadsBeforeReparse >= 0 && fake().heldReads++ >= fake().heldReadsBeforeReparse;
        info->dwFileAttributes = fake().heldAttributes | (reparse ? FILE_ATTRIBUTE_REPARSE_POINT : 0);
        return TRUE;
    }
    info->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    return TRUE;
}

DWORD WINAPI fakeFinalPath(HANDLE handle, LPWSTR buffer, DWORD size, DWORD /*flags*/)
{
    std::wstring path = LR"(\\?\)";
    if (handle == fake().held)
    {
        path += HELD_PATH;
    }
    else
    {
        path += fake().openedPath.empty() ? std::wstring(HELD_PATH) + L'\\' + fake().lastName : fake().openedPath;
    }
    if (size <= path.size())
    {
        return static_cast<DWORD>(path.size() + 1);
    }
    std::ranges::copy(path, buffer);
    std::span(buffer, size)[path.size()] = L'\0';
    return static_cast<DWORD>(path.size());
}

constexpr PinnedFolderFunctions FAKES{
    .ntCreateFile = &fakeNtCreateFile,
    .getFileInformation = &fakeFileInformation,
    .getFinalPath = &fakeFinalPath,
};

/// A PinnedFolder on the fakes, holding an event of this process's own in place of the folder.
[[nodiscard]] PinnedFolder fakePinned()
{
    UniqueHandle held(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    fake().held = held.get();
    return {std::filesystem::path(HELD_PATH), std::move(held), FAKES};
}

TEST(PinnedFolderTest, TheLockFileIsOpenedRelativeToTheHeldFolder)
{
    fake() = FakeFolder{};
    const PinnedFolder folder = fakePinned();
    auto lock = folder.openLockFile(L"tasksmack.lock");
    ASSERT_TRUE(lock.has_value()) << lock.error();
    const UniqueHandle owned(static_cast<HANDLE>(*lock));
    EXPECT_EQ(fake().creates, 1);
    EXPECT_EQ(fake().lastRoot, fake().held) << "relative to the held handle, never by a path from the drive";
    EXPECT_EQ(fake().lastName, L"tasksmack.lock") << "one name, looked up in the held folder";
}

// The folder becomes a reparse point after the pin: nothing is opened in it any more.
TEST(PinnedFolderTest, AFolderThatBecomesAReparsePointAfterThePinOpensNothing)
{
    fake() = FakeFolder{.heldReadsBeforeReparse = 0};
    const PinnedFolder folder = fakePinned();
    const auto lock = folder.openLockFile(L"tasksmack.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_TRUE(lock.error().contains("has become a reparse point since it was checked")) << lock.error();
    EXPECT_EQ(fake().creates, 0) << "refused before anything is opened";

    EXPECT_FALSE(folder.readFile(L"config.toml").has_value());
    EXPECT_FALSE(folder.replaceFile(L"config.toml", "x = 1\n").has_value());
    EXPECT_FALSE(folder.createFolder(L"themes").has_value());
    EXPECT_FALSE(folder.listFiles(L"themes").has_value());
    EXPECT_EQ(fake().creates, 0);
}

// An open whose final path isn't where it was asked for in the held folder is refused, and its handle closed.
TEST(PinnedFolderTest, AnOpenThatLandsOutsideTheHeldFolderIsRefused)
{
    fake() = FakeFolder{.openedPath = LR"(C:\Windows\System32\tasksmack.lock)"};
    const PinnedFolder folder = fakePinned();
    const auto lock = folder.openLockFile(L"tasksmack.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_TRUE(lock.error().contains("opened outside the folder held: C:\\Windows\\System32\\tasksmack.lock")) << lock.error();
    ASSERT_EQ(fake().opened.size(), 1U);
    EXPECT_EQ(WaitForSingleObject(fake().opened.front(), 0), WAIT_FAILED) << "the handle it got was closed";

    const auto read = folder.readFile(L"config.toml");
    ASSERT_FALSE(read.has_value());
    EXPECT_TRUE(read.error().contains("opened outside the folder held")) << read.error();
}

TEST(PinnedFolderTest, OnlyPlainNamesInTheFolderAreOpened)
{
    fake() = FakeFolder{};
    const PinnedFolder folder = fakePinned();
    for (const std::wstring_view name : {
             std::wstring_view(L".."),
             std::wstring_view(LR"(..\elsewhere.lock)"),
             std::wstring_view(LR"(C:\Windows\x.lock)"),
             std::wstring_view(LR"(\x.lock)"),
             std::wstring_view(L"tasksmack.lock:stream"),
             std::wstring_view(LR"(themes\x.lock)"),
             std::wstring_view(L""),
         })
    {
        EXPECT_FALSE(folder.openLockFile(std::filesystem::path(name)).has_value()) << std::filesystem::path(name).string();
    }
    EXPECT_FALSE(folder.readFile(LR"(themes\..\..\x.toml)").has_value());
    EXPECT_EQ(fake().creates, 0);
}

// The real calls, in a folder of this test's own: what is opened or created lands in that folder.

/// A folder of this test's own under the temp folder, held as checkConfigDir() holds one; removed at the end.
class HeldScratchFolder
{
  public:
    HeldScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"TaskSmack.PinnedFolderTest.{}", GetCurrentProcessId()))
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
        std::filesystem::create_directories(m_Path, error);
        EXPECT_FALSE(error) << error.message();
        // The path as Windows names it (a temp folder under a short 8.3 name, say), so the final-path checks match.
        const UniqueHandle probe(CreateFileW(m_Path.c_str(),
                                             FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS,
                                             nullptr));
        m_Path = finalPathOf(probe.get(), &::GetFinalPathNameByHandleW).value_or(m_Path.native());
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~HeldScratchFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
    }
    HeldScratchFolder(const HeldScratchFolder&) = delete;
    HeldScratchFolder& operator=(const HeldScratchFolder&) = delete;
    HeldScratchFolder(HeldScratchFolder&&) = delete;
    HeldScratchFolder& operator=(HeldScratchFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// A handle on @p scratch opened as checkConfigDir() opens the folder it holds.
[[nodiscard]] UniqueHandle heldHandleOf(const HeldScratchFolder& scratch)
{
    UniqueHandle held(CreateFileW(scratch.path().c_str(),
                                  FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    EXPECT_TRUE(held) << GetLastError();
    return held;
}

/// @p scratch held with every real call, as checkConfigDir() holds a folder.
[[nodiscard]] PinnedFolder holdReally(const HeldScratchFolder& scratch, PinnedFolderFunctions functions = {})
{
    return {scratch.path(), heldHandleOf(scratch), functions};
}

/// The names of the entries directly in @p folder, sorted.
[[nodiscard]] std::vector<std::wstring> entriesOf(const std::filesystem::path& folder)
{
    std::vector<std::wstring> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        names.push_back(it->path().filename().native());
    }
    std::ranges::sort(names);
    return names;
}

/// A mount point's reparse data (ntifs.h's REPARSE_DATA_BUFFER, MountPointReparseBuffer), which user-mode headers
/// don't declare.
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

/// Turns the empty folder @p link into a mount point (a junction) to folder @p target, as any user may with a
/// folder of their own: no privilege, FSCTL_SET_REPARSE_POINT on a handle opened for writing. Why not, when it can't.
[[nodiscard]] std::optional<std::string> makeMountPoint(const std::filesystem::path& link, const std::filesystem::path& target)
{
    const UniqueHandle folder(CreateFileW(link.c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
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
    data.SubstituteNameOffset = 0;
    data.SubstituteNameLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.PrintNameOffset = static_cast<WORD>(data.SubstituteNameLength + sizeof(wchar_t));
    data.PrintNameLength = 0;
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

/// Undoes makeMountPoint(): @p link is an empty folder again, so removing it removes nothing behind it.
void removeMountPoint(const std::filesystem::path& link)
{
    const UniqueHandle folder(CreateFileW(link.c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
    REPARSE_GUID_DATA_BUFFER header{};
    header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    DWORD returned = 0;
    EXPECT_NE(DeviceIoControl(
                  folder.get(), FSCTL_DELETE_REPARSE_POINT, &header, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, nullptr, 0, &returned, nullptr),
              FALSE)
        << "the mount point at " << link.string() << " is left: error " << GetLastError();
}

[[nodiscard]] std::string contentsOf(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(PinnedFolderTest, TheRealCallsOpenCreateAndReplaceFilesInsideTheFolder)
{
    const HeldScratchFolder scratch;
    const PinnedFolder folder = holdReally(scratch);

    auto lock = folder.openLockFile(L"tasksmack.lock");
    ASSERT_TRUE(lock.has_value()) << lock.error();
    const UniqueHandle lockHandle(static_cast<HANDLE>(*lock));
    EXPECT_TRUE(std::filesystem::is_regular_file(scratch.path() / L"tasksmack.lock"));

    const auto missing = folder.readFile(L"config.toml");
    ASSERT_TRUE(missing.has_value()) << missing.error();
    EXPECT_FALSE(missing->has_value()) << "not there yet";

    const auto first = folder.replaceFile(L"config.toml", "[theme]\nid = \"mocha\"\n");
    ASSERT_TRUE(first.has_value()) << first.error();
    const auto second = folder.replaceFile(L"config.toml", "[theme]\nid = \"arctic-fire\"\n");
    ASSERT_TRUE(second.has_value()) << "over the one there: " << second.error();
    EXPECT_EQ(contentsOf(scratch.path() / L"config.toml"), "[theme]\nid = \"arctic-fire\"\n");
    const auto read = folder.readFile(L"config.toml");
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->value_or(""), "[theme]\nid = \"arctic-fire\"\n");

    ASSERT_TRUE(folder.createFolder(L"themes").has_value());
    ASSERT_TRUE(folder.createFolder(L"themes").has_value()) << "already there";
    std::ofstream(scratch.path() / L"themes" / L"mine.toml", std::ios::binary) << "[meta]\nname = \"Mine\"\n";
    std::filesystem::create_directory(scratch.path() / L"themes" / L"sub");
    const auto listed = folder.listFiles(L"themes");
    ASSERT_TRUE(listed.has_value()) << listed.error();
    EXPECT_EQ(*listed, std::vector<std::filesystem::path>{L"mine.toml"}) << "plain files only";
    const auto theme = folder.readFile(std::filesystem::path(L"themes") / L"mine.toml");
    ASSERT_TRUE(theme.has_value()) << theme.error();
    EXPECT_EQ(theme->value_or(""), "[meta]\nname = \"Mine\"\n");

    const auto absent = folder.listFiles(L"nothing-here");
    ASSERT_TRUE(absent.has_value()) << absent.error();
    EXPECT_TRUE(absent->empty());

    // Nothing but what was asked for: no staging file left behind.
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(scratch.path()))
    {
        names.push_back(entry.path().filename().native());
    }
    std::ranges::sort(names);
    EXPECT_EQ(names, (std::vector<std::wstring>{L"config.toml", L"tasksmack.lock", L"themes"}));
}

TEST(PinnedFolderTest, TheRealCallsRefuseAHardLinkedFile)
{
    const HeldScratchFolder scratch;
    const PinnedFolder folder = holdReally(scratch);
    std::ofstream(scratch.path() / L"config.toml") << "x = 1\n";
    ASSERT_NE(CreateHardLinkW((scratch.path() / L"other.toml").c_str(), (scratch.path() / L"config.toml").c_str(), nullptr), FALSE)
        << GetLastError();
    const auto read = folder.readFile(L"config.toml");
    ASSERT_FALSE(read.has_value());
    EXPECT_TRUE(read.error().contains("hard-linked")) << read.error();
}

TEST(PinnedFolderTest, AStagingNameAlreadyThereIsNeverOverwritten)
{
    const HeldScratchFolder scratch;
    PinnedFolder folder = holdReally(scratch);
    std::ofstream(scratch.path() / L".tasksmack-config.00000001.tmp") << "someone else's";
    std::uint32_t next = 1;
    folder.setTempNameSourceForTesting([&next] { return next++; });
    const auto replaced = folder.replaceFile(L"config.toml", "x = 2\n");
    ASSERT_TRUE(replaced.has_value()) << replaced.error();
    EXPECT_EQ(contentsOf(scratch.path() / L".tasksmack-config.00000001.tmp"), "someone else's");
    EXPECT_EQ(contentsOf(scratch.path() / L"config.toml"), "x = 2\n");
}

/// A folder of this test's own beside the held one, for a mount point to name; removed at the end.
class TargetFolder
{
  public:
    TargetFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"TaskSmack.PinnedFolderTest.{}.target", GetCurrentProcessId()))
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
        std::filesystem::create_directories(m_Path, error);
        EXPECT_FALSE(error) << error.message();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~TargetFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
    }
    TargetFolder(const TargetFolder&) = delete;
    TargetFolder& operator=(const TargetFolder&) = delete;
    TargetFolder(TargetFolder&&) = delete;
    TargetFolder& operator=(TargetFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

// A junction planted inside the held folder (its themes folder, say) is opened itself, never followed, and refused.
TEST(PinnedFolderTest, TheRealCallsRefuseAJunctionPlantedInTheFolder)
{
    const TargetFolder target;
    std::ofstream(target.path() / L"elsewhere.toml", std::ios::binary) << "[meta]\nname = \"Elsewhere\"\n";
    const HeldScratchFolder scratch;
    std::filesystem::create_directory(scratch.path() / L"themes");
    if (const auto why = makeMountPoint(scratch.path() / L"themes", target.path()); why.has_value())
    {
        GTEST_SKIP() << "can't make a mount point here: " << *why;
    }
    {
        const PinnedFolder folder = holdReally(scratch);
        const auto read = folder.readFile(std::filesystem::path(L"themes") / L"elsewhere.toml");
        ASSERT_FALSE(read.has_value()) << "read through the junction";
        EXPECT_TRUE(read.error().contains("is a link, junction or mount point")) << read.error();
        const auto listed = folder.listFiles(L"themes");
        ASSERT_FALSE(listed.has_value()) << "listed through the junction";
        EXPECT_FALSE(folder.createFolder(L"themes").has_value()) << "the junction isn't taken for the folder";
    }
    removeMountPoint(scratch.path() / L"themes");
    EXPECT_EQ(entriesOf(target.path()), std::vector<std::wstring>{L"elsewhere.toml"}) << "nothing made behind the junction";
}

// The held folder itself turned into a mount point after the pin: nothing more is opened or created in it, and nothing
// lands where the mount point leads.
TEST(PinnedFolderTest, TheRealCallsRefuseTheHeldFolderOnceItIsAMountPoint)
{
    const TargetFolder target;
    const HeldScratchFolder scratch;
    {
        const PinnedFolder folder = holdReally(scratch);
        if (const auto why = makeMountPoint(scratch.path(), target.path()); why.has_value())
        {
            GTEST_SKIP() << "can't make a mount point here: " << *why;
        }
        const auto lock = folder.openLockFile(L"tasksmack.lock");
        ASSERT_FALSE(lock.has_value()) << "opened in a folder that became a mount point";
        EXPECT_TRUE(lock.error().contains("has become a reparse point since it was checked")) << lock.error();
        EXPECT_FALSE(folder.replaceFile(L"config.toml", "x = 1\n").has_value());
        EXPECT_FALSE(folder.createFolder(L"themes").has_value());
        EXPECT_FALSE(folder.readFile(L"config.toml").has_value());
        removeMountPoint(scratch.path());
    }
    EXPECT_TRUE(entriesOf(target.path()).empty()) << "nothing made where the mount point led";
    EXPECT_TRUE(entriesOf(scratch.path()).empty()) << "nothing made in the folder either";
}

NTSTATUS NTAPI failingRename(HANDLE /*file*/, PIO_STATUS_BLOCK /*status*/, PVOID /*information*/, ULONG /*length*/, ULONG /*what*/)
{
    return static_cast<NTSTATUS>(0xC0000022L); // STATUS_ACCESS_DENIED
}

// A rename that fails leaves the old config.toml whole, and its staging file removed.
TEST(PinnedFolderTest, AFailedRenameLeavesTheOldFileAndNoStagingFile)
{
    const HeldScratchFolder scratch;
    std::ofstream(scratch.path() / L"config.toml", std::ios::binary) << "x = 1\n";
    PinnedFolderFunctions functions;
    functions.ntSetInformationFile = &failingRename;
    const PinnedFolder folder = holdReally(scratch, functions);
    const auto replaced = folder.replaceFile(L"config.toml", "x = 2\n");
    ASSERT_FALSE(replaced.has_value());
    EXPECT_TRUE(replaced.error().contains("couldn't replace")) << replaced.error();
    EXPECT_EQ(contentsOf(scratch.path() / L"config.toml"), "x = 1\n");
    EXPECT_EQ(entriesOf(scratch.path()), std::vector<std::wstring>{L"config.toml"}) << "no staging file left";
}

/// GetFinalPathNameByHandleW, but a staging file's path is said to be elsewhere: the create's checks refuse it.
DWORD WINAPI stagingElsewhere(HANDLE handle, LPWSTR buffer, DWORD size, DWORD flags)
{
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer, size, flags);
    if (length == 0 || length >= size || !std::wstring_view(buffer, length).contains(L".tasksmack-config."))
    {
        return length;
    }
    constexpr std::wstring_view ELSEWHERE = LR"(\\?\C:\Elsewhere\.tasksmack-config.tmp)";
    if (size <= ELSEWHERE.size())
    {
        return static_cast<DWORD>(ELSEWHERE.size() + 1);
    }
    std::ranges::copy(ELSEWHERE, buffer);
    std::span(buffer, size)[ELSEWHERE.size()] = L'\0';
    return static_cast<DWORD>(ELSEWHERE.size());
}

// A staging file the create's own checks refuse is removed again: nothing it made is left behind.
TEST(PinnedFolderTest, ARefusedStagingFileLeavesNothingBehind)
{
    const HeldScratchFolder scratch;
    std::ofstream(scratch.path() / L"config.toml", std::ios::binary) << "x = 1\n";
    PinnedFolderFunctions functions;
    functions.getFinalPath = &stagingElsewhere;
    const PinnedFolder folder = holdReally(scratch, functions);
    const auto replaced = folder.replaceFile(L"config.toml", "x = 2\n");
    ASSERT_FALSE(replaced.has_value());
    EXPECT_TRUE(replaced.error().contains("opened outside the folder held")) << replaced.error();
    EXPECT_EQ(contentsOf(scratch.path() / L"config.toml"), "x = 1\n");
    EXPECT_EQ(entriesOf(scratch.path()), std::vector<std::wstring>{L"config.toml"}) << "no staging file left";
}

// A config.toml held open by another program (an indexer, antivirus) that lets it be deleted is still replaced:
// POSIX rename semantics.
TEST(PinnedFolderTest, AConfigHeldOpenWithShareDeleteIsStillReplaced)
{
    const HeldScratchFolder scratch;
    std::ofstream(scratch.path() / L"config.toml", std::ios::binary) << "x = 1\n";
    const PinnedFolder folder = holdReally(scratch);
    const UniqueHandle reader(CreateFileW((scratch.path() / L"config.toml").c_str(),
                                          GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr));
    ASSERT_TRUE(reader) << GetLastError();
    const auto replaced = folder.replaceFile(L"config.toml", "x = 2\n");
    ASSERT_TRUE(replaced.has_value()) << replaced.error();
    EXPECT_EQ(contentsOf(scratch.path() / L"config.toml"), "x = 2\n");
}

// ========== readHandedOverFiles() through the real held folder (#4094) ==========

/// @p folder handed over to Core::ConfigDirOverride for the scope's length, as main.cpp hands over the folder it
/// checked and holds; what active() was is put back after.
class ScopedHandOff
{
  public:
    explicit ScopedHandOff(const std::shared_ptr<const PinnedFolder>& folder) : m_Previous(Core::ConfigDirOverride::active())
    {
        Core::ConfigDirOverride::adoptHandedOver(folder->path(), folder);
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - a test fixture: what escapes it ends the run, as a failed test should
    ~ScopedHandOff()
    {
        Core::ConfigDirOverride::setActiveForTesting(m_Previous);
    }
    ScopedHandOff(const ScopedHandOff&) = delete;
    ScopedHandOff& operator=(const ScopedHandOff&) = delete;
    ScopedHandOff(ScopedHandOff&&) = delete;
    ScopedHandOff& operator=(ScopedHandOff&&) = delete;

  private:
    std::optional<std::filesystem::path> m_Previous;
};

/// The names of @p files, relative to @p folder.
[[nodiscard]] std::vector<std::wstring> namesOf(const std::vector<Core::ConfigDirOverride::HandedOverFile>& files,
                                                const std::filesystem::path& folder)
{
    std::vector<std::wstring> names;
    names.reserve(files.size());
    for (const auto& file : files)
    {
        names.push_back(file.path.lexically_relative(folder).native());
    }
    return names;
}

// The user themes of a hand-off: only the plain .toml files in its themes folder are read, through the held folder. A
// folder named like a theme, a junction named like one, and a file hard-linked from outside are skipped; a junction
// needs no privilege, so this runs unelevated.
TEST(PinnedFolderTest, HandedOverThemesAreThePlainTomlFilesOfTheHeldFolder)
{
    const TargetFolder target;
    std::ofstream(target.path() / L"elsewhere.toml", std::ios::binary) << "[meta]\nname = \"Elsewhere\"\n";
    const HeldScratchFolder scratch;
    const std::filesystem::path themes = scratch.path() / L"themes";
    std::filesystem::create_directory(themes);
    std::ofstream(themes / L"mine.toml", std::ios::binary) << "[meta]\nname = \"Mine\"\n";
    std::ofstream(themes / L"other.toml", std::ios::binary) << "[meta]\nname = \"Other\"\n";
    std::ofstream(themes / L"notes.txt", std::ios::binary) << "not a theme\n";
    std::filesystem::create_directory(themes / L"folder.toml");
    ASSERT_NE(CreateHardLinkW((themes / L"planted.toml").c_str(), (target.path() / L"elsewhere.toml").c_str(), nullptr), FALSE)
        << GetLastError();
    std::filesystem::create_directory(themes / L"junction.toml");
    if (const auto why = makeMountPoint(themes / L"junction.toml", target.path()); why.has_value())
    {
        GTEST_SKIP() << "can't make a mount point here: " << *why;
    }
    {
        const auto folder = std::make_shared<const PinnedFolder>(scratch.path(), heldHandleOf(scratch));
        const ScopedHandOff handOff(folder);
        const auto files = Core::ConfigDirOverride::readHandedOverFiles(L"themes", ".toml");
        ASSERT_TRUE(files.has_value()) << "a hand-off reads through its held folder";
        ASSERT_TRUE(files->has_value()) << files->error();
        EXPECT_EQ(namesOf(**files, scratch.path()), (std::vector<std::wstring>{LR"(themes\mine.toml)", LR"(themes\other.toml)"}));
        ASSERT_EQ((*files)->size(), 2U);
        EXPECT_EQ((**files)[0].text, "[meta]\nname = \"Mine\"\n");
        EXPECT_EQ((**files)[1].text, "[meta]\nname = \"Other\"\n");
    }
    removeMountPoint(themes / L"junction.toml");
}

TEST(PinnedFolderTest, HandedOverThemesFolderThatIsMissingOrEmptyIsNoThemes)
{
    const HeldScratchFolder scratch;
    const auto folder = std::make_shared<const PinnedFolder>(scratch.path(), heldHandleOf(scratch));
    const ScopedHandOff handOff(folder);

    const auto missing = Core::ConfigDirOverride::readHandedOverFiles(L"themes", ".toml");
    ASSERT_TRUE(missing.has_value());
    ASSERT_TRUE(missing->has_value()) << missing->error();
    EXPECT_TRUE((*missing)->empty());

    std::filesystem::create_directory(scratch.path() / L"themes");
    const auto empty = Core::ConfigDirOverride::readHandedOverFiles(L"themes", ".toml");
    ASSERT_TRUE(empty.has_value());
    ASSERT_TRUE(empty->has_value()) << empty->error();
    EXPECT_TRUE((*empty)->empty());
}

// The themes folder itself a junction to a folder elsewhere: nothing is listed through it, and the hand-off says so.
TEST(PinnedFolderTest, HandedOverThemesFolderThatIsAJunctionIsAnError)
{
    const TargetFolder target;
    std::ofstream(target.path() / L"elsewhere.toml", std::ios::binary) << "[meta]\nname = \"Elsewhere\"\n";
    const HeldScratchFolder scratch;
    std::filesystem::create_directory(scratch.path() / L"themes");
    if (const auto why = makeMountPoint(scratch.path() / L"themes", target.path()); why.has_value())
    {
        GTEST_SKIP() << "can't make a mount point here: " << *why;
    }
    {
        const auto folder = std::make_shared<const PinnedFolder>(scratch.path(), heldHandleOf(scratch));
        const ScopedHandOff handOff(folder);
        const auto files = Core::ConfigDirOverride::readHandedOverFiles(L"themes", ".toml");
        ASSERT_TRUE(files.has_value());
        EXPECT_FALSE(files->has_value()) << "listed through the junction";
    }
    removeMountPoint(scratch.path() / L"themes");
}

// PinnedFolder reads at most 16 MiB of a file: a larger theme is skipped, and the others are still read.
TEST(PinnedFolderTest, HandedOverThemeLargerThanTaskSmackReadsIsSkipped)
{
    const HeldScratchFolder scratch;
    const std::filesystem::path themes = scratch.path() / L"themes";
    std::filesystem::create_directory(themes);
    std::ofstream(themes / L"mine.toml", std::ios::binary) << "[meta]\nname = \"Mine\"\n";
    {
        std::ofstream huge(themes / L"huge.toml", std::ios::binary);
        const std::string megabyte(std::size_t{1024} * 1024, '#');
        for (int i = 0; i < 17; ++i)
        {
            huge << megabyte;
        }
    }
    const auto folder = std::make_shared<const PinnedFolder>(scratch.path(), heldHandleOf(scratch));
    const ScopedHandOff handOff(folder);
    const auto files = Core::ConfigDirOverride::readHandedOverFiles(L"themes", ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_TRUE(files->has_value()) << files->error();
    EXPECT_EQ(namesOf(**files, scratch.path()), std::vector<std::wstring>{LR"(themes\mine.toml)"});
}

} // namespace
} // namespace Platform::Windows
