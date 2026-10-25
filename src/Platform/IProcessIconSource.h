#pragma once

// The icon a program shows for itself (#1800): what Task Manager draws beside each process name. Raw
// OS data only -- the executable a process runs and that file's icon as pixels. Caching, scaling and
// textures are Domain's and UI's (Domain/ProcessIconCache.h, UI/IconTextureCache.h).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// An icon as the OS gives it: straight (not premultiplied) alpha RGBA, 4 bytes a pixel, rows top
/// to bottom, width * height * 4 bytes in all.
struct IconPixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

/// Reads processes' executables and their icons. Called from one worker thread at a time, which may
/// differ between calls, never from the UI thread: an icon read touches the disk.
class IProcessIconSource
{
  public:
    virtual ~IProcessIconSource() = default;

    IProcessIconSource() = default;
    IProcessIconSource(const IProcessIconSource&) = default;
    IProcessIconSource& operator=(const IProcessIconSource&) = default;
    IProcessIconSource(IProcessIconSource&&) = default;
    IProcessIconSource& operator=(IProcessIconSource&&) = default;

    /// The full path, UTF-8, of the executable process @p pid runs. Empty when it can't be read: the
    /// process exited, or TaskSmack may not open it (another user's process without the rights).
    [[nodiscard]] virtual std::string executablePath(std::int32_t pid) = 0;

    /// The icon of the program at @p executablePath, as near @p sizePx square as the OS has it.
    /// @p processName is the process's name as listed, for a platform that finds icons by name as
    /// well as by path (Linux's desktop entries); @p executablePath may be empty there. nullopt when
    /// the program has no icon or it can't be read.
    [[nodiscard]] virtual std::optional<IconPixels> loadIcon(std::string_view executablePath, std::string_view processName, int sizePx) = 0;

    /// Whether two paths that differ only in letter case name the same file (Windows), so a cache
    /// keyed by path can fold case.
    [[nodiscard]] virtual bool caseInsensitivePaths() const noexcept = 0;

    /// Whether loadIcon() may answer by @p processName even with a path (Linux: an interpreter's or Wine's
    /// processes share one executable, and their desktop entries match their names), so a cache must key
    /// by both (#2462). False: the icon depends on the path alone.
    [[nodiscard]] virtual bool findsIconsByName() const noexcept
    {
        return false;
    }
};

/// A source with no icons: synthetic runs, and a platform without an implementation. Every process
/// shows the generic icon.
class UnsupportedProcessIconSource final : public IProcessIconSource
{
  public:
    [[nodiscard]] std::string executablePath(std::int32_t /*pid*/) override
    {
        return {};
    }

    [[nodiscard]] std::optional<IconPixels>
    loadIcon(std::string_view /*executablePath*/, std::string_view /*processName*/, int /*sizePx*/) override
    {
        return std::nullopt;
    }

    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return false;
    }
};

} // namespace Platform
