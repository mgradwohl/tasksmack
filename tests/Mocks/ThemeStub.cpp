// Minimal stub implementations of UI::Theme methods for the test and benchmark binaries.
//
// UserConfig.cpp is compiled into the test binary and references Theme::get(),
// Theme::setThemeById(), Theme::setFontSize(), and Theme::currentThemeId(). The stub keeps what
// those setters were given so UserConfig::applyToApplication() -> captureFromApplication() can be
// round-tripped in tests (#1187): setThemeById() records the id verbatim (the real Theme ignores an
// id it has no theme for; no theme files are discovered here), and setFontSize() sets the same
// m_CurrentFontSize the inline currentFontSize() returns.
//
// Theme.cpp is excluded from the test build because applyImGuiStyle() and
// applyPendingStyleChanges() call ImGui/ImPlot runtime APIs that require an active
// rendering context.  The stubs here satisfy the linker without a context.

#include "ColorSchemeFields.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace UI
{

// Defaulted constructor: member variables are initialized from their in-class defaults
// (e.g. m_CurrentThemeIndex = 0, m_CurrentFontSize = Medium).  This intentionally
// skips the initializeFontSizes() / loadDefaultFallbackTheme() calls in the real
// Theme::Theme() body, which is what makes this safe as a test stub.
Theme::Theme() = default;

// No-op private helpers.  Defined here so the linker finds them if any other
// translation unit (transitively) references them; the defaulted constructor above
// does not call them.
void Theme::initializeFontSizes()
{}

void Theme::loadDefaultFallbackTheme()
{}

auto Theme::get() -> Theme&
{
    static Theme instance;
    return instance;
}

// The stubs below define Theme's member functions, which can't be made static however little they
// read of the object.
// NOLINTBEGIN(readability-convert-member-functions-to-static)

namespace
{

/// The id last passed to setThemeById(); empty until then, as the real Theme's is before any theme
/// is discovered.
[[nodiscard]] std::string& stubThemeId()
{
    static std::string id;
    return id;
}

} // namespace

auto Theme::currentThemeId() const -> const std::string&
{
    return stubThemeId();
}

namespace
{

/// A scheme whose chart and text colours are all visible. ImGui and ImPlot draw nothing for a fully
/// transparent colour, so with a default (all-zero) scheme the headless chart scenes
/// (benchmarks/ChartGeometryScenes.h, #1421) would emit no line, fill or strip-text geometry at all
/// and measure an empty chart. The exact hues don't matter; that each colour is drawn does. Lines
/// and text are opaque, fills are translucent as a theme's are.
[[nodiscard]] ColorScheme makeVisibleScheme()
{
    constexpr float FILL_ALPHA = 0.35F;
    const auto line = [](float r, float g, float b)
    {
        return ImVec4(r, g, b, 1.0F);
    };
    const auto fill = [](const ImVec4& c)
    {
        return ImVec4(c.x, c.y, c.z, FILL_ALPHA);
    };

    ColorScheme s;
    s.name = "Stub";
    s.textPrimary = line(0.90F, 0.90F, 0.90F);
    s.textMuted = line(0.60F, 0.60F, 0.60F);
    s.textDisabled = line(0.45F, 0.45F, 0.45F);
    s.windowBg = line(0.10F, 0.10F, 0.12F);
    s.frameBg = line(0.16F, 0.16F, 0.18F);
    s.plotGrid = line(0.25F, 0.25F, 0.28F);
    s.chartCpu = line(0.30F, 0.60F, 1.00F);
    s.chartMemory = line(0.40F, 0.85F, 0.45F);
    s.chartIo = line(1.00F, 0.70F, 0.25F);
    s.chartIoWrite = line(1.00F, 0.45F, 0.35F);
    s.chartNetTx = line(0.85F, 0.45F, 0.95F);
    s.chartNetRx = line(0.35F, 0.85F, 0.90F);
    s.chartCpuFill = fill(s.chartCpu);
    s.chartMemoryFill = fill(s.chartMemory);
    s.chartIoFill = fill(s.chartIo);
    s.chartIoWriteFill = fill(s.chartIoWrite);
    s.chartNetTxFill = fill(s.chartNetTx);
    s.chartNetRxFill = fill(s.chartNetRx);
    s.cpuUser = line(0.30F, 0.60F, 1.00F);
    s.cpuSystem = line(1.00F, 0.40F, 0.40F);
    s.cpuIowait = line(1.00F, 0.80F, 0.30F);
    s.cpuIdle = line(0.50F, 0.50F, 0.50F);
    s.cpuUserFill = fill(s.cpuUser);
    s.cpuSystemFill = fill(s.cpuSystem);
    s.cpuIowaitFill = fill(s.cpuIowait);
    s.cpuIdleFill = fill(s.cpuIdle);
    s.chartPeakLine = ImVec4(0.90F, 0.90F, 0.90F, 0.50F);
    // The metric roles (#1196), each in its own colour as a theme draws them.
    s.chartCpuTotal = s.chartCpu;
    s.chartMemoryCached = line(0.70F, 0.95F, 0.75F);
    s.chartMemoryShared = s.chartMemoryCached;
    s.chartSwap = line(0.70F, 0.55F, 0.95F);
    s.chartMemoryVirtual = s.chartSwap;
    s.chartPower = line(0.95F, 0.90F, 0.40F);
    s.chartBattery = line(0.35F, 0.80F, 0.75F);
    s.chartThreads = line(0.95F, 0.50F, 0.75F);
    s.chartHandles = line(0.70F, 0.72F, 0.75F);
    s.chartPageFaults = line(0.65F, 0.68F, 0.98F);
    s.chartGdi = line(0.40F, 0.80F, 0.60F);
    s.chartMemoryCachedFill = fill(s.chartMemoryCached);
    s.chartMemorySharedFill = fill(s.chartMemoryShared);
    s.chartSwapFill = fill(s.chartSwap);
    s.chartMemoryVirtualFill = fill(s.chartMemoryVirtual);
    s.chartPowerFill = fill(s.chartPower);
    s.chartBatteryFill = fill(s.chartBattery);
    s.chartThreadsFill = fill(s.chartThreads);
    s.chartHandlesFill = fill(s.chartHandles);
    s.gpuPower = s.chartPower;
    // Anything still unset -- chrome, GPU series, accents, a role added later -- is drawn in a visible
    // grey rather than not at all; test_ThemeHeader.cpp holds every field to a non-zero alpha.
    TestColorScheme::forEachColor(s,
                                  [](ImVec4& c)
                                  {
                                      if (c.w <= 0.0F)
                                      {
                                          c = ImVec4(0.55F, 0.55F, 0.58F, 1.0F);
                                      }
                                  });
    return s;
}

} // namespace

// Referenced by ChartWidgets.h's chart and value-strip drawing, which test_FillPlotLayout.cpp,
// test_ChartGeometryBudget.cpp and bench_ChartGeometry.cpp run under a live ImGui context. No theme
// file is loaded here; see makeVisibleScheme().
auto Theme::scheme() const -> const ColorScheme&
{
    static const ColorScheme k_Scheme = makeVisibleScheme();
    return k_Scheme;
}

// Referenced by Process Details' Resources chart (Page Faults, GDI Objects), which
// test_ProcessDetailsChartsRender.cpp runs headless. The same lookup as Theme.cpp's.
auto Theme::accentColor(std::size_t index) const -> ImVec4
{
    return scheme().accents[index % accentCount()];
}

// Referenced by ChartWidgets.h's PlotFontGuard. No fonts are loaded here, so charts draw in the
// context's default font, as the headless scenes want.
auto Theme::chartFont() const -> ImFont*
{
    return nullptr;
}

// Referenced by UI/ChromeWidgets.h's sectionHeader(), which the Actions block and priority control
// draw under test_ProcessActionConfirmPopup.cpp and test_ProcessPriorityViewRender.cpp. No fonts are
// loaded here, so headers draw in the context's default font.
auto Theme::boldFont() const -> ImFont*
{
    return nullptr;
}

// Referenced by ChartWidgets.h's lineWeight(). The reference configuration (Medium font, 100 % display
// scale), where authored line weights are drawn as written.
auto Theme::styleScale() const -> float
{
    return 1.0F;
}

void Theme::setThemeById(std::string_view id)
{
    stubThemeId() = id;
}

void Theme::setFontSize(FontSize size)
{
    m_CurrentFontSize = size;
}

// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace UI
