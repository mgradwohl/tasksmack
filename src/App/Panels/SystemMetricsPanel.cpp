#include "SystemMetricsPanel.h"

#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace App
{

namespace
{

void drawRightAlignedOverlayText(const char* text, float paddingX = 8.0F)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const ImVec2 textSize = ImGui::CalcTextSize(text);

    const float x = rectMax.x - paddingX - textSize.x;
    const float y = rectMin.y + ((rectMax.y - rectMin.y - textSize.y) * 0.5F);
    const ImVec2 pos(x, y);

    const ImU32 shadowCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.0F, pos.y + 1.0F), shadowCol, text);
    ImGui::GetWindowDrawList()->AddText(pos, textCol, text);
}

int hoveredIndexFromPlotX(double mouseX, size_t count)
{
    if (count == 0)
    {
        return -1;
    }

    const double minX = -static_cast<double>(count - 1);
    const double clampedX = std::clamp(mouseX, minX, 0.0);
    const double raw = clampedX - minX;
    const long idx = std::lround(raw);
    return std::clamp(static_cast<int>(idx), 0, static_cast<int>(count - 1));
}

void showCpuBreakdownTooltip(const UI::ColorScheme& scheme,
                             bool showTime,
                             int timeSec,
                             double userPercent,
                             double systemPercent,
                             double iowaitPercent,
                             double idlePercent)
{
    ImGui::BeginTooltip();
    if (showTime)
    {
        ImGui::Text("t: %ds", timeSec);
        ImGui::Separator();
    }
    ImGui::TextColored(scheme.cpuUser, "User: %s", UI::Format::percentCompact(userPercent).c_str());
    ImGui::TextColored(scheme.cpuSystem, "System: %s", UI::Format::percentCompact(systemPercent).c_str());
    ImGui::TextColored(scheme.cpuIowait, "I/O Wait: %s", UI::Format::percentCompact(iowaitPercent).c_str());
    ImGui::TextColored(scheme.cpuIdle, "Idle: %s", UI::Format::percentCompact(idlePercent).c_str());
    ImGui::EndTooltip();
}

} // namespace

SystemMetricsPanel::SystemMetricsPanel() : Panel("System")
{
}

SystemMetricsPanel::~SystemMetricsPanel()
{
    // Direct cleanup to avoid virtual dispatch during destruction
    m_Running = false;
    if (m_SamplerThread.joinable())
    {
        m_SamplerThread.request_stop();
        m_SamplerThread.join();
    }
    m_Model.reset();
}

void SystemMetricsPanel::onAttach()
{
    // Create system model with platform probe
    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe());

    // Start background sampler
    m_Running = true;
    m_SamplerThread = std::jthread(
        [this](std::stop_token stopToken)
        {
            while (!stopToken.stop_requested() && m_Running)
            {
                m_Model->refresh();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });

    spdlog::info("SystemMetricsPanel: initialized with background sampling");
}

void SystemMetricsPanel::onDetach()
{
    m_Running = false;
    if (m_SamplerThread.joinable())
    {
        m_SamplerThread.request_stop();
        m_SamplerThread.join();
    }
    m_Model.reset();
}

