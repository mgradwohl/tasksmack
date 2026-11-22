// Tests for Platform::Windows' handle-relative file calls (#4143), which PinnedFolder (#3774) and App::DebugLogFile
// (#4105) share: what each NT call is handed, through fakes, and what each one does for real in a folder of this
// test's own under the temp folder. Nothing here is elevated, and no real user or system folder is touched.

#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Support/Junction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
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

/// Security this unelevated test can set and still remove: no owner (only an elevated token may make Administrators
/// one), and Authenticated Users too.
constexpr std::string_view TEST_SECURITY = "D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;AU)";

/// What the fake NtCreateFile was handed, and what it answers.
struct CreateCall
{
    int calls = 0;
    HANDLE root = nullptr;
    std::wstring name{}; // NOLINT(readability-redundant-member-init) - designated initializers leave it out
    ACCESS_MASK access = 0;
    ULONG share = 0;
    ULONG disposition = 0;
    ULONG options = 0;
    ULONG attributes = 0;
    PVOID security = nullptr;
    NTSTATUS answer = 0;
    ULONG_PTR information = FILE_OPENED;
};

[[nodiscard]] CreateCall& createCall()
{
    static CreateCall call;
    return call;
}

NTSTATUS NTAPI fakeNtCreateFile(PHANDLE handle,
                                ACCESS_MASK access,
                                POBJECT_ATTRIBUTES attributes,
                                PIO_STATUS_BLOCK status,
                                PLARGE_INTEGER /*allocationSize*/,
                                ULONG /*fileAttributes*/,
                                ULONG share,
                                ULONG disposition,
                                ULONG options,
                                PVOID /*eaBuffer*/,
                                ULONG /*eaLength*/)
{
    CreateCall& call = createCall();
    ++call.calls;
    call.root = attributes->RootDirectory;
    call.name.assign(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    call.access = access;
    call.share = share;
    call.disposition = disposition;
    call.options = options;
    call.attributes = attributes->Attributes;
    call.security = attributes->SecurityDescriptor;
    if (call.answer < 0)
    {
        return call.answer;
    }
    *handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // a real handle of this process's own, closed as a file's would be
    status->Information = call.information;
    return call.answer;
}

/// What the fake NtSetInformationFile was handed, and what it answers, call by call.
struct SetCall
{
    std::vector<ULONG> classes{};                  // NOLINT(readability-redundant-member-init) - each call's information class
    std::vector<std::vector<std::byte>> buffers{}; // NOLINT(readability-redundant-member-init) - each call's information
    std::vector<NTSTATUS> answers{};               // NOLINT(readability-redundant-member-init) - answered in turn; then success
};

[[nodiscard]] SetCall& setCall()
{
    static SetCall call;
    return call;
}

NTSTATUS NTAPI fakeNtSetInformationFile(HANDLE /*file*/, PIO_STATUS_BLOCK /*status*/, PVOID information, ULONG length, ULONG what)
{
    SetCall& call = setCall();
    const std::size_t index = call.classes.size();
    call.classes.push_back(what);
    const std::span bytes(static_cast<const std::byte*>(information), length);
    call.buffers.emplace_back(bytes.begin(), bytes.end());
    return index < call.answers.size() ? call.answers[index] : 0;
}

/// The FILE_RENAME_INFO a rename was handed: its first ULONG (the flags), RootDirectory and name.
struct RenameSeen
{
    ULONG flags = 0;
    HANDLE root = nullptr;
    std::wstring name;
};

[[nodiscard]] RenameSeen renameIn(const std::vector<std::byte>& buffer)
{
    RenameSeen seen;
    EXPECT_GE(buffer.size(), sizeof(FILE_RENAME_INFO));
    std::memcpy(&seen.flags, buffer.data(), sizeof(seen.flags));
    FILE_RENAME_INFO info{};
    std::memcpy(&info, buffer.data(), sizeof(info));
    seen.root = info.RootDirectory;
    const std::size_t offset = offsetof(FILE_RENAME_INFO, FileName);
    EXPECT_GE(buffer.size(), offset + info.FileNameLength + sizeof(wchar_t)) << "room for the name and its terminator";
    seen.name.resize(info.FileNameLength / sizeof(wchar_t));
    std::memcpy(seen.name.data(), std::span(buffer).subspan(offset).data(), info.FileNameLength);
    return seen;
}

// What NtCreateFile is handed: the held parent as RootDirectory, the one name, exactly the access asked, a link
// opened itself, and a synchronous handle only when SYNCHRONIZE is asked.
TEST(HandleRelativeFileTest, AnOpenIsRelativeToTheParentWithExactlyTheAccessAsked)
{
    createCall() = CreateCall{};
    const UniqueHandle parent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    auto opened = openRelative(parent.get(), L"Logs", RelativeOpen{.access = DELETE}, &fakeNtCreateFile);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->handle.valid());
    EXPECT_FALSE(opened->created);
    const CreateCall& call = createCall();
    EXPECT_EQ(call.root, parent.get()) << "relative to the held handle, never by a path from the drive";
    EXPECT_EQ(call.name, L"Logs");
    EXPECT_EQ(call.access, static_cast<ACCESS_MASK>(DELETE)) << "nothing added, so the parent's FILE_DELETE_CHILD grants it";
    EXPECT_EQ(call.share, static_cast<ULONG>(FILE_SHARE_READ | FILE_SHARE_WRITE)) << "never FILE_SHARE_DELETE by default";
    EXPECT_EQ(call.disposition, static_cast<ULONG>(FILE_OPEN));
    EXPECT_EQ(call.options, static_cast<ULONG>(FILE_OPEN_REPARSE_POINT)) << "a link opened itself; not synchronous; either kind";
    EXPECT_EQ(call.attributes, static_cast<ULONG>(OBJ_CASE_INSENSITIVE));
    EXPECT_EQ(call.security, nullptr);
}

