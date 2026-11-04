// Find handles (#3067): matching, counting and the walk over every process, against fake readers.

#include "AllocationFailureHook.h"
#include "Domain/HandleSearch.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace Domain::HandleSearch;

/// run()'s generations, committed to @p slot one by one, as the dialog's worker publishes them.
[[nodiscard]] Publish publishTo(Domain::PublicationSlot<SearchPublication>& slot)
{
    return [&slot](SearchPublication generation)
    {
        slot.commit(std::make_shared<const SearchPublication>(std::move(generation)));
    };
}

[[nodiscard]] SearchProcess process(std::int32_t pid, std::string name)
{
    return SearchProcess{.target = {.pid = pid, .startTimeTicks = 100U + static_cast<std::uint64_t>(pid)}, .name = std::move(name)};
}

[[nodiscard]] Platform::OpenFilesReadResult filesStatus(Platform::OpenFilesReadStatus status)
{
    Platform::OpenFilesReadResult result;
    result.status = status;
    return result;
}

[[nodiscard]] Platform::ModulesReadResult modulesStatus(Platform::ModulesReadStatus status)
{
    Platform::ModulesReadResult result;
    result.status = status;
    return result;
}

[[nodiscard]] Platform::OpenFile file(Platform::OpenFileKind kind, std::string path)
{
    Platform::OpenFile open;
    open.kind = kind;
    open.path = std::move(path);
    return open;
}

[[nodiscard]] Platform::OpenFilesReadResult openFiles(std::vector<Platform::OpenFile> files)
{
    Platform::OpenFilesReadResult result = filesStatus(Platform::OpenFilesReadStatus::Ok);
    result.files = std::move(files);
    return result;
}

[[nodiscard]] Platform::ModulesReadResult modules(std::vector<std::string> paths)
{
    Platform::ModulesReadResult result = modulesStatus(Platform::ModulesReadStatus::Ok);
    for (std::string& path : paths)
    {
        Platform::ProcessModule module;
        module.path = std::move(path);
        result.modules.push_back(std::move(module));
    }
    return result;
}

/// Answers each PID with what the test set; a PID it wasn't given is denied, and readAuthorized() reads
/// it as Ok with the files set in `authorizedFiles`.
class FakeOpenFiles final : public Platform::IProcessOpenFilesReader
{
  public:
    std::map<std::int32_t, Platform::OpenFilesReadResult> results;
    std::map<std::int32_t, Platform::OpenFilesReadResult> authorizedFiles;
    std::vector<std::int32_t> read;
    int authorizedReads = 0;

    [[nodiscard]] bool hasOpenFiles() const override
    {
        return true;
    }

    [[nodiscard]] Platform::OpenFilesReadResult readOpenFiles(const Platform::ProcessTarget& target) override
    {
        read.push_back(target.pid);
        const auto found = results.find(target.pid);
        return found != results.end() ? found->second : filesStatus(Platform::OpenFilesReadStatus::PermissionDenied);
    }

    [[nodiscard]] Platform::OpenFilesReadResult readAuthorized(const Platform::ProcessTarget& target,
                                                               const std::stop_token& /*stopToken*/) override
    {
        ++authorizedReads;
        const auto found = authorizedFiles.find(target.pid);
        return found != authorizedFiles.end() ? found->second : filesStatus(Platform::OpenFilesReadStatus::Failed);
    }
};

class FakeModules final : public Platform::IProcessModulesReader
{
  public:
    std::map<std::int32_t, Platform::ModulesReadResult> results;

    [[nodiscard]] bool hasModules() const override
    {
        return true;
    }

    [[nodiscard]] Platform::ModulesReadResult readModules(const Platform::ProcessTarget& target) override
    {
        const auto found = results.find(target.pid);
        return found != results.end() ? found->second : modulesStatus(Platform::ModulesReadStatus::PermissionDenied);
    }
};

