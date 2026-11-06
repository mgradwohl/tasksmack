#include "LinuxProcessMemoryMapReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "PosixGuards.h"
#include "ProcParsing.h"
#include "SmapsParser.h"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

[[nodiscard]] MemoryMapReadResult failure(int err)
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return {.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}};
    case ENOENT:
    case ESRCH:
        return {.status = MemoryMapReadStatus::ProcessExited, .mappings = {}, .detail = {}};
    default:
        return {.status = MemoryMapReadStatus::Failed, .mappings = {}, .detail = std::system_category().message(err)};
    }
}

} // namespace

LinuxProcessMemoryMapReader::LinuxProcessMemoryMapReader(std::string procRoot, std::size_t maxSmapsBytes)
    : m_ProcRoot(std::move(procRoot)), m_MaxSmapsBytes(maxSmapsBytes)
{
    std::error_code ignored;
    m_HasSmaps = std::filesystem::exists(std::filesystem::path(m_ProcRoot) / "self" / "smaps", ignored);
}

bool LinuxProcessMemoryMapReader::hasMemoryMap() const
{
    return m_HasSmaps;
}

MemoryMapReadResult LinuxProcessMemoryMapReader::readMemoryMap(const ProcessTarget& target)
{
    if (!m_HasSmaps)
    {
        // A kernel without CONFIG_PROC_PAGE_MONITOR has no smaps for any process: unsupported, never "exited".
        return {.status = MemoryMapReadStatus::Unsupported, .mappings = {}, .detail = {}};
    }
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone. The caller retries at its next
        // refresh, once the process's start time is known.
        return {.status = MemoryMapReadStatus::IdentityUnknown, .mappings = {}, .detail = {}};
    }

    // One handle on the process's /proc directory: once open it keeps naming this process, so smaps read
    // through it after the process exits fails or reads empty instead of reaching the PID's next owner.
    const std::string dirPath = m_ProcRoot + "/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(errno);
    }
    if (ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // gone, or the PID now names another process
    }
    // One byte past the cap, so an smaps exactly the cap long reads whole and isn't called cut short.
    const std::size_t readLimit = m_MaxSmapsBytes == std::numeric_limits<std::size_t>::max() ? m_MaxSmapsBytes : m_MaxSmapsBytes + 1;
    const std::expected<std::string, int> smaps = ProcParsing::readWholeFileAt(dir.get(), "smaps", readLimit);
    if (!smaps.has_value())
    {
        return failure(smaps.error());
    }
    // An exiting process's smaps reads empty (as a kernel thread's always does): confirmed before that is
    // reported as an empty map.
    if (smaps->empty() && ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH);
    }
    std::string_view text = *smaps;
    const bool truncated = text.size() > m_MaxSmapsBytes;
    if (truncated)
    {
        const std::size_t lastNewline = text.substr(0, m_MaxSmapsBytes).rfind('\n');
        text = text.substr(0, lastNewline == std::string_view::npos ? 0 : lastNewline + 1);
    }
    std::vector<MemoryMapping> mappings = Smaps::parse(text);
    if (truncated && !mappings.empty())
    {
        mappings.pop_back(); // the cut can fall among its size lines, which would then read as unmeasured
    }
    return {.status = MemoryMapReadStatus::Ok, .mappings = std::move(mappings), .detail = {}, .truncated = truncated};
}

} // namespace Platform
