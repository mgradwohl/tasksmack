#include "LinuxProcessIconSource.h"

#include "Platform/IProcessIconSource.h"
#include "Platform/IconImageFile.h"
#include "Platform/Linux/DesktopEntryIcons.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Platform
{

namespace
{

/// The value of environment variable @p name, or empty.
[[nodiscard]] std::string_view environment(const char* name)
{
    // Read once, while the source is made on the UI thread at attach, before any worker runs.
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

} // namespace

LinuxProcessIconSource::LinuxProcessIconSource()
    : LinuxProcessIconSource(DesktopEntryIcons::searchDirs(environment("HOME"), environment("XDG_DATA_HOME"), environment("XDG_DATA_DIRS")),
                             "/proc")
{}

LinuxProcessIconSource::LinuxProcessIconSource(DesktopEntryIcons::SearchDirs dirs, std::filesystem::path procRoot)
    : m_Dirs(std::move(dirs)), m_ProcRoot(std::move(procRoot))
{}

std::string LinuxProcessIconSource::executablePath(std::int32_t pid)
{
    if (pid <= 0)
    {
        return {};
    }
    // Unreadable for another user's process without CAP_SYS_PTRACE, and for kernel threads: the
    // icon is then looked up by the process's name alone.
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::read_symlink(m_ProcRoot / std::to_string(pid) / "exe", ec);
    if (ec)
    {
        return {};
    }
    std::string path = target.string();
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
