// Minimal stub implementations of UI::Theme methods for the test binary.
//
// UserConfig.cpp is compiled into the test binary and references Theme::get(),
// Theme::setThemeById(), Theme::setFontSize(), and Theme::currentThemeId().
// These code paths are never exercised by the tests, but the Windows linker
// (lld-link) requires every referenced symbol to be defined.
//
// Theme.cpp is excluded from the test build because applyImGuiStyle() and
// applyPendingTheme() call ImGui/ImPlot runtime APIs that require an active
// rendering context.  The stubs here satisfy the linker without a context.

#include "UI/Theme.h"

namespace UI
{

// No-op constructor: skips font initialization and fallback theme loading.
Theme::Theme() = default;

// No-op private helpers (called only by the real constructor).
void Theme::initializeFontSizes() {}

void Theme::loadDefaultFallbackTheme() {}

auto Theme::get() -> Theme&
{
    static Theme instance;
    return instance;
}

auto Theme::currentThemeId() const -> const std::string&
{
    static const std::string k_Empty;
    return k_Empty;
}

void Theme::setThemeById(std::string_view /*id*/) {}

void Theme::setFontSize(FontSize /*size*/) {}

} // namespace UI
