#pragma once

/// @file InterfaceScale.h
/// @brief The Interface scale setting's range and rule (#3376). No SDL or ImGui, so App's config code (and the
/// user-config fuzzer, which builds without them) can use it; DpiScale.h includes it.

namespace UI
{

/// The Interface scale setting's range, in percent (#3376); 0 (or anything outside it) means Auto.
inline constexpr int INTERFACE_SCALE_MIN_PERCENT = 100;
inline constexpr int INTERFACE_SCALE_MAX_PERCENT = 300;

/// The display scale to build the UI at: the Interface scale setting when it names one (@p overridePercent in
/// [INTERFACE_SCALE_MIN_PERCENT, INTERFACE_SCALE_MAX_PERCENT]), otherwise what SDL reports (@p measured, the raw display
/// scale: 1.75 at 175%). Callers divide the answer by the window's pixel density (windowUnitScale()) as they do the
/// display's own scale, so the setting means the same on-screen size on every platform, native Wayland included. The
/// override is for a display that misreports its scale: WSLg tells Linux apps 100% whatever Windows uses (#3376).
[[nodiscard]] constexpr float effectiveDisplayScale(float measured, int overridePercent) noexcept
{
    if (overridePercent >= INTERFACE_SCALE_MIN_PERCENT && overridePercent <= INTERFACE_SCALE_MAX_PERCENT)
    {
        return static_cast<float>(overridePercent) / 100.0F;
    }
    return measured;
}

} // namespace UI
