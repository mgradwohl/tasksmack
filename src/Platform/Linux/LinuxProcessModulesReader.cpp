#include "LinuxProcessModulesReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "PosixGuards.h"
#include "ProcMapsParser.h"
#include "ProcParsing.h"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

[[nodiscard]] ModulesReadResult failure(int err)
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return {.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}};
    case ENOENT:
    case ESRCH:
        return {.status = ModulesReadStatus::ProcessExited, .modules = {}, .detail = {}};
    default:
        return {.status = ModulesReadStatus::Failed, .modules = {}, .detail = std::system_category().message(err)};
    }
}

} // namespace

bool LinuxProcessModulesReader::hasModules() const
{
    return true;
}

ModulesReadResult LinuxProcessModulesReader::readModules(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as checkProcessIdentity() refuses
        // an action. The caller retries at its next refresh, once the process's start time is known.
        return {.status = ModulesReadStatus::IdentityUnknown, .modules = {}, .detail = {}};
    }

    // One handle on the process's /proc directory: once open it keeps naming this process, so a file
    // read through it after the process exits fails instead of reaching the PID's next owner.
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
    // One byte past the cap, so a maps exactly the cap long reads whole and isn't called cut short (#2749).
    const std::size_t readLimit = m_MaxMapsBytes == std::numeric_limits<std::size_t>::max() ? m_MaxMapsBytes : m_MaxMapsBytes + 1;
    const std::expected<std::string, int> maps = ProcParsing::readWholeFileAt(dir.get(), "maps", readLimit);
    if (!maps.has_value())
    {
        return failure(maps.error());
    }
    // An exiting process's maps reads empty: confirmed before that is reported as "no modules".
    if (maps->empty() && ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH);
    }
    // The read went past the cap (#2642): parse only the whole lines within it.
    std::string_view text = *maps;
    const bool truncated = text.size() > m_MaxMapsBytes;
    if (truncated)
    {
        const std::size_t lastNewline = text.substr(0, m_MaxMapsBytes).rfind('\n');
        text = text.substr(0, lastNewline == std::string_view::npos ? 0 : lastNewline + 1);
    }
    return {.status = ModulesReadStatus::Ok, .modules = ProcMaps::parseProcMaps(text), .detail = {}, .truncated = truncated};
}

} // namespace Platform
