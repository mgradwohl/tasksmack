#include "LinuxProcessIconSource.h"

#include "Platform/IProcessIconSource.h"
#include "Platform/IconImageFile.h"
#include "Platform/Linux/DesktopEntryIcons.h"
#include "Platform/Linux/PosixGuards.h"
#include "Platform/Linux/ProcParsing.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// The value of environment variable @p name, or empty.
[[nodiscard]] std::string_view iconSearchEnvironment(const char* name)
{
    // Read once, while the source is made on the UI thread at attach, before any worker runs.
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

} // namespace

LinuxProcessIconSource::LinuxProcessIconSource()
    : LinuxProcessIconSource(DesktopEntryIcons::searchDirs(iconSearchEnvironment("HOME"),
                                                           iconSearchEnvironment("XDG_DATA_HOME"),
                                                           iconSearchEnvironment("XDG_DATA_DIRS")),
                             "/proc")
{}

LinuxProcessIconSource::LinuxProcessIconSource(DesktopEntryIcons::SearchDirs dirs, std::filesystem::path procRoot)
    : m_Dirs(std::move(dirs)), m_ProcRoot(std::move(procRoot))
{}

std::string LinuxProcessIconSource::executablePath(std::int32_t pid, std::uint64_t startTimeTicks)
{
    if (pid <= 0)
    {
        return {};
    }
    // One handle on the process's /proc directory: once open it keeps naming this process, so the exe
    // link read through it after the process exits fails instead of reaching the PID's next owner.
    const std::string dirPath = (m_ProcRoot / std::to_string(pid)).string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const Posix::FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return {};
    }
    // The row's start time, when known (#4192, the Linux half of #4183): a reused PID is another
    // process, whose program isn't the row's, so it gives no path. Unknown (0) reads by PID, as before.
    if (startTimeTicks != 0)
    {
        const std::expected<std::string, int> stat = ProcParsing::readWholeFileAt(dir.get(), "stat");
        if (!stat.has_value() || ProcParsing::parseStatStartTime(*stat) != startTimeTicks)
        {
            return {};
        }
    }
    // Unreadable for another user's process without CAP_SYS_PTRACE, and for kernel threads: the
    // icon is then looked up by the process's name alone.
    std::array<char, 4096> buffer{}; // Linux's PATH_MAX
    const auto length = ::readlinkat(dir.get(), "exe", buffer.data(), buffer.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size())
    {
        return {};
    }
    std::string path(buffer.data(), static_cast<std::size_t>(length));
    // An executable replaced on disk since it started (an upgrade) reads as "... (deleted)".
    constexpr std::string_view DELETED_SUFFIX = " (deleted)";
    if (path.ends_with(DELETED_SUFFIX))
    {
        path.resize(path.size() - DELETED_SUFFIX.size());
    }
    return path;
}

const DesktopEntryIcons::DesktopIndex& LinuxProcessIconSource::index()
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_Index.has_value() || (now - m_IndexReadAt) >= INDEX_LIFETIME)
    {
        DesktopEntryIcons::DesktopIndex fresh;
        fresh.addDirectories(m_Dirs.applications);
        m_Index = std::move(fresh);
        m_IndexReadAt = now;
    }
    return *m_Index;
}

std::optional<IconPixels> LinuxProcessIconSource::loadIcon(std::string_view executablePath, std::string_view processName, int sizePx)
{
    if (sizePx <= 0)
    {
        return std::nullopt;
    }
    const std::optional<std::string> icon = index().iconFor(executablePath, processName);
    if (!icon.has_value())
    {
        return std::nullopt;
    }
    const std::optional<std::filesystem::path> file = DesktopEntryIcons::findIconFile(*icon, sizePx, m_Dirs.iconBases, m_Dirs.pixmaps);
    if (!file.has_value())
    {
        return std::nullopt;
    }
    return decodePng(*file);
}

std::optional<IconPixels> LinuxProcessIconSource::decodePng(const std::filesystem::path& path)
{
    return decodeIconImageFile(path.string());
}

} // namespace Platform