TEST(HandleRelativeFileTest, AnOpenAsksForTheKindAndSynchronousIoItNeedsAndAppliesItsSecurity)
{
    createCall() = CreateCall{.information = FILE_CREATED};
    int descriptor = 0; // stands in for a security descriptor: only its address is handed on
    auto folder = openRelative(nullptr,
                               L"TaskSmack",
                               RelativeOpen{
                                   .access = FILE_LIST_DIRECTORY | SYNCHRONIZE,
                                   .share = FILE_SHARE_READ,
                                   .disposition = FILE_CREATE,
                                   .kind = EntryKind::Directory,
                                   .security = &descriptor,
                               },
                               &fakeNtCreateFile);
    ASSERT_TRUE(folder.has_value());
    EXPECT_TRUE(folder->created) << "IO_STATUS_BLOCK.Information said FILE_CREATED";
    EXPECT_EQ(createCall().options, static_cast<ULONG>(FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE));
    EXPECT_EQ(createCall().share, static_cast<ULONG>(FILE_SHARE_READ));
    EXPECT_EQ(createCall().disposition, static_cast<ULONG>(FILE_CREATE));
    EXPECT_EQ(createCall().security, &descriptor);

    createCall() = CreateCall{};
    const auto file = openRelative(nullptr, L"x.log", RelativeOpen{.kind = EntryKind::File}, &fakeNtCreateFile);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(createCall().options, static_cast<ULONG>(FILE_OPEN_REPARSE_POINT | FILE_NON_DIRECTORY_FILE));
}

TEST(HandleRelativeFileTest, AFailedOpenAnswersItsNtStatus)
{
    createCall() = CreateCall{.answer = NT_OBJECT_NAME_NOT_FOUND};
    const auto missing = openRelative(nullptr, L"x", RelativeOpen{}, &fakeNtCreateFile);
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(isNotFound(missing.error()));
    EXPECT_TRUE(isNotFound(NT_OBJECT_PATH_NOT_FOUND));
    EXPECT_FALSE(isNotFound(NT_OBJECT_NAME_COLLISION));
    EXPECT_EQ(ntStatusText(NT_OBJECT_NAME_COLLISION), "NTSTATUS 0xC0000035");
}

