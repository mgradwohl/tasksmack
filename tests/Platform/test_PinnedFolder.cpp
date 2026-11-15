// Tests for PinnedFolder (#3774): every file TaskSmack opens or creates in a handed-over config folder is opened
// relative to the held folder's handle and checked to be inside it. The refusals run on fakes (a folder that becomes
// a reparse point after the pin, an open whose final path is elsewhere); the opens themselves run for real in a
// folder of this test's own under the temp folder.

#include "Platform/Windows/PinnedFolder.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
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

/// @p scratch held with every real call, as checkConfigDir() holds a folder.
[[nodiscard]] PinnedFolder holdReally(const HeldScratchFolder& scratch)
{
    UniqueHandle held(CreateFileW(scratch.path().c_str(),
                                  FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    EXPECT_TRUE(held) << GetLastError();
    return {scratch.path(), std::move(held)};
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

} // namespace
} // namespace Platform::Windows
