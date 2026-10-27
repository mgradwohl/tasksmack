// Find handles (#3067): matching, counting and the walk over every process, against fake readers.

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
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace Domain::HandleSearch;

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
    run(processes, {.openFiles = &files, .modules = &mods}, false, startFor("FILE-1", processes.size(), 5), slot, stop.get_token());

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
    run(processes, {.openFiles = &files, .modules = nullptr}, false, startFor("x", processes.size(), 1), slot, stop.get_token());
    ASSERT_NE(slot.load(), nullptr);
    EXPECT_TRUE(slot.load()->done);
    EXPECT_EQ(slot.load()->searched, 0U);
    EXPECT_TRUE(files.read.empty());
}

TEST(HandleSearchTest, AnAdministratorSearchAddsTheDeniedProcessesToTheFirstResults)
{
    FakeOpenFiles files;
    files.results[1] = openFiles({file(Platform::OpenFileKind::File, "/srv/data.db")});
    files.authorizedFiles[2] = openFiles({file(Platform::OpenFileKind::File, "/srv/data.db-wal")});
    std::vector<SearchProcess> processes{process(1, "mine"), process(2, "postgres")};
    Domain::PublicationSlot<SearchPublication> slot;
    std::stop_source stop;
    run(processes, {.openFiles = &files, .modules = nullptr}, false, startFor("data.db", processes.size(), 1), slot, stop.get_token());
    const std::shared_ptr<const SearchPublication> first = slot.load();
    ASSERT_EQ(first->matches.size(), 1U);
    ASSERT_EQ(first->denied.size(), 1U);

    const std::vector<SearchProcess> denied = first->denied;
    run(denied, {.openFiles = &files, .modules = nullptr}, true, startAuthorizedAfter(*first, first->version + 1), slot, stop.get_token());
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