TEST(HandleRelativeFileTest, ARenameHandsTheFlagsRootAndNameInOneBuffer)
{
    setCall() = SetCall{};
    const UniqueHandle root(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const NTSTATUS renamed =
        renameRelative(nullptr, root.get(), L"config.toml", FILE_RENAME_INFORMATION_EX_CLASS, 3, &fakeNtSetInformationFile);
    EXPECT_EQ(renamed, 0);
    ASSERT_EQ(setCall().classes, std::vector<ULONG>{FILE_RENAME_INFORMATION_EX_CLASS});
    const RenameSeen seen = renameIn(setCall().buffers.front());
    EXPECT_EQ(seen.flags, 3U) << "the flags are the first ULONG";
    EXPECT_EQ(seen.root, root.get());
    EXPECT_EQ(seen.name, L"config.toml");
}

// A taken name is tried again under another; anything else (held open, an oplock, access) stops at once.
TEST(HandleRelativeFileTest, AMoveAsideTriesAnotherNameOnlyWhenTheNameIsTaken)
{
    setCall() = SetCall{.answers = {NT_OBJECT_NAME_COLLISION, 0}};
    EXPECT_TRUE(renameAside(nullptr, nullptr, L"TaskSmack", &fakeNtSetInformationFile).has_value());
    ASSERT_EQ(setCall().buffers.size(), 2U);
    for (const auto& buffer : setCall().buffers)
    {
        const RenameSeen seen = renameIn(buffer);
        EXPECT_EQ(seen.flags, 0U) << "never replaces what is there";
        EXPECT_TRUE(seen.name.starts_with(L"TaskSmack.untrusted-")) << std::filesystem::path(seen.name).string();
        EXPECT_EQ(seen.name.size(), std::wstring_view(L"TaskSmack.untrusted-").size() + 8);
    }
    EXPECT_EQ(setCall().classes.front(), FILE_RENAME_INFORMATION_CLASS);

    constexpr auto ACCESS_DENIED = static_cast<NTSTATUS>(0xC0000022L);
    setCall() = SetCall{.answers = {ACCESS_DENIED}};
    const auto refused = renameAside(nullptr, nullptr, L"TaskSmack", &fakeNtSetInformationFile);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), ACCESS_DENIED);
    EXPECT_EQ(setCall().classes.size(), 1U) << "another name won't help";

    setCall() = SetCall{.answers = std::vector<NTSTATUS>(8, NT_OBJECT_NAME_COLLISION)};
    const auto taken = renameAside(nullptr, nullptr, L"TaskSmack", &fakeNtSetInformationFile);
    ASSERT_FALSE(taken.has_value());
    EXPECT_EQ(taken.error(), NT_OBJECT_NAME_COLLISION);
    EXPECT_EQ(setCall().classes.size(), 8U) << "a bounded number of tries";
}

// POSIX semantics first, then the classic flag where the volume doesn't take them.
TEST(HandleRelativeFileTest, ADeleteFallsBackToTheClassicFlag)
{
    constexpr ULONG DISPOSITION_EX = 64;
    constexpr ULONG DISPOSITION = 13;
    constexpr auto NOT_SUPPORTED = static_cast<NTSTATUS>(0xC00000BBL);
    setCall() = SetCall{};
    EXPECT_TRUE(deleteThroughHandle(nullptr, &fakeNtSetInformationFile));
    EXPECT_EQ(setCall().classes, std::vector<ULONG>{DISPOSITION_EX});

    setCall() = SetCall{.answers = {NOT_SUPPORTED}};
    EXPECT_TRUE(deleteThroughHandle(nullptr, &fakeNtSetInformationFile));
    EXPECT_EQ(setCall().classes, (std::vector<ULONG>{DISPOSITION_EX, DISPOSITION}));

    setCall() = SetCall{.answers = {NOT_SUPPORTED, NOT_SUPPORTED}};
    EXPECT_FALSE(deleteThroughHandle(nullptr, &fakeNtSetInformationFile));
}

DWORD WINAPI uncFinalPath(HANDLE /*handle*/, LPWSTR buffer, DWORD size, DWORD /*flags*/)
{
    constexpr std::wstring_view PATH = LR"(\\?\UNC\server\share\TaskSmack)";
    if (size <= PATH.size())
    {
        return static_cast<DWORD>(PATH.size() + 1);
    }
    std::ranges::copy(PATH, buffer);
    std::span(buffer, size)[PATH.size()] = L'\0';
    return static_cast<DWORD>(PATH.size());
}

TEST(HandleRelativeFileTest, AFinalPathKeepsTheUncPrefix)
{
    EXPECT_EQ(finalPathOf(nullptr, &uncFinalPath), std::optional<std::wstring>(LR"(\\?\UNC\server\share\TaskSmack)"));
    EXPECT_EQ(finalPathOf(nullptr), std::nullopt) << "no handle, no path";
}

TEST(HandleRelativeFileTest, ASecurityDescriptorIsBuiltOnlyFromValidSddl)
{
    EXPECT_NE(securityDescriptorFrom(TEST_SECURITY), nullptr);
    EXPECT_EQ(securityDescriptorFrom("not SDDL"), nullptr);
}

// The real calls, in a folder of this test's own.

