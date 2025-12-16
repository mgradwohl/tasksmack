#include "Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace UI
{

Theme& Theme::get()
{
    static Theme instance;
    return instance;
}

Theme::Theme()
{
    initializeSchemes();
    initializeFontSizes();
}

void Theme::initializeSchemes()
{
    // ============================================
    // Arctic Fire - Blue → Emerald → Amber → Red
    // ============================================
    m_Schemes[static_cast<size_t>(ThemeId::ArcticFire)] = ColorScheme{
        .name = "Arctic Fire",
        .heatmap =
            {
                hexToImVec4(0x1565C0), // Deep Blue (0%)
                hexToImVec4(0x2196F3), // Sky Blue (25%)
                hexToImVec4(0x00E676), // Emerald (50%)
                hexToImVec4(0xFFB300), // Amber (75%)
                hexToImVec4(0xE53935), // Crimson (100%)
            },
        .accents =
            {
                hexToImVec4(0x42A5F5), // Blue
                hexToImVec4(0xFF7043), // Orange
                hexToImVec4(0x00E676), // Emerald
                hexToImVec4(0xAB47BC), // Purple
                hexToImVec4(0xFFC107), // Gold
                hexToImVec4(0xEC407A), // Pink
                hexToImVec4(0xC6FF00), // Lime
                hexToImVec4(0xFF8A65), // Coral
            },
        .progressLow = hexToImVec4(0x00E676),    // Emerald
        .progressMedium = hexToImVec4(0xFFB300), // Amber
        .progressHigh = hexToImVec4(0xE53935),   // Crimson

        // Semantic UI colors
        .textMuted = ImVec4(0.6F, 0.6F, 0.6F, 1.0F),
        .textError = hexToImVec4(0xE53935),   // Crimson
        .textWarning = hexToImVec4(0xFFB300), // Amber
        .textSuccess = hexToImVec4(0x00E676), // Emerald
        .textInfo = hexToImVec4(0x42A5F5),    // Blue

        // Status colors
        .statusRunning = hexToImVec4(0x00E676),  // Emerald
        .statusStopped = hexToImVec4(0xE53935),  // Crimson
        .statusSleeping = hexToImVec4(0xFFB300), // Amber

        // Chart colors
        .chartCpu = hexToImVec4(0x42A5F5),    // Blue
        .chartMemory = hexToImVec4(0x00E676), // Emerald
        .chartIo = hexToImVec4(0xFF7043),     // Orange

        // CPU breakdown colors
        .cpuUser = hexToImVec4(0x42A5F5),          // Blue
        .cpuSystem = hexToImVec4(0xFF7043),        // Orange
        .cpuIowait = hexToImVec4(0xFFB300),        // Amber
        .cpuIdle = ImVec4(0.5F, 0.5F, 0.5F, 1.0F), // Gray
        .cpuSteal = hexToImVec4(0xE53935),         // Crimson

        // Danger button
        .buttonDanger = hexToImVec4(0xC62828),
        .buttonDangerHovered = hexToImVec4(0xE53935),
        .buttonDangerActive = hexToImVec4(0x8B0000),

        // ImGui style colors - Arctic Fire (dark blue-gray base)
        .windowBg = ImVec4(0.08F, 0.10F, 0.14F, 1.0F),
        .childBg = ImVec4(0.08F, 0.10F, 0.14F, 0.0F),
        .popupBg = ImVec4(0.10F, 0.12F, 0.16F, 0.94F),
        .border = ImVec4(0.20F, 0.25F, 0.35F, 0.5F),
        .frameBg = ImVec4(0.12F, 0.16F, 0.22F, 1.0F),
        .frameBgHovered = ImVec4(0.16F, 0.22F, 0.30F, 1.0F),
        .frameBgActive = ImVec4(0.20F, 0.28F, 0.38F, 1.0F),
        .titleBg = ImVec4(0.06F, 0.08F, 0.10F, 1.0F),
        .titleBgActive = ImVec4(0.10F, 0.14F, 0.20F, 1.0F),
        .titleBgCollapsed = ImVec4(0.06F, 0.08F, 0.10F, 0.5F),
        .menuBarBg = ImVec4(0.10F, 0.12F, 0.16F, 1.0F),
        .scrollbarBg = ImVec4(0.06F, 0.08F, 0.10F, 0.5F),
        .scrollbarGrab = ImVec4(0.25F, 0.35F, 0.50F, 1.0F),
        .scrollbarGrabHovered = ImVec4(0.30F, 0.42F, 0.60F, 1.0F),
        .scrollbarGrabActive = ImVec4(0.35F, 0.50F, 0.70F, 1.0F),
        .checkMark = hexToImVec4(0x42A5F5),
        .sliderGrab = hexToImVec4(0x42A5F5),
        .sliderGrabActive = hexToImVec4(0x2196F3),
        .button = ImVec4(0.15F, 0.22F, 0.32F, 1.0F),
        .buttonHovered = ImVec4(0.20F, 0.30F, 0.45F, 1.0F),
        .buttonActive = ImVec4(0.25F, 0.38F, 0.55F, 1.0F),
        .header = ImVec4(0.15F, 0.22F, 0.32F, 1.0F),
        .headerHovered = ImVec4(0.20F, 0.30F, 0.45F, 1.0F),
        .headerActive = ImVec4(0.25F, 0.38F, 0.55F, 1.0F),
        .separator = ImVec4(0.20F, 0.25F, 0.35F, 0.5F),
        .separatorHovered = hexToImVec4(0x42A5F5),
        .separatorActive = hexToImVec4(0x2196F3),
        .resizeGrip = ImVec4(0.25F, 0.35F, 0.50F, 0.2F),
        .resizeGripHovered = hexToImVec4(0x42A5F5),
        .resizeGripActive = hexToImVec4(0x2196F3),
        .tab = ImVec4(0.10F, 0.14F, 0.20F, 1.0F),
        .tabHovered = ImVec4(0.20F, 0.30F, 0.45F, 1.0F),
        .tabActive = ImVec4(0.15F, 0.25F, 0.40F, 1.0F),
        .tabUnfocused = ImVec4(0.08F, 0.10F, 0.14F, 1.0F),
        .tabUnfocusedActive = ImVec4(0.12F, 0.18F, 0.28F, 1.0F),
        .dockingPreview = ImVec4(0.26F, 0.59F, 0.98F, 0.7F),
        .dockingEmptyBg = ImVec4(0.08F, 0.10F, 0.14F, 1.0F),
        .plotLines = hexToImVec4(0x42A5F5),
        .plotLinesHovered = hexToImVec4(0xFF7043),
        .plotHistogram = hexToImVec4(0x00E676),
        .plotHistogramHovered = hexToImVec4(0x69F0AE),
        .tableHeaderBg = ImVec4(0.12F, 0.16F, 0.22F, 1.0F),
        .tableBorderStrong = ImVec4(0.20F, 0.25F, 0.35F, 1.0F),
        .tableBorderLight = ImVec4(0.15F, 0.20F, 0.28F, 1.0F),
        .tableRowBg = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
        .tableRowBgAlt = ImVec4(1.0F, 1.0F, 1.0F, 0.03F),
        .textSelectedBg = ImVec4(0.26F, 0.59F, 0.98F, 0.35F),
        .dragDropTarget = hexToImVec4(0xFFB300),
        .navHighlight = hexToImVec4(0x42A5F5),
        .navWindowingHighlight = ImVec4(1.0F, 1.0F, 1.0F, 0.7F),
        .navWindowingDimBg = ImVec4(0.8F, 0.8F, 0.8F, 0.2F),
        .modalWindowDimBg = ImVec4(0.0F, 0.0F, 0.0F, 0.6F),
    };

    // ============================================
    // Cyberpunk - Neon synthwave aesthetic
    // ============================================
    m_Schemes[static_cast<size_t>(ThemeId::Cyberpunk)] = ColorScheme{
        .name = "Cyberpunk",
        .heatmap =
            {
                hexToImVec4(0x4A148C), // Deep Purple (0%)
                hexToImVec4(0x7C4DFF), // Electric Violet (25%)
                hexToImVec4(0xFF4081), // Hot Magenta (50%)
                hexToImVec4(0xFF6D00), // Neon Orange (75%)
                hexToImVec4(0xFFEA00), // Electric Yellow (100%)
            },
        .accents =
            {
                hexToImVec4(0xFF4081), // Neon Pink
                hexToImVec4(0x00E5FF), // Electric Blue
                hexToImVec4(0x7C4DFF), // Violet
                hexToImVec4(0x76FF03), // Neon Green
                hexToImVec4(0xFF6D00), // Hot Orange
                hexToImVec4(0xE040FB), // Magenta
                hexToImVec4(0xFFEA00), // Yellow
                hexToImVec4(0xFF1744), // Red
            },
        .progressLow = hexToImVec4(0x7C4DFF),    // Violet
        .progressMedium = hexToImVec4(0xFF4081), // Hot Magenta
        .progressHigh = hexToImVec4(0xFFEA00),   // Electric Yellow

        // Semantic UI colors
        .textMuted = ImVec4(0.5F, 0.5F, 0.6F, 1.0F),
        .textError = hexToImVec4(0xFF1744),   // Red
        .textWarning = hexToImVec4(0xFFEA00), // Yellow
        .textSuccess = hexToImVec4(0x76FF03), // Neon Green
        .textInfo = hexToImVec4(0x00E5FF),    // Electric Blue

        // Status colors
        .statusRunning = hexToImVec4(0x76FF03),  // Neon Green
        .statusStopped = hexToImVec4(0xFF1744),  // Red
        .statusSleeping = hexToImVec4(0xFFEA00), // Yellow

        // Chart colors
        .chartCpu = hexToImVec4(0x00E5FF),    // Electric Blue
        .chartMemory = hexToImVec4(0x76FF03), // Neon Green
        .chartIo = hexToImVec4(0xFF4081),     // Neon Pink

        // CPU breakdown colors
        .cpuUser = hexToImVec4(0x00E5FF),          // Electric Blue
        .cpuSystem = hexToImVec4(0xFF4081),        // Neon Pink
        .cpuIowait = hexToImVec4(0xFFEA00),        // Electric Yellow
        .cpuIdle = ImVec4(0.4F, 0.4F, 0.5F, 1.0F), // Muted
        .cpuSteal = hexToImVec4(0xFF1744),         // Red

        // Danger button
        .buttonDanger = hexToImVec4(0xC51162),
        .buttonDangerHovered = hexToImVec4(0xFF4081),
        .buttonDangerActive = hexToImVec4(0x880E4F),

        // ImGui style colors - Cyberpunk (dark purple base with neon accents)
        .windowBg = ImVec4(0.06F, 0.04F, 0.10F, 1.0F),
        .childBg = ImVec4(0.06F, 0.04F, 0.10F, 0.0F),
        .popupBg = ImVec4(0.08F, 0.06F, 0.14F, 0.94F),
        .border = ImVec4(0.30F, 0.15F, 0.40F, 0.5F),
        .frameBg = ImVec4(0.12F, 0.08F, 0.18F, 1.0F),
        .frameBgHovered = ImVec4(0.18F, 0.12F, 0.28F, 1.0F),
        .frameBgActive = ImVec4(0.24F, 0.16F, 0.36F, 1.0F),
        .titleBg = ImVec4(0.04F, 0.02F, 0.08F, 1.0F),
        .titleBgActive = ImVec4(0.10F, 0.06F, 0.16F, 1.0F),
        .titleBgCollapsed = ImVec4(0.04F, 0.02F, 0.08F, 0.5F),
        .menuBarBg = ImVec4(0.08F, 0.06F, 0.14F, 1.0F),
        .scrollbarBg = ImVec4(0.04F, 0.02F, 0.08F, 0.5F),
        .scrollbarGrab = ImVec4(0.40F, 0.20F, 0.60F, 1.0F),
        .scrollbarGrabHovered = hexToImVec4(0x7C4DFF),
        .scrollbarGrabActive = hexToImVec4(0xFF4081),
        .checkMark = hexToImVec4(0x76FF03),
        .sliderGrab = hexToImVec4(0x7C4DFF),
        .sliderGrabActive = hexToImVec4(0xFF4081),
        .button = ImVec4(0.20F, 0.12F, 0.30F, 1.0F),
        .buttonHovered = ImVec4(0.30F, 0.18F, 0.45F, 1.0F),
        .buttonActive = ImVec4(0.40F, 0.25F, 0.55F, 1.0F),
        .header = ImVec4(0.20F, 0.12F, 0.30F, 1.0F),
        .headerHovered = ImVec4(0.30F, 0.18F, 0.45F, 1.0F),
        .headerActive = hexToImVec4(0x7C4DFF),
        .separator = ImVec4(0.30F, 0.15F, 0.40F, 0.5F),
        .separatorHovered = hexToImVec4(0xFF4081),
        .separatorActive = hexToImVec4(0xFFEA00),
        .resizeGrip = ImVec4(0.40F, 0.20F, 0.60F, 0.2F),
        .resizeGripHovered = hexToImVec4(0xFF4081),
        .resizeGripActive = hexToImVec4(0xFFEA00),
        .tab = ImVec4(0.10F, 0.06F, 0.16F, 1.0F),
        .tabHovered = hexToImVec4(0x7C4DFF),
        .tabActive = ImVec4(0.20F, 0.12F, 0.32F, 1.0F),
        .tabUnfocused = ImVec4(0.06F, 0.04F, 0.10F, 1.0F),
        .tabUnfocusedActive = ImVec4(0.14F, 0.08F, 0.22F, 1.0F),
        .dockingPreview = hexToImVec4(0xFF4081),
        .dockingEmptyBg = ImVec4(0.06F, 0.04F, 0.10F, 1.0F),
        .plotLines = hexToImVec4(0x00E5FF),
        .plotLinesHovered = hexToImVec4(0xFF4081),
        .plotHistogram = hexToImVec4(0x76FF03),
        .plotHistogramHovered = hexToImVec4(0xFFEA00),
        .tableHeaderBg = ImVec4(0.12F, 0.08F, 0.20F, 1.0F),
        .tableBorderStrong = ImVec4(0.30F, 0.15F, 0.40F, 1.0F),
        .tableBorderLight = ImVec4(0.20F, 0.10F, 0.30F, 1.0F),
        .tableRowBg = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
        .tableRowBgAlt = ImVec4(1.0F, 1.0F, 1.0F, 0.02F),
        .textSelectedBg = ImVec4(0.49F, 0.30F, 1.0F, 0.35F),
        .dragDropTarget = hexToImVec4(0xFFEA00),
        .navHighlight = hexToImVec4(0xFF4081),
        .navWindowingHighlight = ImVec4(1.0F, 1.0F, 1.0F, 0.7F),
        .navWindowingDimBg = ImVec4(0.8F, 0.8F, 0.8F, 0.2F),
        .modalWindowDimBg = ImVec4(0.0F, 0.0F, 0.0F, 0.6F),
    };

    // ============================================
    // Monochrome - Terminal green aesthetic
    // ============================================
    m_Schemes[static_cast<size_t>(ThemeId::Monochrome)] = ColorScheme{
        .name = "Monochrome",
        .heatmap =
            {
                hexToImVec4(0x1B3D1B), // Dark (0%)
                hexToImVec4(0x2E5D2E), // Dim (25%)
                hexToImVec4(0x4CAF50), // Medium (50%)
                hexToImVec4(0x81C784), // Bright (75%)
                hexToImVec4(0xB9F6CA), // Glow (100%)
            },
        .accents =
            {
                hexToImVec4(0x1B5E20), // Darkest
                hexToImVec4(0x2E7D32),
                hexToImVec4(0x388E3C),
                hexToImVec4(0x43A047),
                hexToImVec4(0x4CAF50),
                hexToImVec4(0x66BB6A),
                hexToImVec4(0x81C784),
                hexToImVec4(0xA5D6A7), // Lightest
            },
        .progressLow = hexToImVec4(0x2E7D32),    // Dim green
        .progressMedium = hexToImVec4(0x4CAF50), // Medium green
        .progressHigh = hexToImVec4(0xB9F6CA),   // Bright glow

        // Semantic UI colors (all green shades for monochrome)
        .textMuted = hexToImVec4(0x66BB6A),   // Muted green
        .textError = hexToImVec4(0xB9F6CA),   // Bright (stands out)
        .textWarning = hexToImVec4(0x81C784), // Bright green
        .textSuccess = hexToImVec4(0x4CAF50), // Medium green
        .textInfo = hexToImVec4(0x66BB6A),    // Light green

        // Status colors (variations of green)
        .statusRunning = hexToImVec4(0x4CAF50),  // Medium
        .statusStopped = hexToImVec4(0xB9F6CA),  // Bright (alert)
        .statusSleeping = hexToImVec4(0x2E7D32), // Dim

        // Chart colors (different green intensities)
        .chartCpu = hexToImVec4(0x81C784),    // Bright
        .chartMemory = hexToImVec4(0x4CAF50), // Medium
        .chartIo = hexToImVec4(0x66BB6A),     // Light

        // CPU breakdown colors (green spectrum)
        .cpuUser = hexToImVec4(0x81C784),   // Bright green
        .cpuSystem = hexToImVec4(0x4CAF50), // Medium green
        .cpuIowait = hexToImVec4(0xB9F6CA), // Glow
        .cpuIdle = hexToImVec4(0x2E7D32),   // Dim green
        .cpuSteal = hexToImVec4(0xB9F6CA),  // Brightest (alert)

        // Danger button (use brightest green for visibility)
        .buttonDanger = hexToImVec4(0x388E3C),
        .buttonDangerHovered = hexToImVec4(0xB9F6CA),
        .buttonDangerActive = hexToImVec4(0x1B5E20),

        // ImGui style colors - Monochrome (dark with green tint)
        .windowBg = ImVec4(0.04F, 0.08F, 0.04F, 1.0F),
        .childBg = ImVec4(0.04F, 0.08F, 0.04F, 0.0F),
        .popupBg = ImVec4(0.06F, 0.10F, 0.06F, 0.94F),
        .border = ImVec4(0.12F, 0.25F, 0.12F, 0.5F),
        .frameBg = ImVec4(0.08F, 0.14F, 0.08F, 1.0F),
        .frameBgHovered = ImVec4(0.12F, 0.20F, 0.12F, 1.0F),
        .frameBgActive = ImVec4(0.16F, 0.28F, 0.16F, 1.0F),
        .titleBg = ImVec4(0.03F, 0.06F, 0.03F, 1.0F),
        .titleBgActive = ImVec4(0.06F, 0.12F, 0.06F, 1.0F),
        .titleBgCollapsed = ImVec4(0.03F, 0.06F, 0.03F, 0.5F),
        .menuBarBg = ImVec4(0.06F, 0.10F, 0.06F, 1.0F),
        .scrollbarBg = ImVec4(0.03F, 0.06F, 0.03F, 0.5F),
        .scrollbarGrab = hexToImVec4(0x2E7D32),
        .scrollbarGrabHovered = hexToImVec4(0x4CAF50),
        .scrollbarGrabActive = hexToImVec4(0x81C784),
        .checkMark = hexToImVec4(0x4CAF50),
        .sliderGrab = hexToImVec4(0x4CAF50),
        .sliderGrabActive = hexToImVec4(0x81C784),
        .button = ImVec4(0.10F, 0.18F, 0.10F, 1.0F),
        .buttonHovered = ImVec4(0.14F, 0.26F, 0.14F, 1.0F),
        .buttonActive = ImVec4(0.18F, 0.35F, 0.18F, 1.0F),
        .header = ImVec4(0.10F, 0.18F, 0.10F, 1.0F),
        .headerHovered = ImVec4(0.14F, 0.26F, 0.14F, 1.0F),
        .headerActive = hexToImVec4(0x388E3C),
        .separator = ImVec4(0.12F, 0.25F, 0.12F, 0.5F),
        .separatorHovered = hexToImVec4(0x4CAF50),
        .separatorActive = hexToImVec4(0x81C784),
        .resizeGrip = ImVec4(0.12F, 0.25F, 0.12F, 0.2F),
        .resizeGripHovered = hexToImVec4(0x4CAF50),
        .resizeGripActive = hexToImVec4(0x81C784),
        .tab = ImVec4(0.06F, 0.10F, 0.06F, 1.0F),
        .tabHovered = hexToImVec4(0x388E3C),
        .tabActive = ImVec4(0.10F, 0.18F, 0.10F, 1.0F),
        .tabUnfocused = ImVec4(0.04F, 0.08F, 0.04F, 1.0F),
        .tabUnfocusedActive = ImVec4(0.08F, 0.14F, 0.08F, 1.0F),
        .dockingPreview = hexToImVec4(0x4CAF50),
        .dockingEmptyBg = ImVec4(0.04F, 0.08F, 0.04F, 1.0F),
        .plotLines = hexToImVec4(0x81C784),
        .plotLinesHovered = hexToImVec4(0xB9F6CA),
        .plotHistogram = hexToImVec4(0x4CAF50),
        .plotHistogramHovered = hexToImVec4(0x81C784),
        .tableHeaderBg = ImVec4(0.08F, 0.14F, 0.08F, 1.0F),
        .tableBorderStrong = ImVec4(0.12F, 0.25F, 0.12F, 1.0F),
        .tableBorderLight = ImVec4(0.08F, 0.18F, 0.08F, 1.0F),
        .tableRowBg = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
        .tableRowBgAlt = ImVec4(0.0F, 0.2F, 0.0F, 0.03F),
        .textSelectedBg = ImVec4(0.18F, 0.55F, 0.18F, 0.35F),
        .dragDropTarget = hexToImVec4(0xB9F6CA),
        .navHighlight = hexToImVec4(0x4CAF50),
        .navWindowingHighlight = ImVec4(0.5F, 1.0F, 0.5F, 0.7F),
        .navWindowingDimBg = ImVec4(0.2F, 0.4F, 0.2F, 0.2F),
        .modalWindowDimBg = ImVec4(0.0F, 0.0F, 0.0F, 0.6F),
    };
}

void Theme::setTheme(ThemeId theme)
{
    if (theme < ThemeId::Count)
    {
        m_CurrentTheme = theme;
        applyImGuiStyle();
    }
}

void Theme::applyImGuiStyle() const
{
    const auto& s = scheme();
    ImGuiStyle& style = ImGui::GetStyle();

    // Apply theme colors to ImGui style
    style.Colors[ImGuiCol_WindowBg] = s.windowBg;
    style.Colors[ImGuiCol_ChildBg] = s.childBg;
    style.Colors[ImGuiCol_PopupBg] = s.popupBg;
    style.Colors[ImGuiCol_Border] = s.border;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    style.Colors[ImGuiCol_FrameBg] = s.frameBg;
    style.Colors[ImGuiCol_FrameBgHovered] = s.frameBgHovered;
    style.Colors[ImGuiCol_FrameBgActive] = s.frameBgActive;
    style.Colors[ImGuiCol_TitleBg] = s.titleBg;
    style.Colors[ImGuiCol_TitleBgActive] = s.titleBgActive;
    style.Colors[ImGuiCol_TitleBgCollapsed] = s.titleBgCollapsed;
    style.Colors[ImGuiCol_MenuBarBg] = s.menuBarBg;
    style.Colors[ImGuiCol_ScrollbarBg] = s.scrollbarBg;
    style.Colors[ImGuiCol_ScrollbarGrab] = s.scrollbarGrab;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = s.scrollbarGrabHovered;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = s.scrollbarGrabActive;
    style.Colors[ImGuiCol_CheckMark] = s.checkMark;
    style.Colors[ImGuiCol_SliderGrab] = s.sliderGrab;
    style.Colors[ImGuiCol_SliderGrabActive] = s.sliderGrabActive;
    style.Colors[ImGuiCol_Button] = s.button;
    style.Colors[ImGuiCol_ButtonHovered] = s.buttonHovered;
    style.Colors[ImGuiCol_ButtonActive] = s.buttonActive;
    style.Colors[ImGuiCol_Header] = s.header;
    style.Colors[ImGuiCol_HeaderHovered] = s.headerHovered;
    style.Colors[ImGuiCol_HeaderActive] = s.headerActive;
    style.Colors[ImGuiCol_Separator] = s.separator;
    style.Colors[ImGuiCol_SeparatorHovered] = s.separatorHovered;
    style.Colors[ImGuiCol_SeparatorActive] = s.separatorActive;
    style.Colors[ImGuiCol_ResizeGrip] = s.resizeGrip;
    style.Colors[ImGuiCol_ResizeGripHovered] = s.resizeGripHovered;
    style.Colors[ImGuiCol_ResizeGripActive] = s.resizeGripActive;
    style.Colors[ImGuiCol_Tab] = s.tab;
    style.Colors[ImGuiCol_TabHovered] = s.tabHovered;
    style.Colors[ImGuiCol_TabSelected] = s.tabActive;
    style.Colors[ImGuiCol_TabDimmed] = s.tabUnfocused;
    style.Colors[ImGuiCol_TabDimmedSelected] = s.tabUnfocusedActive;
    style.Colors[ImGuiCol_DockingPreview] = s.dockingPreview;
    style.Colors[ImGuiCol_DockingEmptyBg] = s.dockingEmptyBg;
    style.Colors[ImGuiCol_PlotLines] = s.plotLines;
    style.Colors[ImGuiCol_PlotLinesHovered] = s.plotLinesHovered;
    style.Colors[ImGuiCol_PlotHistogram] = s.plotHistogram;
    style.Colors[ImGuiCol_PlotHistogramHovered] = s.plotHistogramHovered;
    style.Colors[ImGuiCol_TableHeaderBg] = s.tableHeaderBg;
    style.Colors[ImGuiCol_TableBorderStrong] = s.tableBorderStrong;
    style.Colors[ImGuiCol_TableBorderLight] = s.tableBorderLight;
    style.Colors[ImGuiCol_TableRowBg] = s.tableRowBg;
    style.Colors[ImGuiCol_TableRowBgAlt] = s.tableRowBgAlt;
    style.Colors[ImGuiCol_TextSelectedBg] = s.textSelectedBg;
    style.Colors[ImGuiCol_DragDropTarget] = s.dragDropTarget;
    style.Colors[ImGuiCol_NavHighlight] = s.navHighlight;
    style.Colors[ImGuiCol_NavWindowingHighlight] = s.navWindowingHighlight;
    style.Colors[ImGuiCol_NavWindowingDimBg] = s.navWindowingDimBg;
    style.Colors[ImGuiCol_ModalWindowDimBg] = s.modalWindowDimBg;

    // Text color - keep it light for visibility
    style.Colors[ImGuiCol_Text] = ImVec4(0.92F, 0.92F, 0.92F, 1.0F);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.5F, 0.5F, 0.5F, 1.0F);
}