void SystemMetricsPanel::render(bool* open)
{
    if (!ImGui::Begin(m_Hostname.c_str(), open))
    {
        ImGui::End();
        return;
    }

    if (!m_Model)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "System model not initialized");
        ImGui::End();
        return;
    }

    // Get thread-safe copy of current snapshot
    auto snap = m_Model->snapshot();

    // Update cached hostname if changed
    if (!snap.hostname.empty() && snap.hostname != m_Hostname)
    {
        m_Hostname = snap.hostname;
    }

    // Delayed layout recalculation:
    // Frame N: Font changes externally
    // Frame N+1: We detect the change, mark layout dirty (but DON'T recalculate yet)
    // Frame N+2: Layout is dirty, so we recalculate (font is now stable)
    auto& theme = UI::Theme::get();
    auto currentFontSize = theme.currentFontSize();
    size_t currentCoreCount = snap.cpuPerCore.size();

    if (currentFontSize != m_LastFontSize || currentCoreCount != m_LastCoreCount)
    {
        // Font or core count changed - mark dirty for NEXT frame
        m_LastFontSize = currentFontSize;
        m_LastCoreCount = currentCoreCount;
        m_LayoutDirty = true;
    }
    else if (m_LayoutDirty)
    {
        // One frame has passed since change, now safe to recalculate
        updateCachedLayout();
        m_LayoutDirty = false;
    }

    // Tabs for different views
    if (ImGui::BeginTabBar("SystemTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))
        {
            renderOverview();
            ImGui::EndTabItem();
        }

        if (snap.coreCount > 1)
        {
            if (ImGui::BeginTabItem("CPU Cores"))
            {
                renderPerCoreSection();
                ImGui::EndTabItem();
            }
        }

        if (ImGui::BeginTabItem("Memory"))
        {
            renderMemorySection();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void SystemMetricsPanel::renderOverview()
{
    auto snap = m_Model->snapshot();

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned)
    // Format uptime string
    std::string uptimeStr;
    if (snap.uptimeSeconds > 0)
    {
        uint64_t days = snap.uptimeSeconds / 86400;
        uint64_t hours = (snap.uptimeSeconds % 86400) / 3600;
        uint64_t minutes = (snap.uptimeSeconds % 3600) / 60;

        char uptimeBuf[64];
        if (days > 0)
        {
            snprintf(uptimeBuf,
                     sizeof(uptimeBuf),
                     "Up: %lud %luh %lum",
                     static_cast<unsigned long>(days),
                     static_cast<unsigned long>(hours),
                     static_cast<unsigned long>(minutes));
        }
        else if (hours > 0)
        {
            snprintf(uptimeBuf, sizeof(uptimeBuf), "Up: %luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
        }
        else
        {
            snprintf(uptimeBuf, sizeof(uptimeBuf), "Up: %lum", static_cast<unsigned long>(minutes));
        }
        uptimeStr = uptimeBuf;
    }

    // Display: "CPU Model (N cores @ X.XX GHz)     Uptime: Xd Yh Zm"
    char coreInfo[64];
    if (snap.cpuFreqMHz > 0)
    {
        snprintf(coreInfo, sizeof(coreInfo), " (%d cores @ %.2f GHz)", snap.coreCount, static_cast<double>(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        snprintf(coreInfo, sizeof(coreInfo), " (%d cores)", snap.coreCount);
    }

    float availWidth = ImGui::GetContentRegionAvail().x;
    float uptimeWidth = ImGui::CalcTextSize(uptimeStr.c_str()).x;

    // CPU model with core count and frequency
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo);

    // Right-align uptime
    if (!uptimeStr.empty())
    {
        ImGui::SameLine(availWidth - uptimeWidth);
        ImGui::TextUnformatted(uptimeStr.c_str());
    }

    ImGui::Spacing();

    // Get theme for colored progress bars
    auto& theme = UI::Theme::get();

    // CPU usage bar with themed color
    ImGui::Text("CPU Usage:");
    ImGui::SameLine(m_OverviewLabelWidth);
    float cpuFraction = static_cast<float>(snap.cpuTotal.totalPercent) / 100.0F;
    const std::string cpuOverlay = UI::Format::percentCompact(snap.cpuTotal.totalPercent);
    ImVec4 cpuColor = theme.progressColor(snap.cpuTotal.totalPercent);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cpuColor);
    // Use custom overlay text (suppress ImGui's default percent overlay)
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), "");
    drawRightAlignedOverlayText(cpuOverlay.c_str());
    ImGui::PopStyleColor();

    // CPU History Graph
    {
        auto cpuHist = m_Model->cpuHistory();
        auto cpuUserHist = m_Model->cpuUserHistory();
        auto cpuSystemHist = m_Model->cpuSystemHistory();
        auto cpuIowaitHist = m_Model->cpuIowaitHistory();
        auto cpuIdleHist = m_Model->cpuIdleHistory();

        const size_t n = std::min({cpuUserHist.size(), cpuSystemHist.size(), cpuIowaitHist.size(), cpuIdleHist.size()});
        std::vector<float> timeData(n);
        for (size_t i = 0; i < n; ++i)
        {
            timeData[i] = static_cast<float>(i) - static_cast<float>(n - 1);
        }

        if (ImPlot::BeginPlot("##OverviewCPUHistory", ImVec2(-1, 200)))
        {
            ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

            if (n > 0)
            {
                std::vector<float> y0(n, 0.0F);
                std::vector<float> yUserTop(n);
                std::vector<float> ySystemTop(n);
                std::vector<float> yIowaitTop(n);
                std::vector<float> yTotalTop(n);

                for (size_t i = 0; i < n; ++i)
                {
                    yUserTop[i] = cpuUserHist[i];
                    ySystemTop[i] = cpuUserHist[i] + cpuSystemHist[i];
                    yIowaitTop[i] = ySystemTop[i] + cpuIowaitHist[i];
                    yTotalTop[i] = yIowaitTop[i] + cpuIdleHist[i];
                }

                ImVec4 userFill = theme.scheme().cpuUser;
                userFill.w = 0.35F;
                ImPlot::SetNextFillStyle(userFill);
                ImPlot::PlotShaded("##CpuUser", timeData.data(), y0.data(), yUserTop.data(), static_cast<int>(n));

                ImVec4 systemFill = theme.scheme().cpuSystem;
                systemFill.w = 0.35F;
                ImPlot::SetNextFillStyle(systemFill);
                ImPlot::PlotShaded("##CpuSystem", timeData.data(), yUserTop.data(), ySystemTop.data(), static_cast<int>(n));

                ImVec4 iowaitFill = theme.scheme().cpuIowait;
                iowaitFill.w = 0.35F;
                ImPlot::SetNextFillStyle(iowaitFill);
                ImPlot::PlotShaded("##CpuIowait", timeData.data(), ySystemTop.data(), yIowaitTop.data(), static_cast<int>(n));

                ImVec4 idleFill = theme.scheme().cpuIdle;
                idleFill.w = 0.20F;
                ImPlot::SetNextFillStyle(idleFill);
                ImPlot::PlotShaded("##CpuIdle", timeData.data(), yIowaitTop.data(), yTotalTop.data(), static_cast<int>(n));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(mouse.x, n);
                    if (idx >= 0)
                    {
                        const size_t si = static_cast<size_t>(idx);
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                idx - static_cast<int>(n - 1),
                                                static_cast<double>(cpuUserHist[si]),
                                                static_cast<double>(cpuSystemHist[si]),
                                                static_cast<double>(cpuIowaitHist[si]),
                                                static_cast<double>(cpuIdleHist[si]));
                    }
                }
            }
            else if (!cpuHist.empty())
            {
                std::vector<float> fallbackTime(cpuHist.size());
                for (size_t i = 0; i < cpuHist.size(); ++i)
                {
                    fallbackTime[i] = static_cast<float>(i) - static_cast<float>(cpuHist.size() - 1);
                }

                ImVec4 fillColor = theme.scheme().chartCpu;
                fillColor.w = 0.3F;
                ImPlot::SetNextFillStyle(fillColor);
                ImPlot::PlotShaded("##CPUShaded", fallbackTime.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);

                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImPlot::PlotLine("##CPU", fallbackTime.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(mouse.x, cpuHist.size());
                    if (idx >= 0)
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %ds", idx - static_cast<int>(cpuHist.size() - 1));
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(static_cast<double>(cpuHist[static_cast<size_t>(idx)])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
            else
            {
                ImPlot::PlotDummy("##CPU");
            }

            ImPlot::EndPlot();
        }
    }

    // CPU breakdown stacked bar (User | System | I/O Wait | Idle)
    {
        ImGui::Text("Breakdown:");
        ImGui::SameLine(m_OverviewLabelWidth);

        ImVec2 barStart = ImGui::GetCursorScreenPos();
        float barWidth = ImGui::GetContentRegionAvail().x;
        float barHeight = ImGui::GetFrameHeight();

        // Calculate segment widths based on percentages
        float userWidth = barWidth * static_cast<float>(snap.cpuTotal.userPercent) / 100.0F;
        float systemWidth = barWidth * static_cast<float>(snap.cpuTotal.systemPercent) / 100.0F;
        float iowaitWidth = barWidth * static_cast<float>(snap.cpuTotal.iowaitPercent) / 100.0F;
        // Idle is the background - drawn first, then overlaid with other segments

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw background
        drawList->AddRectFilled(
            barStart, ImVec2(barStart.x + barWidth, barStart.y + barHeight), ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuIdle), 3.0F);

        float xOffset = 0.0F;

        // User segment
        if (userWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + userWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuUser),
                                    xOffset < 0.5F ? 3.0F : 0.0F,
                                    xOffset < 0.5F ? ImDrawFlags_RoundCornersLeft : 0);
            xOffset += userWidth;
        }

        // System segment
        if (systemWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + systemWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuSystem));
            xOffset += systemWidth;
        }

        // I/O Wait segment
        if (iowaitWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + iowaitWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuIowait));
        }

        // Draw frame border
        drawList->AddRect(barStart,
                          ImVec2(barStart.x + barWidth, barStart.y + barHeight),
                          ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Border)),
                          3.0F);

        // Overlay text (consistent with other bars)
        {
            const std::string breakdownOverlay = std::string("U ") + UI::Format::percentCompact(snap.cpuTotal.userPercent) + "  S " +
                                                 UI::Format::percentCompact(snap.cpuTotal.systemPercent) + "  IO " +
                                                 UI::Format::percentCompact(snap.cpuTotal.iowaitPercent) + "  I " +
                                                 UI::Format::percentCompact(snap.cpuTotal.idlePercent);

            // Reserve space for the bar (so we can draw overlay text based on item rect).
            ImGui::Dummy(ImVec2(barWidth, barHeight));
            drawRightAlignedOverlayText(breakdownOverlay.c_str());
        }

        // Tooltip on hover with breakdown details
        if (ImGui::IsItemHovered())
        {
            showCpuBreakdownTooltip(theme.scheme(),
                                    false,
                                    0,
                                    snap.cpuTotal.userPercent,
                                    snap.cpuTotal.systemPercent,
                                    snap.cpuTotal.iowaitPercent,
                                    snap.cpuTotal.idlePercent);
        }
    }

    ImGui::Spacing();

    // Load average (Linux only, shows nothing on Windows)
    if (snap.loadAvg1 > 0.0 || snap.loadAvg5 > 0.0 || snap.loadAvg15 > 0.0)
    {
        ImGui::Text("Load Avg:");
        ImGui::SameLine(m_OverviewLabelWidth);

        char loadBuf[64];
        snprintf(loadBuf, sizeof(loadBuf), "%.2f / %.2f / %.2f (1/5/15 min)", snap.loadAvg1, snap.loadAvg5, snap.loadAvg15);
        ImGui::TextUnformatted(loadBuf);
    }

    // Memory usage bar with themed color
    ImGui::Text("Memory:");
    ImGui::SameLine(m_OverviewLabelWidth);
    float memFraction = static_cast<float>(snap.memoryUsedPercent) / 100.0F;

    const std::string memOverlay =
        UI::Format::bytesUsedTotalPercentCompact(snap.memoryUsedBytes, snap.memoryTotalBytes, snap.memoryUsedPercent);
    ImVec4 memColor = theme.progressColor(snap.memoryUsedPercent);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, memColor);
    ImGui::ProgressBar(memFraction, ImVec2(-1, 0), "");
    drawRightAlignedOverlayText(memOverlay.c_str());
    ImGui::PopStyleColor();

    // Swap usage bar (if available) with themed color
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Text("Swap:");
        ImGui::SameLine(m_OverviewLabelWidth);
        float swapFraction = static_cast<float>(snap.swapUsedPercent) / 100.0F;

        const std::string swapOverlay =
            UI::Format::bytesUsedTotalPercentCompact(snap.swapUsedBytes, snap.swapTotalBytes, snap.swapUsedPercent);
        ImVec4 swapColor = theme.progressColor(snap.swapUsedPercent);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, swapColor);
        ImGui::ProgressBar(swapFraction, ImVec2(-1, 0), "");
        drawRightAlignedOverlayText(swapOverlay.c_str());
        ImGui::PopStyleColor();
    }
}