TEST(HandleSearchTest, PathMatchesIgnoresAsciiCaseAndAnEmptyQueryMatchesNothing)
{
    EXPECT_TRUE(pathMatches("/home/me/Report.ODS", lowered("report.ods")));
    EXPECT_TRUE(pathMatches("/home/me/report.ods", lowered("ME/REP")));
    EXPECT_FALSE(pathMatches("/home/me/report.ods", lowered("reports")));
    EXPECT_FALSE(pathMatches("/home/me/report.ods", ""));
    EXPECT_FALSE(pathMatches("", "a"));
    // Bytes outside ASCII are compared as they are.
    EXPECT_TRUE(pathMatches("/tmp/caf\xC3\xA9.txt", lowered("CAF\xC3\xA9")));
}

TEST(HandleSearchTest, KindsFollowTheOpenFileKindAndModulesAreLibraries)
{
    EXPECT_EQ(kindOf(Platform::OpenFileKind::File), HandleKind::File);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Memfd), HandleKind::File);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Directory), HandleKind::Directory);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Socket), HandleKind::Socket);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Pipe), HandleKind::Pipe);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Device), HandleKind::Device);
    EXPECT_EQ(kindOf(Platform::OpenFileKind::AnonInode), HandleKind::Other);
    EXPECT_EQ(kindLabel(HandleKind::Directory), "Folder");
    EXPECT_EQ(kindLabel(HandleKind::Library), "Library");
}

TEST(HandleSearchTest, EveryKindHasItsLabelAndAnUnclassifiedFileIsOther)
{
    EXPECT_EQ(kindOf(Platform::OpenFileKind::Other), HandleKind::Other);
    EXPECT_EQ(kindLabel(HandleKind::File), "File");
    EXPECT_EQ(kindLabel(HandleKind::Socket), "Socket");
    EXPECT_EQ(kindLabel(HandleKind::Pipe), "Pipe");
    EXPECT_EQ(kindLabel(HandleKind::Device), "Device");
    EXPECT_EQ(kindLabel(HandleKind::Other), "Other");
}

TEST(HandleSearchTest, AddProcessKeepsTruncatedOnceEitherReadWasCut)
{
    SearchPublication into;
    Platform::ModulesReadResult cutModules = modules({"/usr/lib/libz.so"});
    cutModules.truncated = true;
    addProcess(process(1, "a"), openFiles({}), cutModules, "libz", into);
    EXPECT_TRUE(into.truncated);
    ASSERT_EQ(into.matches.size(), 1U);

    // Already truncated: a later process whose reads were whole doesn't clear it.
    addProcess(process(2, "b"), openFiles({}), modules({}), "libz", into);
    EXPECT_TRUE(into.truncated);

    SearchPublication filesCut;
    Platform::OpenFilesReadResult cutFiles = openFiles({file(Platform::OpenFileKind::Socket, "socket:[42]")});
    cutFiles.truncated = true;
    addProcess(process(3, "c"), cutFiles, modules({}), "socket", filesCut);
    EXPECT_TRUE(filesCut.truncated);
    ASSERT_EQ(filesCut.matches.size(), 1U);
    EXPECT_EQ(filesCut.matches[0].kind, HandleKind::Socket);
}

TEST(HandleSearchTest, AddProcessCountsByWhicheverReaderSaidSo)
{
    SearchPublication into;
    // The modules alone read: listed, not counted as a failure.
    addProcess(process(1, "mods"), filesStatus(Platform::OpenFilesReadStatus::Failed), modules({"/lib/x.so"}), "x.so", into);
    EXPECT_EQ(into.matches.size(), 1U);
    EXPECT_EQ(into.failed, 0U);

    // The modules reader saw the exit, the files reader only failed.
    addProcess(process(2, "gone"),
               filesStatus(Platform::OpenFilesReadStatus::Failed),
               modulesStatus(Platform::ModulesReadStatus::ProcessExited),
               "x",
               into);
    EXPECT_EQ(into.exited, 1U);

    // The modules reader was refused, the files reader only failed.
    addProcess(process(3, "locked"),
               filesStatus(Platform::OpenFilesReadStatus::Failed),
               modulesStatus(Platform::ModulesReadStatus::PermissionDenied),
               "x",
               into);
    ASSERT_EQ(into.denied.size(), 1U);
    EXPECT_EQ(into.denied[0].target.pid, 3);

    // No open-files reader, but the modules read failed: a failure.
    addProcess(process(4, "odd"),
               filesStatus(Platform::OpenFilesReadStatus::Unsupported),
               modulesStatus(Platform::ModulesReadStatus::Failed),
               "x",
               into);
    EXPECT_EQ(into.failed, 1U);
    EXPECT_EQ(into.searched, 4U);
}

