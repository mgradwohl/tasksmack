#include "Domain/HandleSearch.h"

#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
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

[[nodiscard]] char lowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Adds @p path to @p into when it matches, stopping at MAX_MATCHES.
void addMatch(const SearchProcess& process,
              HandleKind kind,
              const std::string& path,
              bool deleted,
              std::string_view loweredQuery,
              SearchPublication& into)
{
    if (!pathMatches(path, loweredQuery))
    {
        return;
    }
    if (into.matches.size() >= MAX_MATCHES)
    {
        into.truncated = true;
        return;
    }
    into.matches.push_back(HandleMatch{.target = process.target, .process = process.name, .kind = kind, .path = path, .deleted = deleted});
}

} // namespace

std::string lowered(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

bool pathMatches(std::string_view path, std::string_view loweredQuery)
{
    if (loweredQuery.empty() || path.size() < loweredQuery.size())
    {
        return false;
    }
    return !std::ranges::search(path, loweredQuery, {}, lowerAscii).empty();
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
                std::string_view loweredQuery,
                SearchPublication& into)
{
    ++into.searched;
    const bool filesOk = files.status == Platform::OpenFilesReadStatus::Ok;
    const bool modulesOk = modules.status == Platform::ModulesReadStatus::Ok;
    if (filesOk)
    {
        for (const Platform::OpenFile& file : files.files)
        {
            addMatch(process, kindOf(file.kind), file.path, file.deleted, loweredQuery, into);
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
            addMatch(process, HandleKind::Library, module.path, module.deleted, loweredQuery, into);
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
    if (filesOk || modulesOk)
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

/// The walk itself: run()'s, which catches what this throws.
void walk(std::span<const SearchProcess> processes,
          std::span<const SearchSources> sources,
          const Readers& readers,
          bool authorized,
          SearchPublication& current,
          const Publish& publish,
          const std::stop_token& stopToken)
{
    const std::string loweredQuery = lowered(current.query);

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
    const BatchScope batch(readers.openFiles);

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
        addProcess(process, files, modules, loweredQuery, current);
        if (++sincePublish == PUBLISH_EVERY_PROCESSES)
        {
            sincePublish = 0;
            ++current.version;
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