const ColorScheme& Theme::scheme() const
{
    return m_Schemes[static_cast<size_t>(m_CurrentTheme)];
}

const ColorScheme& Theme::scheme(ThemeId id) const
{
    return m_Schemes[static_cast<size_t>(id)];
}

std::string_view Theme::themeName(ThemeId id) const
{
    return m_Schemes[static_cast<size_t>(id)].name;
}

ImVec4 Theme::heatmapColor(double percent) const
{
    const auto& hm = scheme().heatmap;

    // Clamp percent to 0-100
    percent = std::clamp(percent, 0.0, 100.0);

    // Map 0-100 to 0-4 (5 stops)
    double scaled = percent / 25.0; // 0-4
    size_t idx = static_cast<size_t>(scaled);
    double frac = scaled - static_cast<double>(idx);

    // Clamp index
    if (idx >= 4)
    {
        return hm[4];
    }

    // Lerp between two colors
    const ImVec4& c1 = hm[idx];
    const ImVec4& c2 = hm[idx + 1];
    float t = static_cast<float>(frac);

    return ImVec4(c1.x + (c2.x - c1.x) * t, c1.y + (c2.y - c1.y) * t, c1.z + (c2.z - c1.z) * t, 1.0F);
}

ImVec4 Theme::progressColor(double percent) const
{
    const auto& s = scheme();
    if (percent > 80.0)
    {
        return s.progressHigh;
    }
    if (percent > 50.0)
    {
        return s.progressMedium;
    }
    return s.progressLow;
}