TEST(HandleSearchTest, RunStopsOnceTheMatchCapIsReached)
{
    FakeModules mods;
    std::vector<std::string> paths(MAX_MATCHES, "/usr/lib/libcap.so");
    mods.results[1] = modules(std::move(paths));
    mods.results[2] = modules({"/usr/lib/libcap.so"});
    const std::vector<SearchProcess> processes{process(1, "full"), process(2, "never")};
    Domain::PublicationSlot<SearchPublication> slot;
    const std::stop_source stop;
    // No open-files reader: the modules alone are searched.
    run(processes,
        {.openFiles = nullptr, .modules = &mods},
        false,
        startFor("libcap", processes.size(), 1),
        publishTo(slot),
        stop.get_token());

    const std::shared_ptr<const SearchPublication> last = slot.load();
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->done);
    EXPECT_EQ(last->searched, 1U); // the second process was never read
    EXPECT_EQ(last->matches.size(), MAX_MATCHES);
    // #3299: the first process brought exactly the last match the cap allows, so nothing set truncated while
    // adding; the processes left unread make the result the first matches, and it says so.
    EXPECT_TRUE(last->truncated);
}

// #3299: an administrator search on a result already at the cap searches none of the denied processes; they
// leave the list unsearched, and the result says it is cut short rather than complete.
TEST(HandleSearchTest, AnAdministratorSearchOnAFullResultIsMarkedTruncated)
{
    FakeOpenFiles files;
    files.results[1] = openFiles(std::vector<Platform::OpenFile>(MAX_MATCHES, file(Platform::OpenFileKind::File, "/srv/data.db")));
    files.authorizedFiles[2] = openFiles({file(Platform::OpenFileKind::File, "/srv/data.db-wal")});
    // postgres is read first (denied), then mine fills the cap.
    std::vector<SearchProcess> processes{process(2, "postgres"), process(1, "mine")};
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("data.db", processes.size(), 1),
        publishTo(slot),
        stop.get_token());
    const std::shared_ptr<const SearchPublication> first = slot.load();
    ASSERT_EQ(first->matches.size(), MAX_MATCHES);
    ASSERT_EQ(first->denied.size(), 1U);

    const std::vector<SearchProcess> denied = first->denied;
    run(denied,
        {.openFiles = &files, .modules = nullptr},
        true,
        startAuthorizedAfter(*first, first->version + 1),
        publishTo(slot),
        stop.get_token());
    const std::shared_ptr<const SearchPublication> second = slot.load();
    EXPECT_TRUE(second->done);
    EXPECT_TRUE(second->truncated);
    EXPECT_EQ(files.authorizedReads, 0);
}

TEST(HandleSearchTest, AnAuthorizedRunReadsTheModulesAuthorizedToo)
{
    FakeOpenFiles files;
    files.authorizedFiles[5] = openFiles({});
    FakeModules mods;
    mods.results[5] = modules({"/opt/app/libsecret.so"});
    const std::vector<SearchProcess> processes{process(5, "daemon")};
    Domain::PublicationSlot<SearchPublication> slot;
    const std::stop_source stop;
    run(processes,
        {.openFiles = &files, .modules = &mods},
        true,
        startFor("secret", processes.size(), 1),
        publishTo(slot),
        stop.get_token());

    const std::shared_ptr<const SearchPublication> last = slot.load();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(files.authorizedReads, 1);
    ASSERT_EQ(last->matches.size(), 1U);
    EXPECT_EQ(last->matches[0].kind, HandleKind::Library);
}