void SystemMetricsPanel::renderCpuSection()
{
    const auto& theme = UI::Theme::get();
    auto snap = m_Model->snapshot();
    auto cpuHist = m_Model->cpuHistory();
    auto cpuUserHist = m_Model->cpuUserHistory();
    auto cpuSystemHist = m_Model->cpuSystemHistory();
    auto cpuIowaitHist = m_Model->cpuIowaitHistory();
    auto cpuIdleHist = m_Model->cpuIdleHistory();

    ImGui::Text("CPU History (last %zu samples)", cpuHist.size());
    ImGui::Spacing();

    // Prepare time axis
    std::vector<float> timeData(cpuHist.size());
    for (size_t i = 0; i < cpuHist.size(); ++i)
    {
        timeData[i] = static_cast<float>(i) - static_cast<float>(cpuHist.size() - 1);
    }

    // CPU Usage Plot
    if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 200)))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (!cpuHist.empty())
        {
            ImVec4 fillColor = theme.scheme().chartCpu;
            fillColor.w = 0.3F; // Semi-transparent fill
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);

            // Draw the line on top of the shaded region.
            ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
            ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));

            if (ImPlot::IsPlotHovered())
            {
                const size_t n = cpuHist.size();
                const size_t m = std::min({cpuUserHist.size(), cpuSystemHist.size(), cpuIowaitHist.size(), cpuIdleHist.size()});
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                const int idx = hoveredIndexFromPlotX(mouse.x, n);
                if (idx >= 0)
                {
                    const int timeSec = idx - static_cast<int>(n - 1);
                    if (m == n)
                    {
                        const size_t si = static_cast<size_t>(idx);
                        showCpuBreakdownTooltip(theme.scheme(),
                                                true,
                                                timeSec,
                                                static_cast<double>(cpuUserHist[si]),
                                                static_cast<double>(cpuSystemHist[si]),
                                                static_cast<double>(cpuIowaitHist[si]),
                                                static_cast<double>(cpuIdleHist[si]));
                    }
                    else
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %ds", timeSec);
                        ImGui::Text("CPU: %s", UI::Format::percentCompact(static_cast<double>(cpuHist[static_cast<size_t>(idx)])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
        else
        {
            ImPlot::PlotDummy("##CPU");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Current values
    ImGui::Text("Current: %.1f%% (User: %.1f%%, System: %.1f%%)",
                snap.cpuTotal.totalPercent,
                snap.cpuTotal.userPercent,
                snap.cpuTotal.systemPercent);
}

void SystemMetricsPanel::renderMemorySection()
{
    const auto& theme = UI::Theme::get();
    auto snap = m_Model->snapshot();
    auto memHist = m_Model->memoryHistory();
    auto swapHist = m_Model->swapHistory();

    ImGui::Text("Memory History (last %zu samples)", memHist.size());
    ImGui::Spacing();

    // Prepare time axis
    std::vector<float> timeData(memHist.size());
    for (size_t i = 0; i < memHist.size(); ++i)
    {
        timeData[i] = static_cast<float>(i) - static_cast<float>(memHist.size() - 1);
    }

    // Memory Usage Plot
    if (ImPlot::BeginPlot("##MemoryHistory", ImVec2(-1, 200)))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (!memHist.empty())
        {
            ImVec4 fillColor = theme.scheme().chartMemory;
            fillColor.w = 0.3F; // Semi-transparent fill
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotShaded("##MemoryShaded", timeData.data(), memHist.data(), static_cast<int>(memHist.size()), 0.0);

            // Draw the line on top of the shaded region.
            ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
            ImPlot::PlotLine("##Memory", timeData.data(), memHist.data(), static_cast<int>(memHist.size()));

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                const int idx = hoveredIndexFromPlotX(mouse.x, memHist.size());
                if (idx >= 0)
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("t: %ds", idx - static_cast<int>(memHist.size() - 1));
                    ImGui::Text("Memory: %s", UI::Format::percentCompact(static_cast<double>(memHist[static_cast<size_t>(idx)])).c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        else
        {
            ImPlot::PlotDummy("##Memory");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Memory details
    auto formatBytesWithUnit = [](uint64_t bytes, const UI::Format::ByteUnit unit) -> std::string
    {
        const double value = static_cast<double>(bytes) / unit.scale;
        if (unit.decimals == 1)
        {
            return std::format("{:.1f} {}", value, unit.suffix);
        }
        return std::format("{:.0f} {}", value, unit.suffix);
    };

    auto formatSizeForBar = [](uint64_t usedBytes, uint64_t totalBytes, double percent) -> std::string
    {
        return UI::Format::bytesUsedTotalPercentCompact(usedBytes, totalBytes, percent);
    };

    auto drawMemoryStackedBar = [&](uint64_t totalBytes, uint64_t usedBytes, uint64_t cachedBytes, const char* overlayText)
    {
        if (totalBytes == 0)
        {
            return;
        }

        // Avoid underflow and handle platforms that may report inconsistent values.
        const uint64_t clampedUsedBytes = std::min(usedBytes, totalBytes);
        const uint64_t remainingAfterUsed = totalBytes - clampedUsedBytes;
        const uint64_t clampedCachedBytes = std::min(cachedBytes, remainingAfterUsed);
        const uint64_t otherAvailableBytes = remainingAfterUsed - clampedCachedBytes;

        const float usedFrac = static_cast<float>(static_cast<double>(clampedUsedBytes) / static_cast<double>(totalBytes));
        const float cachedFrac = static_cast<float>(static_cast<double>(clampedCachedBytes) / static_cast<double>(totalBytes));
        const float otherFrac = static_cast<float>(static_cast<double>(otherAvailableBytes) / static_cast<double>(totalBytes));

        const ImVec2 startPos = ImGui::GetCursorScreenPos();
        const float fullWidth = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight();
        const ImVec2 size(fullWidth, height);
        ImGui::InvisibleButton("##MemoryStackedBar", size);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = ImGui::GetStyle().FrameRounding;

        const ImU32 bgCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 usedCol = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartMemory);
        const ImU32 cachedCol = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartIo);
        const ImU32 otherCol = ImGui::GetColorU32(ImGuiCol_FrameBgActive);

        const ImVec2 endPos(startPos.x + size.x, startPos.y + size.y);
        drawList->AddRectFilled(startPos, endPos, bgCol, rounding);

        float x = startPos.x;
        const float usedW = size.x * usedFrac;
        const float cachedW = size.x * cachedFrac;
        const float otherW = size.x * otherFrac;

        if (usedW > 0.0F)
        {
            const ImDrawFlags flags = (usedW + cachedW + otherW >= size.x) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
            drawList->AddRectFilled(ImVec2(x, startPos.y), ImVec2(x + usedW, endPos.y), usedCol, rounding, flags);
            x += usedW;
        }

        if (cachedW > 0.0F)
        {
            const ImDrawFlags flags = (x + cachedW + otherW >= endPos.x) ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
            drawList->AddRectFilled(ImVec2(x, startPos.y), ImVec2(x + cachedW, endPos.y), cachedCol, rounding, flags);
            x += cachedW;
        }

        if (otherW > 0.0F)
        {
            drawList->AddRectFilled(ImVec2(x, startPos.y), endPos, otherCol, rounding, ImDrawFlags_RoundCornersRight);
        }

        // Overlay text (consistent with other bars)
        drawRightAlignedOverlayText(overlayText);

        // Restore cursor position to below the bar.
        ImGui::SetCursorScreenPos(ImVec2(startPos.x, endPos.y + ImGui::GetStyle().ItemInnerSpacing.y));
    };

    const std::string overlay = formatSizeForBar(snap.memoryUsedBytes, snap.memoryTotalBytes, snap.memoryUsedPercent);
    ImGui::TextUnformatted("Memory:");
    ImGui::SameLine();
    drawMemoryStackedBar(snap.memoryTotalBytes, snap.memoryUsedBytes, snap.memoryCachedBytes, overlay.c_str());

    // Details in tooltip (keeps bars consistent across panels)
    if (ImGui::IsItemHovered())
    {
        if (snap.memoryTotalBytes == 0)
        {
            return;
        }

        // Mirror stacked-bar clamping so tooltip numbers match segments.
        const uint64_t totalBytes = snap.memoryTotalBytes;
        const uint64_t usedBytes = std::min(snap.memoryUsedBytes, totalBytes);
        const uint64_t remainingAfterUsed = totalBytes - usedBytes;
        const uint64_t cachedBytes = std::min(snap.memoryCachedBytes, remainingAfterUsed);
        const uint64_t otherBytes = remainingAfterUsed - cachedBytes;

        const UI::Format::ByteUnit unit = UI::Format::unitForTotalBytes(totalBytes);
        const double cachedPercent = (totalBytes > 0) ? (static_cast<double>(cachedBytes) / static_cast<double>(totalBytes) * 100.0) : 0.0;
        const double otherPercent = (totalBytes > 0) ? (static_cast<double>(otherBytes) / static_cast<double>(totalBytes) * 100.0) : 0.0;

        ImGui::BeginTooltip();
        const std::string usedLabel =
            UI::Format::bytesUsedTotalPercentCompact(snap.memoryUsedBytes, snap.memoryTotalBytes, snap.memoryUsedPercent);
        ImGui::Text("Used: %s", usedLabel.c_str());
        ImGui::Text("Cached: %s (%s)", formatBytesWithUnit(cachedBytes, unit).c_str(), UI::Format::percentCompact(cachedPercent).c_str());
        ImGui::Text("Available: %s (%s)", formatBytesWithUnit(otherBytes, unit).c_str(), UI::Format::percentCompact(otherPercent).c_str());
        ImGui::EndTooltip();
    }

    // Swap section
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Swap History (last %zu samples)", swapHist.size());
        ImGui::Spacing();

        std::vector<float> swapTimeData(swapHist.size());
        for (size_t i = 0; i < swapHist.size(); ++i)
        {
            swapTimeData[i] = static_cast<float>(i) - static_cast<float>(swapHist.size() - 1);
        }

        if (ImPlot::BeginPlot("##SwapHistory", ImVec2(-1, 200)))
        {
            ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

            if (!swapHist.empty())
            {
                ImVec4 fillColor = theme.scheme().chartIo;
                fillColor.w = 0.3F; // Semi-transparent fill
                ImPlot::SetNextFillStyle(fillColor);
                ImPlot::PlotShaded("##SwapShaded", swapTimeData.data(), swapHist.data(), static_cast<int>(swapHist.size()), 0.0);

                // Draw the line on top of the shaded region.
                ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                ImPlot::PlotLine("##Swap", swapTimeData.data(), swapHist.data(), static_cast<int>(swapHist.size()));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    const int idx = hoveredIndexFromPlotX(mouse.x, swapHist.size());
                    if (idx >= 0)
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("t: %ds", idx - static_cast<int>(swapHist.size() - 1));
                        ImGui::Text("Swap: %s",
                                    UI::Format::percentCompact(static_cast<double>(swapHist[static_cast<size_t>(idx)])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
            else
            {
                ImPlot::PlotDummy("##Swap");
            }

            ImPlot::EndPlot();
        }

        // Swap usage bar (consistent with other bars: values on the bar)
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Swap:");
            ImGui::SameLine();

            const float swapFraction = static_cast<float>(snap.swapUsedPercent / 100.0);
            const std::string swapOverlay =
                UI::Format::bytesUsedTotalPercentCompact(snap.swapUsedBytes, snap.swapTotalBytes, snap.swapUsedPercent);

            const ImVec4 swapColor = theme.progressColor(snap.swapUsedPercent);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, swapColor);
            ImGui::ProgressBar(swapFraction, ImVec2(-1, 0), "");
            drawRightAlignedOverlayText(swapOverlay.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void SystemMetricsPanel::renderPerCoreSection()
{
    auto snap = m_Model->snapshot();
    auto perCoreHist = m_Model->perCoreHistory();
    auto& theme = UI::Theme::get();

    ImGui::Text("Per-Core CPU Usage (%d cores)", snap.coreCount);
    ImGui::Spacing();

    // ========================================
    // Multi-column progress bars
    // ========================================
    const size_t numCores = snap.cpuPerCore.size();
    if (numCores == 0)
    {
        ImGui::TextColored(theme.scheme().textMuted, "No per-core data available");
        return;
    }

    // Calculate optimal column count based on core count and window width
    float windowWidth = ImGui::GetContentRegionAvail().x;
    constexpr float minColWidth = 200.0F; // Minimum width per column
    int numCols = std::max(1, std::min(4, static_cast<int>(windowWidth / minColWidth)));
    int numRows = static_cast<int>((numCores + static_cast<size_t>(numCols) - 1) / static_cast<size_t>(numCols));

    if (ImGui::BeginTable("PerCoreBars", numCols, ImGuiTableFlags_SizingStretchSame))
    {
        for (int row = 0; row < numRows; ++row)
        {
            ImGui::TableNextRow();
            for (int col = 0; col < numCols; ++col)
            {
                const auto numColsSizeT = static_cast<std::size_t>(numCols);
                const auto coreIdx = (static_cast<std::size_t>(row) * numColsSizeT) + static_cast<std::size_t>(col);
                if (coreIdx >= numCores)
                {
                    break;
                }

                ImGui::TableNextColumn();

                double percent = snap.cpuPerCore[coreIdx].totalPercent;
                float fraction = static_cast<float>(percent) / 100.0F;

                const std::string overlay = UI::Format::percentCompact(percent);

                // Use theme color
                ImVec4 color = theme.progressColor(percent);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);

                // Right-aligned label within cached width
                char label[8];
                snprintf(label, sizeof(label), "%zu", coreIdx);
                float labelW = ImGui::CalcTextSize(label).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m_PerCoreLabelWidth - labelW);
                ImGui::Text("%s", label);
                ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
                ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
                drawRightAlignedOverlayText(overlay.c_str());

                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ========================================
    // Heatmap for history
    // ========================================
    ImGui::Text("Per-Core History (Heatmap)");

    if (!perCoreHist.empty() && !perCoreHist[0].empty())
    {
        size_t historySize = perCoreHist[0].size();
        size_t coreCount = perCoreHist.size();

        // Build heatmap data (row-major: cores × time samples)
        std::vector<double> heatmapData(coreCount * historySize);
        for (size_t core = 0; core < coreCount; ++core)
        {
            for (size_t t = 0; t < historySize; ++t)
            {
                // ImPlot heatmap expects row-major with (0,0) at top-left
                // We want oldest time on left, newest on right
                // And core 0 at top, highest core at bottom
                heatmapData[(core * historySize) + t] = static_cast<double>(perCoreHist[core][t]);
            }
        }

        // Use remaining vertical space in the panel, with minimum height
        float availableHeight = ImGui::GetContentRegionAvail().y;
        float heatmapHeight = std::max(availableHeight, 80.0F);

        // Set up colormap based on theme (only recreate when theme changes)
        std::size_t currentThemeIdx = theme.currentThemeIndex();
        if (m_HeatmapColormap == -1 || currentThemeIdx != m_LastThemeIndex)
        {
            const auto& hm = theme.scheme().heatmap;
            ImVec4 colors[5] = {hm[0], hm[1], hm[2], hm[3], hm[4]};

            // Generate unique name for this theme's colormap
            char cmapName[32];
            snprintf(cmapName, sizeof(cmapName), "CPUHeat_%zu", currentThemeIdx);

            // Check if this colormap already exists
            int existingIdx = ImPlot::GetColormapIndex(cmapName);
            if (existingIdx == -1)
            {
                m_HeatmapColormap = ImPlot::AddColormap(cmapName, &colors[0], 5, true);
            }
            else
            {
                m_HeatmapColormap = existingIdx;
            }
            m_LastThemeIndex = currentThemeIdx;
        }

        ImPlot::PushColormap(m_HeatmapColormap);

        if (ImPlot::BeginPlot(
                "##CPUHeatmap", ImVec2(-1, heatmapHeight), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoFrame))
        {
            // Show numeric row labels (core index) on Y; keep X unlabeled.
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks, ImPlotAxisFlags_NoTickMarks);

            std::vector<double> yTicks(coreCount);
            std::vector<std::string> yTickLabels(coreCount);
            std::vector<const char*> yTickLabelPtrs(coreCount);
            for (size_t core = 0; core < coreCount; ++core)
            {
                yTicks[core] = static_cast<double>(core);
                yTickLabels[core] = std::to_string(core);
                yTickLabelPtrs[core] = yTickLabels[core].c_str();
            }
            ImPlot::SetupAxisTicks(ImAxis_Y1, yTicks.data(), static_cast<int>(coreCount), yTickLabelPtrs.data());

            // Set limits
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(historySize), ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, static_cast<double>(coreCount) - 0.5, ImPlotCond_Always);

            double xMin = 0.0;
            double xMax = static_cast<double>(historySize);
            double yMin = -0.5;
            double yMax = static_cast<double>(coreCount) - 0.5;

            // heatmapData already set up above with correct order:
            // History buffer: index 0 = oldest, index size-1 = newest
            // Direct copy means: left side = oldest, right side = newest

            ImPlot::PlotHeatmap("##heat",
                                heatmapData.data(),
                                static_cast<int>(coreCount),
                                static_cast<int>(historySize),
                                0.0,   // scale min
                                100.0, // scale max
                                nullptr,
                                ImPlotPoint(xMin, yMin),
                                ImPlotPoint(xMax, yMax));

            ImPlot::EndPlot();
        }

        ImPlot::PopColormap();

        // Simple description below the heatmap
        ImGui::TextColored(theme.scheme().textMuted, "0-%zu (top to bottom)  |  Oldest (left) to Now (right)", coreCount - 1);
    }
    else
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting data...");
    }
}

void SystemMetricsPanel::updateCachedLayout()
{
    auto& theme = UI::Theme::get();

    // Overview: width needed for "CPU Usage:" label + spacing
    m_OverviewLabelWidth = ImGui::CalcTextSize("CPU Usage:").x + ImGui::GetStyle().ItemSpacing.x;

    // Per-core: width needed for max core number (e.g., "31" for 32 cores)
    if (m_LastCoreCount > 0)
    {
        char maxLabel[8];
        snprintf(maxLabel, sizeof(maxLabel), "%zu", m_LastCoreCount - 1);
        m_PerCoreLabelWidth = ImGui::CalcTextSize(maxLabel).x;
    }
    else
    {
        m_PerCoreLabelWidth = ImGui::CalcTextSize("0").x;
    }

    spdlog::debug("SystemMetricsPanel: cached layout updated (font={}, overviewWidth={:.1f}, perCoreWidth={:.1f})",
                  static_cast<int>(theme.currentFontSize()),
                  m_OverviewLabelWidth,
                  m_PerCoreLabelWidth);
}

} // namespace App
