#pragma once

// Find handles (#3067): which processes hold a file, folder, library or socket whose name contains a
// query ("what's locking this file?"). One search walks every process the latest snapshot lists through
// the open-files (IProcessOpenFiles.h) and modules (IProcessModules.h) readers Process Details uses, the
// open-files reader in one batch (beginBatch(), #3357: one pass over Windows' handle table), on a
// worker, and publishes its matches in batches as it goes. Searching is pure here; the worker that runs
// it (a Domain::Worker, #3343) belongs to the Find handles dialog.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Domain::HandleSearch
{

/// The most matches one search lists; it stops there and says so.
inline constexpr std::size_t MAX_MATCHES = 10000;

/// How many processes the walker reads between publications, so the first matches show early.
inline constexpr std::size_t PUBLISH_EVERY_PROCESSES = 16;

/// What a match is.
enum class HandleKind : std::uint8_t
{
    File,
    Directory,
    Library, ///< A loaded module: an executable image or shared library.
    Socket,
    Pipe,
    Device,
    Other,
};

/// One process to search: its identity and its name, from the process snapshot.
struct SearchProcess
{
    Platform::ProcessTarget target;
    std::string name;
};

/// Which of a process's sources a read covers: its open files, its loaded modules, or both.
struct SearchSources
{
    bool openFiles = true;
    bool modules = true;

    friend bool operator==(const SearchSources&, const SearchSources&) = default;
};

/// A process one or both of whose sources the OS wouldn't let this run read (another user's, or on Windows an
/// elevated one), and which those were (#3357): the two readers need different rights, so one can be refused while
/// the other reads. An administrator's search reads only the refused ones, so no match is found twice.
struct DeniedProcess
{
    SearchProcess process;
    SearchSources refused;
};

/// One handle whose name contains the query.
struct HandleMatch
{
    Platform::ProcessTarget target;
    std::string process;
    HandleKind kind = HandleKind::Other;
    std::string path;
    bool deleted = false; ///< The file was deleted after it was opened or mapped (Linux).
};

/// One generation of a search's results: all the matches so far and how far the walk has got.
struct SearchPublication
{
    std::uint64_t version = 0; ///< 0 before any search.
    std::string query;         ///< What this search looks for, as typed.
    std::vector<HandleMatch> matches;
    std::size_t total = 0;    ///< Processes to search.
    std::size_t searched = 0; ///< Processes read so far, whether or not they could be.
    /// Processes with a source the OS wouldn't let this run read (another user's, or on Windows an elevated one), and
    /// which: an administrator can search those (processesOf(), sourcesOf()).
    std::vector<DeniedProcess> denied;
    std::size_t exited = 0; ///< Processes that ended before they were read.
    std::size_t failed = 0; ///< Processes whose read failed some other way.
    /// Processes read, but some of whose handles weren't named: a handle didn't answer in time (Windows), so a match
    /// among them may be missing.
    std::size_t partlyNamed = 0;
    bool truncated = false;  ///< It stopped at MAX_MATCHES, or a process had more than a read lists.
    bool done = false;       ///< The walk finished (or was stopped).
    bool authorized = false; ///< This search read as an administrator.
    std::string failure;     ///< Why the walk stopped early, when it failed (#3335); empty otherwise.
};

/// @p text in lower case (ASCII letters only, so a UTF-8 path keeps its other bytes).
[[nodiscard]] std::string lowered(std::string_view text);

/// Whether @p path contains @p loweredQuery, ignoring ASCII case. An empty query matches nothing.
[[nodiscard]] bool pathMatches(std::string_view path, std::string_view loweredQuery);

/// The kind shown for an open descriptor or handle of @p kind.
[[nodiscard]] HandleKind kindOf(Platform::OpenFileKind kind) noexcept;

/// A short label for @p kind ("File", "Library", ...).
[[nodiscard]] std::string_view kindLabel(HandleKind kind) noexcept;

/// What one process's reads add to @p into: its open files' and modules' matches, then its counts.
/// A source a reader refuses makes the process denied for that source (#3357), even when the other source was read.
/// One neither source read, and either reports exited, counts as exited. A source not read comes as a default
/// (Unsupported) result.
void addProcess(const SearchProcess& process,
                const Platform::OpenFilesReadResult& files,
                const Platform::ModulesReadResult& modules,
                std::string_view loweredQuery,
                SearchPublication& into);

/// The readers one search uses; either may be null (that kind isn't searched).
struct Readers
{
    Platform::IProcessOpenFilesReader* openFiles = nullptr;
    Platform::IProcessModulesReader* modules = nullptr;
};

/// Where run() puts each generation it makes: its progress (done false) and its end (done true). It may throw
/// (no memory to publish): run() catches that as it does a reader's failure.
using Publish = std::function<void(SearchPublication generation)>;

/// Searches @p processes, adding to @p start, and hands @p publish a new generation every
/// PUBLISH_EVERY_PROCESSES processes and once at the end (done). The caller shows @p start itself, if
/// readers should see the search begin, before handing it to a worker. @p start carries the version to begin
/// at, the query, the total and anything found before: a fresh search's (startFor()) or the matches so far
/// when an administrator searches the processes the first search was denied (startAuthorizedAfter()).
/// @p authorized reads each process as an administrator (readAuthorized(): one password prompt for the
/// walk). Stops between processes when @p stopToken is signalled, publishing what it has as done.
/// Throws nothing (#3335): a reader that throws, or an allocation (or a publication) that fails, ends the walk
/// with a done publication that says why (failure), so the worker never takes TaskSmack down. When even that
/// one can't be made, the last generation handed over stands, and the worker returning ends the search.
/// @p sources says which sources to read for each process, sources[i] for processes[i] (an administrator's search
/// reads only the ones refused, sourcesOf()); empty, or past its end, reads both.
void run(std::span<const SearchProcess> processes,
         const Readers& readers,
         bool authorized,
         SearchPublication start,
         const Publish& publish,
         const std::stop_token& stopToken,
         std::span<const SearchSources> sources = {}) noexcept;

/// @p denied's processes, in order: what an administrator's search walks.
[[nodiscard]] std::vector<SearchProcess> processesOf(std::span<const DeniedProcess> denied);

/// @p denied's refused sources, in order: what an administrator's search reads for each (run()'s @p sources).
[[nodiscard]] std::vector<SearchSources> sourcesOf(std::span<const DeniedProcess> denied);

/// The first generation of a new search for @p query over @p total processes, at @p version.
[[nodiscard]] SearchPublication startFor(std::string_view query, std::size_t total, std::uint64_t version);

/// The first generation of an administrator's search of @p previous's denied processes: its matches and
/// counts, with the denied ones not yet searched, at @p version.
[[nodiscard]] SearchPublication startAuthorizedAfter(const SearchPublication& previous, std::uint64_t version);

} // namespace Domain::HandleSearch
