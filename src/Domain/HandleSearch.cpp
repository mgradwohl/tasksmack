#include "Domain/HandleSearch.h"

#include "Domain/PublicationSlot.h"
#include "Domain/PublicationWake.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

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
    }
    if (modulesOk)
    {
        for (const Platform::ProcessModule& module : modules.modules)
        {
            addMatch(process, HandleKind::Library, module.path, module.deleted, loweredQuery, into);
        }
        into.truncated = into.truncated || modules.truncated;
    }
    if (filesOk || modulesOk)
    {
        return;
    }
    if (files.status == Platform::OpenFilesReadStatus::ProcessExited || modules.status == Platform::ModulesReadStatus::ProcessExited)
    {
        ++into.exited;
    }
    else if (files.status == Platform::OpenFilesReadStatus::PermissionDenied ||
             modules.status == Platform::ModulesReadStatus::PermissionDenied)
    {
        into.denied.push_back(process);
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

SearchPublication startAuthorizedAfter(const SearchPublication& previous, std::uint64_t version)
{
    SearchPublication start = previous;
    start.version = version;
    start.searched -= std::min(start.searched, previous.denied.size());
    start.denied.clear();
    start.done = false;
    start.authorized = true;
    return start;
}

namespace
{

/// The walk itself: run()'s, which catches what this throws.
void walk(std::span<const SearchProcess> processes,
          const Readers& readers,
          bool authorized,
          SearchPublication& current,
          PublicationSlot<SearchPublication>& slot,
          const std::stop_token& stopToken)
{
    const std::string loweredQuery = lowered(current.query);

    std::size_t sincePublish = 0;
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
        // Each reader confirms the process's identity (start time) itself, so a reused PID reads as exited.
        Platform::OpenFilesReadResult files;
        Platform::ModulesReadResult modules;
        if (readers.openFiles != nullptr)
        {
            files = authorized ? readers.openFiles->readAuthorized(process.target, stopToken)
                               : readers.openFiles->readOpenFiles(process.target);
        }
        if (readers.modules != nullptr)
        {
            modules =
                authorized ? readers.modules->readAuthorized(process.target, stopToken) : readers.modules->readModules(process.target);
        }
        addProcess(process, files, modules, loweredQuery, current);
        if (++sincePublish == PUBLISH_EVERY_PROCESSES)
        {
            sincePublish = 0;
            ++current.version;
            // No wake: the dialog's progress line can wait for the frame loop's slow floor (#3156).
            slot.commit(std::make_shared<const SearchPublication>(current));
        }
    }
}

/// Publishes @p current as done, saying why it stopped early when @p reason isn't null. A publication that can't be
/// made (no memory left even for it) is given up on: the dialog sees its worker return and stops waiting (#3335).
void finish(SearchPublication& current, PublicationSlot<SearchPublication>& slot, const char* reason) noexcept
{
    try
    {
        current.done = true;
        if (reason != nullptr)
        {
            current.failure = std::format("The search stopped: {}", reason);
        }
        ++current.version;
        slot.commit(std::make_shared<const SearchPublication>(std::move(current)));
    }
    catch (...) // NOLINT(bugprone-empty-catch) - nothing is left to report with; see above
    {}
    PublicationWake::notify(); // the finished search shows at once (#3156)
}

} // namespace

void run(std::span<const SearchProcess> processes,
         const Readers& readers,
         bool authorized,
         SearchPublication start,
         PublicationSlot<SearchPublication>& slot,
         const std::stop_token& stopToken) noexcept
{
    SearchPublication current = std::move(start);
    try
    {
        walk(processes, readers, authorized, current, slot, stopToken);
    }
    catch (const std::exception& error)
    {
        finish(current, slot, error.what());
        return;
    }
    catch (...)
    {
        finish(current, slot, "an unknown error");
        return;
    }
    finish(current, slot, nullptr);
}

} // namespace Domain::HandleSearch
