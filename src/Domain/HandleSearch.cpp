#include "Domain/HandleSearch.h"

#include "Domain/Utf8Sanitize.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/Utf8CaseFold.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Domain::HandleSearch
{

namespace
{

/// Adds @p path to @p into when it matches, stopping at MAX_MATCHES.
void addMatch(
    const SearchProcess& process, HandleKind kind, const std::string& path, bool deleted, std::string_view query, SearchPublication& into)
{
    if (!pathMatches(path, query))
    {
        return;
    }
    if (into.matches.size() >= MAX_MATCHES)
    {
        into.truncated = true;
        return;
    }
    // Shown and copied only (an action goes by target), so well-formed UTF-8 (#4301): a raw Linux path or comm
    // can hold invalid bytes, which ImGui would draw by swallowing the bytes after them.
    into.matches.append(HandleMatch{
        .target = process.target,
        .process = sanitizedUtf8(process.name),
        .kind = kind,
        .path = sanitizedUtf8(path),
        .deleted = deleted,
    });
}

} // namespace

const HandleMatch& MatchList::Iterator::operator*() const noexcept
{
    return m_Chunk < m_List->m_Chunks.size() ? (*m_List->m_Chunks[m_Chunk])[m_Offset] : m_List->m_Tail[m_Offset];
}

MatchList::Iterator& MatchList::Iterator::operator++() noexcept
{
    // Chunks are never empty, so the step past a chunk's last match lands on the next one's first, or on the tail.
    ++m_Offset;
    if (m_Chunk < m_List->m_Chunks.size() && m_Offset == m_List->m_Chunks[m_Chunk]->size())
    {
        ++m_Chunk;
        m_Offset = 0;
    }
    return *this;
}

MatchList::MatchList(std::vector<HandleMatch> matches) : m_Tail(std::move(matches))
{
    seal();
}

const HandleMatch& MatchList::operator[](std::size_t index) const noexcept
{
    const std::size_t sealed = sealedSize();
    if (index >= sealed)
    {
        return m_Tail[index - sealed];
    }
    // The first chunk that ends past index holds it.
    const auto chunk = static_cast<std::size_t>(std::ranges::upper_bound(m_Ends, index) - m_Ends.begin());
    const std::size_t first = chunk == 0 ? 0 : m_Ends[chunk - 1];
    return (*m_Chunks[chunk])[index - first];
}

MatchList::Iterator MatchList::begin() const noexcept
{
    return {this, 0, 0};
}

MatchList::Iterator MatchList::end() const noexcept
{
    return {this, m_Chunks.size(), m_Tail.size()};
}

void MatchList::append(HandleMatch match)
{
    m_Tail.push_back(std::move(match));
}

void MatchList::seal()
{
    if (m_Tail.empty())
    {
        return; // no empty chunks: the iterator steps over a chunk only at its end
    }
    // Room first, then the chunk, whose allocation comes before the tail is moved into it: a throw leaves the tail.
    m_Chunks.reserve(m_Chunks.size() + 1);
    m_Ends.reserve(m_Ends.size() + 1);
    const std::size_t end = size();
    auto chunk = std::make_shared<const std::vector<HandleMatch>>(std::move(m_Tail));
    m_Tail.clear(); // moved from: empty, and this list's own again
    m_Chunks.push_back(std::move(chunk));
    m_Ends.push_back(end);
}

bool MatchList::extends(const MatchList& earlier) const noexcept
{
    return earlier.m_Tail.empty() && earlier.m_Chunks.size() <= m_Chunks.size() &&
           std::ranges::equal(earlier.m_Chunks, std::span(m_Chunks).first(earlier.m_Chunks.size()));
}

bool pathMatches(std::string_view path, std::string_view query)
{
    // Unicode simple case folding (#4303): "ölfilter" finds C:\Users\Ana\Ölfilter.xlsx, as NTFS treats them as one
    // file, and /home/Ölaf on Linux. Byte lengths can differ (U+212A is three bytes, its fold one), so no size test.
    return !query.empty() && Platform::containsFoldedCase(path, query);
}

HandleKind kindOf(Platform::OpenFileKind kind) noexcept
{
    switch (kind)
    {
    case Platform::OpenFileKind::File:
    case Platform::OpenFileKind::Memfd:
        return HandleKind::File;
    case Platform::OpenFileKind::Directory:
        return HandleKind::Directory;
    case Platform::OpenFileKind::Device:
        return HandleKind::Device;
    case Platform::OpenFileKind::Socket:
        return HandleKind::Socket;
    case Platform::OpenFileKind::Pipe:
        return HandleKind::Pipe;
    case Platform::OpenFileKind::AnonInode:
    case Platform::OpenFileKind::Other:
        return HandleKind::Other;
    }
    return HandleKind::Other;
}

std::string_view kindLabel(HandleKind kind) noexcept
{
    switch (kind)
    {
    case HandleKind::File:
        return "File";
    case HandleKind::Directory:
        return "Folder";
    case HandleKind::Library:
        return "Library";
    case HandleKind::Socket:
        return "Socket";
    case HandleKind::Pipe:
        return "Pipe";
    case HandleKind::Device:
        return "Device";
    case HandleKind::Other:
        return "Other";
    }
    return "Other";
}

void addProcess(const SearchProcess& process,
                const Platform::OpenFilesReadResult& files,
                const Platform::ModulesReadResult& modules,
                std::string_view query,
                SearchPublication& into,
                SearchSources asked)
{
    ++into.searched;
    const bool filesOk = files.status == Platform::OpenFilesReadStatus::Ok;
    const bool modulesOk = modules.status == Platform::ModulesReadStatus::Ok;
    // A source not asked for was read by the first search (#3875): an administrator's pass asks only for what was
    // refused. Then the process was read, however this pass's read went: never exited or failed, whose matches the
    // first search still lists.
    const bool readBefore = !asked.openFiles || !asked.modules;
    if (filesOk)
    {
        for (const Platform::OpenFile& file : files.files)
        {
            addMatch(process, kindOf(file.kind), file.path, file.deleted, query, into);
        }
        into.truncated = into.truncated || files.truncated;
        if (files.namesIncomplete)
        {
            ++into.partlyNamed;
        }
    }
    if (modulesOk)
    {
        for (const Platform::ProcessModule& module : modules.modules)
        {
            addMatch(process, HandleKind::Library, module.path, module.deleted, query, into);
        }
        into.truncated = into.truncated || modules.truncated;
    }
    // Each source refused on its own (#3357): Windows' readers need different rights, so the open files can be read
    // while the modules are refused, or the other way round, and an administrator's search reads only what was refused.
    const SearchSources refused{
        .openFiles = files.status == Platform::OpenFilesReadStatus::PermissionDenied,
        .modules = modules.status == Platform::ModulesReadStatus::PermissionDenied,
    };
    const bool anyRefused = refused.openFiles || refused.modules;
    if (filesOk || modulesOk || readBefore)
    {
        if (anyRefused)
        {
            into.denied.push_back(DeniedProcess{.process = process, .refused = refused});
        }
        return;
    }
    if (files.status == Platform::OpenFilesReadStatus::ProcessExited || modules.status == Platform::ModulesReadStatus::ProcessExited)
    {
        ++into.exited;
    }
    else if (anyRefused)
    {
        into.denied.push_back(DeniedProcess{.process = process, .refused = refused});
    }
    else if (files.status != Platform::OpenFilesReadStatus::Unsupported || modules.status != Platform::ModulesReadStatus::Unsupported)
    {
        ++into.failed;
    }
}

SearchPublication startFor(std::string_view query, std::size_t total, std::uint64_t version)
{
    SearchPublication start;
    start.version = version;
    start.query = std::string(query);
    start.total = total;
    return start;
}

std::vector<SearchProcess> processesOf(std::span<const DeniedProcess> denied)
{
    std::vector<SearchProcess> processes;
    processes.reserve(denied.size());
    for (const DeniedProcess& entry : denied)
    {
        processes.push_back(entry.process);
    }
    return processes;
}

std::vector<SearchSources> sourcesOf(std::span<const DeniedProcess> denied)
{
    std::vector<SearchSources> sources;
    sources.reserve(denied.size());
    for (const DeniedProcess& entry : denied)
    {
        sources.push_back(entry.refused);
    }
    return sources;
}

SearchPublication startAuthorizedAfter(const SearchPublication& previous, std::uint64_t version)
{
    SearchPublication start = previous;
    start.version = version;
    start.searched -= std::min(start.searched, previous.denied.size());
    start.denied.clear();
    start.done = false;
    start.failure.clear(); // a failure of the first search isn't this one's (#3335)
    start.authorized = true;
    return start;
}

namespace
{

/// Gets an administrator's pass ready (#4420): starts the helper once, after one password prompt, for every read the
/// walk makes, rather than letting each readAuthorized() ask again after the prompt is dismissed. Each reader the walk
/// reads with is asked; they share one helper, so once the first has started it the second has nothing to start. The
/// error is the first one's that couldn't.
[[nodiscard]] std::expected<void, std::string> authorizeWalk(std::span<const SearchProcess> processes,
                                                             std::span<const SearchSources> sources,
                                                             const Readers& readers,
                                                             const std::stop_token& stopToken)
{
    // A process past the end of `sources` reads both.
    const bool all = processes.size() > sources.size();
    const std::span<const SearchSources> asked = sources.first(std::min(sources.size(), processes.size()));
    if (readers.openFiles != nullptr && (all || std::ranges::any_of(asked, &SearchSources::openFiles)))
    {
        if (std::expected<void, std::string> ready = readers.openFiles->authorize(stopToken); !ready.has_value())
        {
            return ready;
        }
    }
    if (readers.modules != nullptr && (all || std::ranges::any_of(asked, &SearchSources::modules)))
    {
        return readers.modules->authorize(stopToken);
    }
    return {};
}

/// The processes an administrator's pass won't read, since its helper didn't start (#4420): each stays denied for what
/// it was to read, so the status line counts them and offers the search again.
void keepDenied(std::span<const SearchProcess> processes,
                std::span<const SearchSources> sources,
                const Readers& readers,
                SearchPublication& current)
{
    for (std::size_t i = 0; i < processes.size(); ++i)
    {
        const SearchSources asked = i < sources.size() ? sources[i] : SearchSources{};
        const SearchSources refused{
            .openFiles = asked.openFiles && readers.openFiles != nullptr,
            .modules = asked.modules && readers.modules != nullptr,
        };
        ++current.searched;
        if (refused.openFiles || refused.modules)
        {
            current.denied.push_back(DeniedProcess{.process = processes[i], .refused = refused});
        }
    }
}

/// The walk itself: run()'s, which catches what this throws.
void walk(std::span<const SearchProcess> processes,
          std::span<const SearchSources> sources,
          const Readers& readers,
          bool authorized,
          SearchPublication& current,
          const Publish& publish,
          const std::stop_token& stopToken)
{
    const std::string query = current.query; // folded as it is matched, by pathMatches()
    if (stopToken.stop_requested())
    {
        return; // stopped before it began: no batch, so no table walked for a search that reads nothing
    }
    // The sources of the processes from processes[from] on (a process past the end of `sources` reads both).
    const auto sourcesFrom = [sources](std::size_t from)
    {
        return sources.subspan(std::min(from, sources.size()));
    };
    // Whether an administrator's pass may read on from processes[from] (#4420): its helper is running, or was started
    // now. When it isn't (the prompt was dismissed, Stop was pressed while it was up, or the helper couldn't start),
    // the pass ends there with one line saying why, its processes from there on still refused, rather than prompting
    // again for each one.
    const auto readyFrom = [&](std::size_t from, std::size_t count)
    {
        const std::span<const SearchProcess> rest = processes.subspan(from);
        std::expected<void, std::string> ready =
            authorizeWalk(rest.first(std::min(count, rest.size())), sourcesFrom(from), readers, stopToken);
        if (ready.has_value())
        {
            return true;
        }
        keepDenied(rest, sourcesFrom(from), readers, current);
        if (!stopToken.stop_requested())
        {
            current.failure = from == 0 ? std::format("The search as administrator didn't start: {}", ready.error())
                                        : std::format("The search as administrator stopped: {}", ready.error());
        }
        return false;
    };
    // One password prompt for the walk, before any table is walked.
    if (authorized && !readyFrom(0, processes.size()))
    {
        return;
    }

    // One batch for the walk (#3357): on Windows, one pass over the system handle table for every process, not one
    // pass each. Ended however the walk ends, a throw included.
    struct BatchScope
    {
        explicit BatchScope(Platform::IProcessOpenFilesReader* reader) : m_Reader(reader)
        {
            if (m_Reader != nullptr)
            {
                m_Reader->beginBatch();
            }
        }
        ~BatchScope()
        {
            if (m_Reader != nullptr)
            {
                m_Reader->endBatch();
            }
        }
        BatchScope(const BatchScope&) = delete;
        BatchScope& operator=(const BatchScope&) = delete;
        BatchScope(BatchScope&&) = delete;
        BatchScope& operator=(BatchScope&&) = delete;

      private:
        Platform::IProcessOpenFilesReader* m_Reader;
    };
    // Only when some process is to have its open files read: a retry of refused modules alone walks no table.
    // A process past the end of `sources` reads both.
    const bool readsOpenFiles =
        processes.size() > sources.size() || std::ranges::any_of(sources.first(processes.size()), &SearchSources::openFiles);
    const BatchScope batch(readsOpenFiles ? readers.openFiles : nullptr);
    if (authorized && readsOpenFiles && readers.openFiles != nullptr)
    {
        // An administrator's pass names the processes whose open files it reads (#3578), so the helper can read them
        // all from one walk of its handle table: just the ones whose open files were refused.
        std::vector<Platform::ProcessTarget> targets;
        targets.reserve(processes.size());
        for (std::size_t i = 0; i < processes.size(); ++i)
        {
            if (i >= sources.size() || sources[i].openFiles)
            {
                targets.push_back(processes[i].target);
            }
        }
        readers.openFiles->expectAuthorizedReads(targets);
    }

    std::size_t sincePublish = 0;
    std::size_t index = 0; // the process's place in processes, and its sources' in sources
    for (const SearchProcess& process : processes)
    {
        if (stopToken.stop_requested())
        {
            break;
        }
        if (current.matches.size() >= MAX_MATCHES)
        {
            // Processes are left unsearched (#3299): the result is the first matches, not all of them, even when
            // the last one read added exactly the last match the cap allows.
            current.truncated = true;
            break;
        }
        // Each reader confirms the process's identity (start time) itself, so a reused PID reads as exited. Only the
        // sources asked for are read: an administrator's search reads just the ones the first search was refused.
        const SearchSources asked = index < sources.size() ? sources[index] : SearchSources{};
        // The helper doesn't stay for good (its idle exit, or it ended): one that has gone is started again once, and
        // a prompt dismissed then ends the pass here, as at its start, not with a prompt for every process left.
        if (authorized && index > 0 && !readyFrom(index, 1))
        {
            break;
        }
        ++index;
        Platform::OpenFilesReadResult files;
        Platform::ModulesReadResult modules;
        if (readers.openFiles != nullptr && asked.openFiles)
        {
            files = authorized ? readers.openFiles->readAuthorized(process.target, stopToken)
                               : readers.openFiles->readOpenFiles(process.target);
        }
        if (readers.modules != nullptr && asked.modules)
        {
            modules =
                authorized ? readers.modules->readAuthorized(process.target, stopToken) : readers.modules->readModules(process.target);
        }
        addProcess(process, files, modules, query, current, asked);
        if (++sincePublish == PUBLISH_EVERY_PROCESSES)
        {
            sincePublish = 0;
            ++current.version;
            // The matches since the last generation become a chunk this one shares (#3888): the copy publish()
            // takes copies no match, and the walk's later matches go to its own tail, not to what was published.
            current.matches.seal();
            publish(current);
        }
    }
}

/// Publishes @p current as done, saying why it stopped early when @p reason isn't null. A publication that can't be
/// made (no memory left even for it) is given up on: the dialog sees its worker return and stops waiting (#3335).
void finish(SearchPublication& current, const Publish& publish, const char* reason) noexcept
{
    try
    {
        current.done = true;
        if (reason != nullptr)
        {
            current.failure = std::format("The search stopped: {}", reason);
        }
        ++current.version;
        current.matches.seal(); // published lists have no tail, so an administrator's search after it extends it
        publish(std::move(current));
    }
    catch (...) // NOLINT(bugprone-empty-catch) - nothing is left to report with; see above
    {}
}

} // namespace

void run(std::span<const SearchProcess> processes,
         const Readers& readers,
         bool authorized,
         SearchPublication start,
         const Publish& publish,
         const std::stop_token& stopToken,
         std::span<const SearchSources> sources) noexcept
{
    SearchPublication current = std::move(start);
    try
    {
        walk(processes, sources, readers, authorized, current, publish, stopToken);
    }
    catch (const std::exception& error)
    {
        finish(current, publish, error.what());
        return;
    }
    catch (...)
    {
        finish(current, publish, "an unknown error");
        return;
    }
    finish(current, publish, nullptr);
}

} // namespace Domain::HandleSearch