ImVec4 Theme::accentColor(size_t index) const
{
    return scheme().accents[index % accentCount()];
}

void Theme::initializeFontSizes()
{
    m_FontSizes[static_cast<size_t>(FontSize::Small)] = FontSizeConfig{
        .name = "Small",
        .regularPt = 6.0F,
        .largePt = 8.0F,
    };

    m_FontSizes[static_cast<size_t>(FontSize::Medium)] = FontSizeConfig{
        .name = "Medium",
        .regularPt = 8.0F,
        .largePt = 10.0F,
    };

    m_FontSizes[static_cast<size_t>(FontSize::Large)] = FontSizeConfig{
        .name = "Large",
        .regularPt = 10.0F,
        .largePt = 12.0F,
    };

    m_FontSizes[static_cast<size_t>(FontSize::ExtraLarge)] = FontSizeConfig{
        .name = "Extra Large",
        .regularPt = 12.0F,
        .largePt = 14.0F,
    };
}

void Theme::setFontSize(FontSize size)
{
    if (size < FontSize::Count && size != m_CurrentFontSize)
    {
        spdlog::info("Theme: changing font size from {} to {}", static_cast<int>(m_CurrentFontSize), static_cast<int>(size));
        m_CurrentFontSize = size;
    }
}

const FontSizeConfig& Theme::fontConfig() const
{
    return m_FontSizes[static_cast<size_t>(m_CurrentFontSize)];
}