TEST(HandleSearchTest, AddProcessListsMatchingFilesAndModules)
{
    SearchPublication into;
    addProcess(process(7, "soffice"),
               openFiles({file(Platform::OpenFileKind::File, "/home/me/report.ods"),
                          file(Platform::OpenFileKind::File, "/home/me/notes.txt"),
                          file(Platform::OpenFileKind::Directory, "/home/me")}),
               modules({"/usr/lib/libreport.so", "/usr/lib/libc.so.6"}),
               "report",
               into);
    ASSERT_EQ(into.matches.size(), 2U);
    EXPECT_EQ(into.matches[0].path, "/home/me/report.ods");
    EXPECT_EQ(into.matches[0].kind, HandleKind::File);
    EXPECT_EQ(into.matches[0].process, "soffice");
    EXPECT_EQ(into.matches[0].target.pid, 7);
    EXPECT_EQ(into.matches[1].path, "/usr/lib/libreport.so");
    EXPECT_EQ(into.matches[1].kind, HandleKind::Library);
    EXPECT_EQ(into.searched, 1U);
    EXPECT_TRUE(into.denied.empty());
}

TEST(HandleSearchTest, AddProcessCountsDeniedExitedAndFailedReads)
{
    SearchPublication into;
    const Platform::OpenFilesReadResult deniedFiles = filesStatus(Platform::OpenFilesReadStatus::PermissionDenied);
    const Platform::ModulesReadResult deniedModules = modulesStatus(Platform::ModulesReadStatus::PermissionDenied);
    addProcess(process(1, "init"), deniedFiles, deniedModules, "x", into);
    addProcess(process(2, "gone"), filesStatus(Platform::OpenFilesReadStatus::ProcessExited), deniedModules, "x", into);
    addProcess(process(3, "odd"),
               filesStatus(Platform::OpenFilesReadStatus::Failed),
               modulesStatus(Platform::ModulesReadStatus::Failed),
               "x",
               into);
    // Neither reader exists in this run: nothing to count.
    addProcess(process(4, "none"), {}, {}, "x", into);
    EXPECT_EQ(into.searched, 4U);
    ASSERT_EQ(into.denied.size(), 1U);
    EXPECT_EQ(into.denied[0].target.pid, 1);
    EXPECT_EQ(into.exited, 1U);
    EXPECT_EQ(into.failed, 1U);
}

TEST(HandleSearchTest, AddProcessStopsAtTheMatchCap)
{
    SearchPublication into;
    std::vector<Platform::OpenFile> files(MAX_MATCHES + 5, file(Platform::OpenFileKind::File, "/tmp/lock"));
    addProcess(process(9, "many"), openFiles(std::move(files)), {}, "lock", into);
    EXPECT_EQ(into.matches.size(), MAX_MATCHES);
    EXPECT_TRUE(into.truncated);
}

TEST(HandleSearchTest, RunWalksEveryProcessAndPublishesBatchesThenDone)
{
    FakeOpenFiles files;
    FakeModules mods;
    std::vector<SearchProcess> processes;
    for (std::int32_t pid = 1; pid <= 40; ++pid)
    {
        processes.push_back(process(pid, "p" + std::to_string(pid)));
        files.results[pid] = openFiles({file(Platform::OpenFileKind::File, "/tmp/file-" + std::to_string(pid))});
        mods.results[pid] = modules({});
    }
    files.results.erase(40); // denied
    mods.results.erase(40);
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;
    run(processes,
        {.openFiles = &files, .modules = &mods},
        false,
        startFor("FILE-1", processes.size(), 5),
        publishTo(slot),
        stop.get_token());

    const std::shared_ptr<const SearchPublication> last = slot.load();
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->done);
    EXPECT_EQ(last->query, "FILE-1");
    EXPECT_EQ(last->total, 40U);
    EXPECT_EQ(last->searched, 40U);
    // file-1 and file-10 .. file-19
    EXPECT_EQ(last->matches.size(), 11U);
    ASSERT_EQ(last->denied.size(), 1U);
    EXPECT_EQ(last->denied[0].target.pid, 40);
    // Two batches of 16, then the final generation, each one version on from the start.
    EXPECT_EQ(last->version, 5U + 3U);
    EXPECT_EQ(files.read.size(), 40U);
}

TEST(HandleSearchTest, RunStopsBetweenProcessesWhenAsked)
{
    FakeOpenFiles files;
    std::vector<SearchProcess> processes{process(1, "a"), process(2, "b")};
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;
    stop.request_stop();
    run(processes, {.openFiles = &files, .modules = nullptr}, false, startFor("x", processes.size(), 1), publishTo(slot), stop.get_token());
    ASSERT_NE(slot.load(), nullptr);
    EXPECT_TRUE(slot.load()->done);
    EXPECT_EQ(slot.load()->searched, 0U);
    EXPECT_TRUE(files.read.empty());
}

