#pragma once

// Find handles (#3067): which processes hold a file, folder, library or socket whose name contains a
// query ("what's locking this file?"). One search walks every process the latest snapshot lists through
// the open-files (IProcessOpenFiles.h) and modules (IProcessModules.h) readers Process Details uses, the
// open-files reader in one batch (beginBatch(), #3357: one pass over Windows' handle table), on a
// worker, and publishes its matches in batches as it goes. An administrator's pass names the processes
// it reads in that batch (expectAuthorizedReads(), #3578), so the helper reads them in one request. Searching is pure here; the worker that
// runs it (a Domain::Worker, #3343) belongs to the Find handles dialog.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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

/// A search's matches in the order found (#3888): sealed chunks, each shared and immutable once sealed, then a tail
/// still being added to. A copy shares the sealed chunks rather than copying their matches, so a generation never
/// copies what an earlier one published, and a copy keeps exactly the matches it had: what the original adds later
/// goes to the original's own tail. The walk seals before each publication, so a published list has no tail and is
/// read, never written, from then on.
class MatchList
{
  public:
    /// Walks the matches in order: each sealed chunk, then the tail.
    class Iterator
    {
      public:
        // The standard library's iterator names, which range algorithms look for.
        using value_type = HandleMatch;                      // NOLINT(readability-identifier-naming)
        using difference_type = std::ptrdiff_t;              // NOLINT(readability-identifier-naming)
        using iterator_category = std::forward_iterator_tag; // NOLINT(readability-identifier-naming)
        using pointer = const HandleMatch*;                  // NOLINT(readability-identifier-naming)
        using reference = const HandleMatch&;                // NOLINT(readability-identifier-naming)

        Iterator() = default;

        [[nodiscard]] const HandleMatch& operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept
        {
            Iterator before = *this;
            ++*this;
            return before;
        }
        [[nodiscard]] friend bool operator==(const Iterator& a, const Iterator& b) noexcept
        {
            return a.m_List == b.m_List && a.m_Chunk == b.m_Chunk && a.m_Offset == b.m_Offset;
        }

      private:
        friend class MatchList;
        Iterator(const MatchList* list, std::size_t chunk, std::size_t offset) noexcept : m_List(list), m_Chunk(chunk), m_Offset(offset)
        {}

        const MatchList* m_List = nullptr;
        std::size_t m_Chunk = 0;  ///< The sealed chunk it is in; m_Chunks.size() for the tail.
        std::size_t m_Offset = 0; ///< Its place in that chunk.
    };

    MatchList() = default;
    /// @p matches as one sealed chunk (a list made in one go, as a test's).
    explicit MatchList(std::vector<HandleMatch> matches);

    [[nodiscard]] std::size_t size() const noexcept
    {
        return sealedSize() + m_Tail.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }
    /// The @p index th match found. @p index must be below size().
    [[nodiscard]] const HandleMatch& operator[](std::size_t index) const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

    /// Adds @p match to the tail.
    void append(HandleMatch match);
    /// Makes the tail a sealed chunk, shared from then on by every copy. Strong guarantee: when it throws (no
    /// memory), the list is as it was.
    void seal();
    /// How many sealed chunks it has.
    [[nodiscard]] std::size_t chunkCount() const noexcept
    {
        return m_Chunks.size();
    }
    /// Whether this list is @p earlier with matches added after: @p earlier has no tail, and its chunks are this
    /// one's first ones, the same chunks, not copies. Then @p earlier's matches are this one's first ones.
    [[nodiscard]] bool extends(const MatchList& earlier) const noexcept;

  private:
    [[nodiscard]] std::size_t sealedSize() const noexcept
    {
        return m_Ends.empty() ? 0 : m_Ends.back();
    }

    std::vector<std::shared_ptr<const std::vector<HandleMatch>>> m_Chunks; ///< Sealed; never empty ones
    std::vector<std::size_t> m_Ends;                                       ///< m_Ends[i]: the matches in chunks 0 to i
    std::vector<HandleMatch> m_Tail;                                       ///< Added since the last seal(); this list's own
};

/// One generation of a search's results: all the matches so far and how far the walk has got.
struct SearchPublication
{
    std::uint64_t version = 0; ///< 0 before any search.
    std::string query;         ///< What this search looks for, as typed.
    MatchList matches;         ///< Shares its chunks with the generations before it (#3888)
    std::size_t total = 0;     ///< Processes to search.
    std::size_t searched = 0;  ///< Processes read so far, whether or not they could be.
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

/// Whether @p path contains @p query by Unicode simple case folding (#4303). An empty query matches nothing.
[[nodiscard]] bool pathMatches(std::string_view path, std::string_view query);

/// The kind shown for an open descriptor or handle of @p kind.
[[nodiscard]] HandleKind kindOf(Platform::OpenFileKind kind) noexcept;

/// A short label for @p kind ("File", "Library", ...).
[[nodiscard]] std::string_view kindLabel(HandleKind kind) noexcept;

/// What one process's reads add to @p into: its open files' and modules' matches, then its counts.
/// A source a reader refuses makes the process denied for that source (#3357), even when the other source was read.
/// One neither source read, and either reports exited, counts as exited. A source not read comes as a default
/// (Unsupported) result. @p asked is the sources this pass read: one left out was read by an earlier pass (an
/// administrator's search reads only what was refused), so the process counts as read, never as exited or failed,
/// whatever this pass's read reports (#3875).
void addProcess(const SearchProcess& process,
                const Platform::OpenFilesReadResult& files,
                const Platform::ModulesReadResult& modules,
                std::string_view query,
                SearchPublication& into,
                SearchSources asked = {});

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
/// @p authorized reads each process as an administrator (readAuthorized()), after one password prompt for the walk:
/// each reader's authorize() first (#4420). When that fails (the prompt was dismissed, or the helper couldn't start),
/// the pass reads nothing, its processes stay denied, and failure says why (nothing, when it was stopped). A helper
/// that goes during the walk is started again once, before the next process, and the same holds from there on.
/// Stops between processes when @p stopToken is signalled, publishing what it has as done.
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