const FontSizeConfig& Theme::fontConfig(FontSize size) const
{
    return m_FontSizes[static_cast<size_t>(size)];
}

bool Theme::increaseFontSize()
{
    auto current = static_cast<size_t>(m_CurrentFontSize);
    auto max = static_cast<size_t>(FontSize::Count) - 1;

    if (current < max)
    {
        setFontSize(static_cast<FontSize>(current + 1));
        return true;
    }
    return false;
}

bool Theme::decreaseFontSize()
{
    auto current = static_cast<size_t>(m_CurrentFontSize);

    if (current > 0)
    {
        setFontSize(static_cast<FontSize>(current - 1));
        return true;
    }
    return false;
}

ImFont* Theme::regularFont() const
{
    return m_Fonts[static_cast<size_t>(m_CurrentFontSize)].regular;
}

ImFont* Theme::largeFont() const
{
    return m_Fonts[static_cast<size_t>(m_CurrentFontSize)].large;
}

void Theme::registerFonts(FontSize size, ImFont* regular, ImFont* large)
{
    if (size < FontSize::Count)
    {
        m_Fonts[static_cast<size_t>(size)].regular = regular;
        m_Fonts[static_cast<size_t>(size)].large = large;
        spdlog::debug("Theme: registered fonts for size {} (regular={}, large={})",
                      static_cast<int>(size),
                      static_cast<void*>(regular),
                      static_cast<void*>(large));
    }
}

} // namespace UI
