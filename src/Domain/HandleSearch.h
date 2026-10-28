#pragma once

// Find handles (#3067): which processes hold a file, folder, library or socket whose name contains a
// query ("what's locking this file?"). One search walks every process the latest snapshot lists through
// the open-files (IProcessOpenFiles.h) and modules (IProcessModules.h) readers Process Details uses, on a
// worker, and publishes its matches in batches as it goes. Searching is pure here; the worker that runs
// it belongs to the Find handles dialog.

#include "Domain/PublicationSlot.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
    /// Processes the OS wouldn't let this run read (another user's): an administrator can search them.
    std::vector<SearchProcess> denied;
    std::size_t exited = 0;  ///< Processes that ended before they were read.
    std::size_t failed = 0;  ///< Processes whose read failed some other way.
    bool truncated = false;  ///< It stopped at MAX_MATCHES, or a process had more than a read lists.
    bool done = false;       ///< The walk finished (or was stopped).
    bool authorized = false; ///< This search read as an administrator.
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
/// A process both readers refuse counts as denied; one either reports exited counts as exited.
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

/// Searches @p processes, adding to @p start, and commits a new generation to @p slot every
/// PUBLISH_EVERY_PROCESSES processes and once at the end (done). The caller commits @p start itself, if
/// readers should see the search begin, before handing it to a worker. @p start carries the version to begin
/// at, the query, the total and anything found before: a fresh search's (startFor()) or the matches so far
/// when an administrator searches the processes the first search was denied (startAuthorizedAfter()).
/// @p authorized reads each process as an administrator (readAuthorized(): one password prompt for the
/// walk). Stops between processes when @p stopToken is signalled, publishing what it has as done.
void run(std::span<const SearchProcess> processes,
         const Readers& readers,
         bool authorized,
         SearchPublication start,
         PublicationSlot<SearchPublication>& slot,
         const std::stop_token& stopToken);

/// The first generation of a new search for @p query over @p total processes, at @p version.
[[nodiscard]] SearchPublication startFor(std::string_view query, std::size_t total, std::uint64_t version);

/// The first generation of an administrator's search of @p previous's denied processes: its matches and
/// counts, with the denied ones not yet searched, at @p version.
[[nodiscard]] SearchPublication startAuthorizedAfter(const SearchPublication& previous, std::uint64_t version);

} // namespace Domain::HandleSearch