/// A folder of this test's own under the temp folder, as Windows names it; removed at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"TaskSmack.HandleRelativeFileTest.{}", GetCurrentProcessId()))
    {
        std::error_code error;
        std::filesystem::remove_all(m_Path, error);
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

/// The names in @p folder.
[[nodiscard]] std::vector<std::wstring> entriesOf(const std::filesystem::path& folder)
{
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        names.push_back(entry.path().filename().wstring());
    }
    std::ranges::sort(names);
    return names;
}

TEST(HandleRelativeFileTest, AHeldFolderOpensCreatesMovesAsideAndDeletesForReal)
{
    const ScratchFolder scratch;
    auto parent = openFolderByPath(scratch.path());
    ASSERT_TRUE(parent.has_value()) << parent.error();
    EXPECT_EQ(isPlainFolder(parent->get()), std::optional(true));
    EXPECT_EQ(finalPathOf(parent->get()), std::optional(scratch.path().native()));

    const auto missing = openRelative(parent->get(), L"absent.log", RelativeOpen{.access = DELETE}, &systemNtCreateFile);
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(isNotFound(missing.error()));

    {
        auto made = openRelative(parent->get(),
                                 L"made.log",
                                 RelativeOpen{
                                     .access = GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                     .disposition = FILE_CREATE,
                                     .kind = EntryKind::File,
                                 });
        ASSERT_TRUE(made.has_value()) << ntStatusText(made.error());
        EXPECT_TRUE(made->created);
        EXPECT_EQ(isPlainFolder(made->handle.get()), std::optional(false));
        const auto again = openRelative(
            parent->get(), L"made.log", RelativeOpen{.access = GENERIC_WRITE, .disposition = FILE_CREATE, .kind = EntryKind::File});
        ASSERT_FALSE(again.has_value()) << "CREATE-only never opens one that is there";
        EXPECT_EQ(again.error(), NT_OBJECT_NAME_COLLISION);
    }
    EXPECT_EQ(entriesOf(scratch.path()), std::vector<std::wstring>{L"made.log"});

    {
        auto removable = openRelative(parent->get(), L"made.log", RelativeOpen{.access = DELETE});
        ASSERT_TRUE(removable.has_value()) << ntStatusText(removable.error());
        ASSERT_TRUE(renameAside(removable->handle.get(), parent->get(), L"made.log").has_value());
    }
    auto moved = entriesOf(scratch.path());
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_TRUE(moved.front().starts_with(L"made.log.untrusted-")) << std::filesystem::path(moved.front()).string();

    {
        auto stale = openRelative(parent->get(), moved.front(), RelativeOpen{.access = DELETE});
        ASSERT_TRUE(stale.has_value()) << ntStatusText(stale.error());
        EXPECT_TRUE(deleteThroughHandle(stale->handle.get()));
    }
    EXPECT_TRUE(entriesOf(scratch.path()).empty()) << "deleted through its handle";

    {
        auto staging =
            openRelative(parent->get(),
                         L"staging.tmp",
                         RelativeOpen{.access = GENERIC_WRITE | DELETE | SYNCHRONIZE, .disposition = FILE_CREATE, .kind = EntryKind::File});
        ASSERT_TRUE(staging.has_value()) << ntStatusText(staging.error());
        markDeleteOnClose(staging->handle.get());
    }
    EXPECT_TRUE(entriesOf(scratch.path()).empty()) << "deleted when its handle closed";
}

// A junction is opened itself, never the folder it names, so isPlainFolder() refuses it; so is a file.
TEST(HandleRelativeFileTest, AFolderByPathOpensAJunctionItselfNeverWhatItNames)
{
    const ScratchFolder scratch;
    const std::filesystem::path target = scratch.path() / L"target";
    const std::filesystem::path link = scratch.path() / L"link";
    std::filesystem::create_directory(target);
    std::filesystem::create_directory(link);
    const std::optional<std::string> notMade = ::TestSupport::makeJunction(link, target);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});
    {
        auto junction = openFolderByPath(link);
        ASSERT_TRUE(junction.has_value()) << junction.error();
        EXPECT_EQ(isPlainFolder(junction->get()), std::optional(false)) << "the junction itself, a reparse point";
        EXPECT_EQ(finalPathOf(junction->get()), std::optional(link.native())) << "not the folder it names";
    }
    ASSERT_TRUE(::TestSupport::removeJunction(link)) << "an empty folder again, so removing it removes nothing behind it";

    std::ofstream(scratch.path() / L"file.txt", std::ios::binary) << "x\n";
    auto file = openFolderByPath(scratch.path() / L"file.txt");
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_EQ(isPlainFolder(file->get()), std::optional(false)) << "callers check it is a plain folder";
    const auto missing = openFolderByPath(scratch.path() / L"absent");
    EXPECT_FALSE(missing.has_value());
}