/// Reads like FakeOpenFiles, but throws on one PID: an exception it chooses, as a reader or an allocation might.
class ThrowingOpenFiles final : public Platform::IProcessOpenFilesReader
{
  public:
    std::int32_t throwOn = 0;
    int kind = 0; ///< 0: std::runtime_error, 1: std::bad_alloc, 2: not a std::exception

    [[nodiscard]] bool hasOpenFiles() const override
    {
        return true;
    }

    [[nodiscard]] Platform::OpenFilesReadResult readOpenFiles(const Platform::ProcessTarget& target) override
    {
        if (target.pid == throwOn)
        {
            if (kind == 1)
            {
                throw std::bad_alloc();
            }
            if (kind == 2)
            {
                throw 42; // NOLINT(hicpp-exception-baseclass) - an exception that isn't a std::exception
            }
            throw std::runtime_error("injected reader failure");
        }
        return openFiles({file(Platform::OpenFileKind::File, "/tmp/file-" + std::to_string(target.pid))});
    }

    [[nodiscard]] Platform::OpenFilesReadResult readAuthorized(const Platform::ProcessTarget& target,
                                                               const std::stop_token& /*stopToken*/) override
    {
        return readOpenFiles(target);
    }
};

// #3335: a reader that throws, after a progress publication or before the first, or an allocation that fails
// (std::bad_alloc), or something that isn't a std::exception, ends the walk with a done publication that says
// why. run() returns instead of letting it escape the worker, and a search after it runs as usual.
TEST(HandleSearchTest, AReaderOrAllocationFailureEndsTheWalkAsDoneWithItsReason)
{
    std::vector<SearchProcess> processes;
    for (std::int32_t pid = 1; pid <= 20; ++pid)
    {
        processes.push_back(process(pid, "p" + std::to_string(pid)));
    }
    ThrowingOpenFiles files;
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;

    files.throwOn = 18; // after the first progress publication (16 processes)
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("file-1", processes.size(), 1),
        publishTo(slot),
        stop.get_token());
    std::shared_ptr<const SearchPublication> last = slot.load();
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->done);
    EXPECT_EQ(last->failure, "The search stopped: injected reader failure");
    EXPECT_EQ(last->searched, 17U) << "what it read before the failure is kept";
    EXPECT_EQ(last->matches.size(), 9U) << "file-1 and file-10 .. file-17";
    EXPECT_EQ(last->version, 1U + 2U) << "one progress publication, then the done one";

    files.throwOn = 1; // before any progress publication
    files.kind = 1;
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("file", processes.size(), 10),
        publishTo(slot),
        stop.get_token());
    last = slot.load();
    ASSERT_TRUE(last->done);
    EXPECT_TRUE(last->failure.starts_with("The search stopped: ")) << last->failure;
    EXPECT_GT(last->failure.size(), std::string_view("The search stopped: ").size());
    EXPECT_EQ(last->searched, 0U);
    EXPECT_EQ(last->version, 11U);

    files.kind = 2;
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("file", processes.size(), 20),
        publishTo(slot),
        stop.get_token());
    EXPECT_EQ(slot.load()->failure, "The search stopped: an unknown error");

    // A search after the failures runs to its end, with no failure.
    files.throwOn = 0;
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("file-2", processes.size(), 30),
        publishTo(slot),
        stop.get_token());
    last = slot.load();
    EXPECT_TRUE(last->done);
    EXPECT_TRUE(last->failure.empty()) << last->failure;
    EXPECT_EQ(last->searched, 20U);
    EXPECT_EQ(last->matches.size(), 2U) << "file-2 and file-20";
}

