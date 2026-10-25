#pragma once

// Process icons on Linux (#1800), best effort: a process's executable from /proc/[pid]/exe, and the
// icon of the desktop entry that runs it, found as DesktopEntryIcons.h describes and decoded with
// stb_image (PNG only: no SVG decoder is bundled). A process with no desktop entry -- most daemons,
// shells and kernel threads -- has no icon, and shows the generic one.

#include "Platform/IProcessIconSource.h"
#include "Platform/IconImageFile.h"
#include "Platform/Linux/DesktopEntryIcons.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Platform
{

class LinuxProcessIconSource final : public IProcessIconSource
{
  public:
    /// How long the desktop entries read stay indexed before they are read again, so an application
    /// installed while TaskSmack runs is found.
    static constexpr std::chrono::minutes INDEX_LIFETIME{10};

    /// The XDG directories from the environment ($HOME, $XDG_DATA_HOME, $XDG_DATA_DIRS).
    LinuxProcessIconSource();

    /// Test seam: these directories, and processes under @p procRoot instead of /proc.
    LinuxProcessIconSource(DesktopEntryIcons::SearchDirs dirs, std::filesystem::path procRoot);

    [[nodiscard]] std::string executablePath(std::int32_t pid) override;
    [[nodiscard]] std::optional<IconPixels> loadIcon(std::string_view executablePath, std::string_view processName, int sizePx) override;
    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return false;
    }
    /// A python3's or wine64-preloader's icon is its script's or program's, by the process name (#2462).
    [[nodiscard]] bool findsIconsByName() const noexcept override
    {
        return true;
    }

    /// The pixels of the PNG at @p path as straight RGBA; nullopt when it can't be decoded or is
    /// larger than MAX_ICON_IMAGE_SIDE_PX on a side.
    [[nodiscard]] static std::optional<IconPixels> decodePng(const std::filesystem::path& path);

  private:
    const DesktopEntryIcons::DesktopIndex& index();

    DesktopEntryIcons::SearchDirs m_Dirs;
    std::filesystem::path m_ProcRoot;
    std::optional<DesktopEntryIcons::DesktopIndex> m_Index;
    std::chrono::steady_clock::time_point m_IndexReadAt;
};

} // namespace Platform