// A child folder: made fresh, taken over in place when its owner is trusted, or moved aside and remade.
TEST(HandleRelativeFileTest, AChildFolderIsMadeTakenOverOrMovedAside)
{
    const ScratchFolder scratch;
    const auto security = securityDescriptorFrom(TEST_SECURITY);
    ASSERT_NE(security, nullptr);
    auto parent = openFolderByPath(scratch.path());
    ASSERT_TRUE(parent.has_value()) << parent.error();
    const std::string display = "the test folder";
    const OwnerTrustedFn trusted = [](HANDLE)
    {
        return std::optional(true);
    };
    const OwnerTrustedFn untrusted = [](HANDLE)
    {
        return std::optional(false);
    };

    {
        auto fresh = secureChildFolder(parent->get(), L"TaskSmack", security.get(), display, trusted);
        ASSERT_TRUE(fresh.has_value()) << fresh.error();
        EXPECT_EQ(isPlainFolder(fresh->get()), std::optional(true));
    }
    std::ofstream(scratch.path() / L"TaskSmack" / L"kept.txt", std::ios::binary) << "x\n";
    {
        auto kept = secureChildFolder(parent->get(), L"TaskSmack", security.get(), display, trusted);
        ASSERT_TRUE(kept.has_value()) << kept.error();
    }
    EXPECT_TRUE(std::filesystem::exists(scratch.path() / L"TaskSmack" / L"kept.txt")) << "taken over in place";
    EXPECT_EQ(entriesOf(scratch.path()), std::vector<std::wstring>{L"TaskSmack"});
    {
        auto remade = secureChildFolder(parent->get(), L"TaskSmack", security.get(), display, untrusted);
        ASSERT_TRUE(remade.has_value()) << remade.error();
    }
    EXPECT_FALSE(std::filesystem::exists(scratch.path() / L"TaskSmack" / L"kept.txt")) << "a fresh folder";
    const auto entries = entriesOf(scratch.path());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front(), L"TaskSmack");
    EXPECT_TRUE(entries.back().starts_with(L"TaskSmack.untrusted-")) << "the untrusted one moved aside, not reused";
    EXPECT_TRUE(std::filesystem::exists(scratch.path() / entries.back() / L"kept.txt"));

    // A planted file where the folder should be is moved aside too.
    std::ofstream(scratch.path() / L"Logs", std::ios::binary) << "planted\n";
    {
        auto logs = secureChildFolder(parent->get(), L"Logs", security.get(), display, trusted);
        ASSERT_TRUE(logs.has_value()) << logs.error();
        EXPECT_EQ(isPlainFolder(logs->get()), std::optional(true));
    }
    EXPECT_TRUE(std::filesystem::is_directory(scratch.path() / L"Logs"));
}

TEST(HandleRelativeFileTest, AChildFolderThatCantBeMadeSaysWhich)
{
    const auto security = securityDescriptorFrom(TEST_SECURITY);
    // No parent: NtCreateFile refuses a relative name without a RootDirectory.
    const auto refused = secureChildFolder(nullptr, L"TaskSmack", security.get(), "the test folder");
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().starts_with("the test folder can't be made: NTSTATUS 0x")) << refused.error();
}

/// GetFinalPathNameByHandleW answering that it needs one more character than it was given, every time.
DWORD WINAPI neverFitsFinalPath(HANDLE /*handle*/, LPWSTR /*path*/, DWORD size, DWORD /*flags*/)
{
    return size + 1;
}

// A path that still doesn't fit once the buffer has been made as long as the first answer said is no path, not a
// guess or a loop.
TEST(HandleRelativeFileFailureTest, APathThatNeverFitsItsBufferIsNoPath)
{
    EXPECT_EQ(finalPathOf(nullptr, &neverFitsFinalPath), std::nullopt);
}

// A handle that can't be asked is neither a plain folder nor a trusted owner's: both answer "don't know", which
// callers treat as a refusal, never as yes.
TEST(HandleRelativeFileFailureTest, AHandleThatCantBeAskedIsNeitherAPlainFolderNorATrustedOwners)
{
    EXPECT_EQ(isPlainFolder(nullptr), std::nullopt);
    EXPECT_EQ(isPlainFolder(INVALID_HANDLE_VALUE), std::nullopt);
    EXPECT_EQ(ownerIsSystemOrAdministratorsOrTrustedInstaller(nullptr), std::nullopt);
}

} // namespace
} // namespace Platform::Windows