// #3335, with real allocation failures (AllocationFailureHook): each allocation run() makes is failed in turn, the
// lowered query's, a match's, a progress publication's and the done publication's. run() never lets one out. The
// slot ends either done, saying why when the walk failed, or, when the done publication itself couldn't be made,
// at the last one it could, for the dialog to end the search when its job does.
TEST(HandleSearchTest, EveryAllocationRunMakesCanFailWithoutEscaping)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    std::vector<SearchProcess> processes;
    FakeOpenFiles files;
    for (std::int32_t pid = 1; pid <= 20; ++pid)
    {
        processes.push_back(process(pid, "p" + std::to_string(pid)));
        files.results[pid] = openFiles({file(Platform::OpenFileKind::File, "/tmp/a-much-longer-path-for-the-heap-" + std::to_string(pid))});
    }
    int reported = 0;
    int unpublished = 0;
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        Domain::PublicationSlot<SearchPublication> slot;
        // A long query that matches pid 1 and 10..19's paths: the sweep fails match insertion and the copy of a
        // nonempty match list into a progress publication too.
        const SearchPublication start = startFor("a-much-longer-path-for-the-heap-1", processes.size(), 1);
        slot.commit(std::make_shared<const SearchPublication>(start));
        std::stop_source stop;
        SearchPublication input = start; // copied before the hook is armed: only run()'s own allocations fail
        const Publish publish = publishTo(slot);
        bool fired = false;
        {
            const TestSupport::FailAllocationAfter guard(failAt);
            run(processes, {.openFiles = &files, .modules = nullptr}, false, std::move(input), publish, stop.get_token());
            fired = !TestSupport::allocationFailurePending();
        }
        const std::shared_ptr<const SearchPublication> last = slot.load();
        ASSERT_NE(last, nullptr);
        if (!fired)
        {
            EXPECT_TRUE(last->done);
            EXPECT_TRUE(last->failure.empty()) << "nothing failed, nothing to say";
            EXPECT_EQ(last->matches.size(), 11U) << "the query matches: the sweep went through match insertion";
            break;
        }
        if (last->done)
        {
            reported += last->failure.empty() ? 0 : 1;
        }
        else
        {
            ++unpublished; // the done publication's own allocation failed
        }
    }
    EXPECT_GT(reported, 0) << "a failure in the walk is published as done with its reason";
    EXPECT_GT(unpublished, 0) << "a failed done publication leaves the last one (the dialog ends the search when its job does)";
}

// #3335: an administrator search after a failed one starts without that failure.
TEST(HandleSearchTest, AnAdministratorSearchDoesNotCarryTheFirstSearchsFailure)
{
    SearchPublication failed = startFor("x", 2, 4);
    failed.done = true;
    failed.failure = "The search stopped: injected reader failure";
    failed.denied.push_back(process(7, "other"));
    const SearchPublication again = startAuthorizedAfter(failed, 5);
    EXPECT_TRUE(again.failure.empty()) << again.failure;
    EXPECT_FALSE(again.done);
}

TEST(HandleSearchTest, AnAdministratorSearchAddsTheDeniedProcessesToTheFirstResults)
{
    FakeOpenFiles files;
    files.results[1] = openFiles({file(Platform::OpenFileKind::File, "/srv/data.db")});
    files.authorizedFiles[2] = openFiles({file(Platform::OpenFileKind::File, "/srv/data.db-wal")});
    std::vector<SearchProcess> processes{process(1, "mine"), process(2, "postgres")};
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;
    run(processes,
        {.openFiles = &files, .modules = nullptr},
        false,
        startFor("data.db", processes.size(), 1),
        publishTo(slot),
        stop.get_token());
    const std::shared_ptr<const SearchPublication> first = slot.load();
    ASSERT_EQ(first->matches.size(), 1U);
    ASSERT_EQ(first->denied.size(), 1U);

    const std::vector<SearchProcess> denied = first->denied;
    run(denied,
        {.openFiles = &files, .modules = nullptr},
        true,
        startAuthorizedAfter(*first, first->version + 1),
        publishTo(slot),
        stop.get_token());
    const std::shared_ptr<const SearchPublication> second = slot.load();
    EXPECT_TRUE(second->done);
    EXPECT_TRUE(second->authorized);
    EXPECT_EQ(second->searched, 2U);
    EXPECT_TRUE(second->denied.empty());
    ASSERT_EQ(second->matches.size(), 2U);
    EXPECT_EQ(second->matches[1].process, "postgres");
    EXPECT_EQ(files.authorizedReads, 1);
}

} // namespace
