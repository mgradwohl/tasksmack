#include "ProcessesPanel.h"

#include "App/Panel.h"
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{

namespace
{

constexpr float TREE_INDENT_WIDTH = 16.0F; // Indent width per tree level in pixels
constexpr int MAX_TREE_DEPTH = 1000;       // Maximum tree depth to detect cycles or malformed data

[[nodiscard]] std::chrono::milliseconds
chooseAdaptiveProcessInterval(const std::chrono::milliseconds baseInterval, const bool isActiveTab, const bool interactionRedrawActive)
{
    const auto baseMs = std::max(baseInterval.count(), static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));

    // During active resize/move interactions, dramatically reduce update frequency to preserve UI responsiveness.
    // ProcessModel refresh includes expensive probe work; batching updates prevents stalls during interaction.
    if (interactionRedrawActive)
    {
        // 3x multiplier during interaction: defer model updates while user is actively resizing/dragging
        const auto throttledMs = std::min(baseMs * 3LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
        return std::chrono::milliseconds(throttledMs);
    }

    if (!isActiveTab)
    {
        // 2x multiplier for inactive tabs (already optimized but far less critical than interaction)
        const auto relaxedMs = std::min(baseMs * 2LL, static_cast<long long>(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
        return std::chrono::milliseconds(relaxedMs);
    }

    // Active tab, no interaction: use base interval
    return std::chrono::milliseconds(baseMs);
}

constexpr float INTERACTION_INTERVAL_HOLD_SECONDS = 0.40F;

// Column-specific unit widths (based on longest unit that can appear)
// Unit widths measured by longest unit string that column can display
// using "W" as a wide character placeholder.
constexpr std::string_view UNIT_PERCENT = "%";           // CPU %, MEM % (actual rendered unit)
constexpr std::string_view UNIT_BYTES = " WW";           // VIRT, RES, PEAK, SHR (longest: " GB")
constexpr std::string_view UNIT_BYTES_PER_SEC = " WW/W"; // I/O, Net (longest: " GB/s")
constexpr std::string_view UNIT_POWER = " WW";           // Power (mW is wider than µW in most fonts)

// Static UI labels (cached for text size measurements)
constexpr std::string_view TREE_VIEW_LABEL = "Tree View";
constexpr std::string_view LIST_VIEW_LABEL = "List View";

[[nodiscard]] auto lowerAscii(char ch) -> int
{
    // Safe/necessary: std::tolower is undefined for negative signed char values (except EOF).
    // Cast to unsigned char to avoid UB when char is signed.
    return std::tolower(static_cast<unsigned char>(ch));
}

[[nodiscard]] constexpr auto toImGuiId(ProcessColumn col) noexcept -> ImGuiID
{
    // Safe: ProcessColumn is a small uint8_t-backed enum; ImGuiID is a wider unsigned type.
    return ImGuiID{std::to_underlying(col)};
}

[[nodiscard]] auto columnFromUserId(ImGuiID id) -> std::optional<ProcessColumn>
{
    // Map back via known columns to avoid integer->enum casts.
    for (const ProcessColumn col : allProcessColumns())
    {
        if (toImGuiId(col) == id)
        {
            return col;
        }
    }

    return std::nullopt;
}

void renderRightAlignedText(std::string_view text)
{
    // GetContentRegionAvail() properly returns space from cursor to right edge of cell
    const float textWidth = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float currentX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(currentX + std::max(0.0F, availWidth - textWidth));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

[[nodiscard]] auto formatAlignedPercentString(double percent) -> std::string
{
    const auto parts = UI::Format::splitPercentForAlignment(percent);

    std::string out;
    out.reserve(parts.wholePart.size() + 2);
    out.append(parts.wholePart.data(), parts.wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(UI::Format::AlignedPercentParts::unitPart.data(), UI::Format::AlignedPercentParts::unitPart.size());
    return out;
}

void renderRightAlignedPercentText(std::string_view text,
                                   const std::array<float, 101>& wholeWithDotWidths,
                                   float decimalDigitWidth,
                                   float unitPercentWidth)
{
    if (text.empty() || text == "-")
    {
        renderRightAlignedText(text);
        return;
    }

    const std::size_t dotPos = text.find('.');
    if (dotPos == std::string_view::npos || (dotPos + 1) >= text.size())
    {
        renderRightAlignedText(text);
        return;
    }

    const std::string_view wholePart = text.substr(0, dotPos + 1); // includes trailing '.'
    const std::string_view decimalPart = text.substr(dotPos + 1, 1);
    const std::string_view unitPart = (dotPos + 2 < text.size()) ? text.substr(dotPos + 2) : std::string_view{"%"};

    std::int32_t wholeValue = -1;
    const std::string_view wholeDigits = text.substr(0, dotPos);
    const auto parseResult = std::from_chars(wholeDigits.data(), wholeDigits.data() + wholeDigits.size(), wholeValue);

    float wholeWidth = 0.0F;
    if (parseResult.ec == std::errc{} && parseResult.ptr == (wholeDigits.data() + wholeDigits.size()) && wholeValue >= 0 &&
        wholeValue <= 100)
    {
        wholeWidth = wholeWithDotWidths[static_cast<std::size_t>(wholeValue)];
    }
    else
    {
        wholeWidth = ImGui::CalcTextSize(wholePart.data(), wholePart.data() + wholePart.size()).x;
    }

    const float cellStartX = ImGui::GetCursorPosX();
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float cellEndX = cellStartX + availWidth;

    const float unitRegionStart = cellEndX - unitPercentWidth;
    const float decimalRegionStart = unitRegionStart - decimalDigitWidth;
    const float wholeStartX = decimalRegionStart - wholeWidth;

    ImGui::SetCursorPosX(std::max(cellStartX, wholeStartX));
    ImGui::TextUnformatted(wholePart.data(), wholePart.data() + wholePart.size());

    ImGui::SetCursorPosX(decimalRegionStart);
    ImGui::TextUnformatted(decimalPart.data(), decimalPart.data() + decimalPart.size());

    ImGui::SetCursorPosX(unitRegionStart);
    ImGui::TextUnformatted(unitPart.data(), unitPart.data() + unitPart.size());
}

[[nodiscard]] auto formatAlignedBytesString(double bytes, UI::Format::ByteUnit unit) -> std::string
{
    const auto parts = UI::Format::splitBytesForAlignmentFast(bytes, unit);
    const auto wholePart = parts.wholePart();
    std::string out;
    out.reserve(wholePart.size() + parts.unitPart.size() + 1);
    out.append(wholePart.data(), wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(parts.unitPart.data(), parts.unitPart.size());
    return out;
}

[[nodiscard]] auto formatAlignedBytesPerSecString(double bytesPerSec, UI::Format::ByteUnit unit) -> std::string
{
    const auto parts = UI::Format::splitBytesPerSecForAlignmentFast(bytesPerSec, unit);
    const auto wholePart = parts.wholePart();
    std::string out;
    out.reserve(wholePart.size() + parts.unitPart.size() + 1);
    out.append(wholePart.data(), wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(parts.unitPart.data(), parts.unitPart.size());
    return out;
}

[[nodiscard]] auto formatAlignedPowerString(double watts) -> std::string
{
    const auto parts = UI::Format::splitPowerForAlignment(watts);
    std::string out;
    out.reserve(parts.wholePart.size() + parts.decimalPart.size() + parts.unitPart.size());
    out.append(parts.wholePart);
    out.append(parts.decimalPart);
    out.append(parts.unitPart);
    return out;
}

} // namespace

// ============================================================================
// TextSizeCache implementation
// ============================================================================

bool ProcessesPanel::TextSizeCache::isValid() const noexcept
{
    // Cache invalid if not yet populated or if font has changed
    return fontPtr != nullptr && fontPtr == ImGui::GetFont();
}

void ProcessesPanel::TextSizeCache::populate()
{
    // Store current font pointer for invalidation detection
    fontPtr = ImGui::GetFont();

    // Cache column header widths
    for (const ProcessColumn col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        columnHeaderWidths[toIndex(col)] = ImGui::CalcTextSize(info.name.data(), info.name.data() + info.name.size()).x;
    }

    // Cache unit string widths
    unitPercentWidth = ImGui::CalcTextSize(UNIT_PERCENT.data(), UNIT_PERCENT.data() + UNIT_PERCENT.size()).x;
    unitBytesWidth = ImGui::CalcTextSize(UNIT_BYTES.data(), UNIT_BYTES.data() + UNIT_BYTES.size()).x;
    unitBytesPerSecWidth = ImGui::CalcTextSize(UNIT_BYTES_PER_SEC.data(), UNIT_BYTES_PER_SEC.data() + UNIT_BYTES_PER_SEC.size()).x;
    unitPowerWidth = ImGui::CalcTextSize(UNIT_POWER.data(), UNIT_POWER.data() + UNIT_POWER.size()).x;
    singleDigitWidth = ImGui::CalcTextSize("0").x;
    for (std::size_t i = 0; i < percentWholeWithDotWidths.size(); ++i)
    {
        std::string wholeWithDot = std::to_string(i);
        wholeWithDot.push_back('.');
        percentWholeWithDotWidths[i] = ImGui::CalcTextSize(wholeWithDot.c_str()).x;
    }

    // Cache static label widths
    treeViewLabelWidth = ImGui::CalcTextSize(TREE_VIEW_LABEL.data(), TREE_VIEW_LABEL.data() + TREE_VIEW_LABEL.size()).x;
    listViewLabelWidth = ImGui::CalcTextSize(LIST_VIEW_LABEL.data(), LIST_VIEW_LABEL.data() + LIST_VIEW_LABEL.size()).x;
}

void ProcessesPanel::ensureTextSizeCacheValid()
{
    if (!m_TextSizeCache.isValid())
    {
        m_TextSizeCache.populate();
    }
}

// ============================================================================
// ProcessesPanel implementation
// ============================================================================

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{}

ProcessesPanel::~ProcessesPanel()
{
    // Stop the background sampler before releasing the model: the sampler callback holds a
    // raw pointer to the model, so the thread must be joined before the model is destroyed.
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    if (m_ProcessModel)
    {
        m_ProcessModel->setInteractionActive(false);
    }
    this->m_InteractionHoldSeconds = 0.0F;
    m_ProcessModel.reset();
}

void ProcessesPanel::onAttach()
{
    // Load column settings from user config
    m_ColumnSettings = UserConfig::get().settings().processColumns;

    const int intervalMs = UserConfig::get().settings().refreshIntervalMs;
    m_RefreshInterval = std::chrono::milliseconds(intervalMs);
    m_AppliedSamplerInterval = m_RefreshInterval;
    this->m_InteractionHoldSeconds = 0.0F;
    m_ForceRefresh = false;

    // Create probe, extract capabilities/ticks/systemMemory, then transfer probe ownership
    // to BackgroundSampler so enumeration runs off the main thread.
    auto processProbe = Platform::makeProcessProbe();

    const int socketStatsCacheTtlMs = UserConfig::get().settings().socketStatsCacheTtlMs;
    processProbe->setSocketStatsCacheTtl(std::chrono::milliseconds(socketStatsCacheTtlMs));

    const Platform::ProcessCapabilities caps = processProbe->capabilities();
    const long ticks = processProbe->ticksPerSecond();
    const std::uint64_t systemMem = processProbe->systemTotalMemory();

    // Build ProcessModel in background-sampler mode: no probe, data arrives via callback.
    m_ProcessModel = std::make_unique<Domain::ProcessModel>(caps, ticks, systemMem);

    // Seed with one synchronous read so the first background callback produces valid CPU
    // deltas instead of all-zero percentages (first call establishes the prev-sample
    // baseline; second call — coming from the background thread — computes the delta).
    const auto seedCounters = processProbe->enumerate();
    const std::uint64_t seedCpuTime = processProbe->totalCpuTime();
    m_ProcessModel->updateFromCounters(seedCounters, seedCpuTime);

    // Wire sampler: owns the probe, fires callback on each interval tick.
    Domain::SamplerConfig samplerCfg{m_AppliedSamplerInterval};
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(std::move(processProbe), samplerCfg);
    m_Sampler->setCallback(
        [model = m_ProcessModel.get()](const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime)
        { model->updateFromCounters(counters, totalCpuTime); });
    m_Sampler->start();

    m_LastSnapshotVersion = m_ProcessModel->snapshotVersion();

    spdlog::info("ProcessesPanel: initialized with background sampler ({}ms interval)", intervalMs);
}

void ProcessesPanel::setSamplingInterval(std::chrono::milliseconds interval)
{
    m_RefreshInterval = interval;
    m_AppliedSamplerInterval = interval;
    if (m_Sampler)
    {
        m_Sampler->setInterval(m_AppliedSamplerInterval);
    }
    m_ForceRefresh = true;
}

void ProcessesPanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void ProcessesPanel::onDetach()
{
    // Save column settings to user config
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    // Stop sampler thread before releasing the model (callback holds a raw pointer to it).
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    this->m_InteractionHoldSeconds = 0.0F;
    m_ProcessModel.reset();
}

void ProcessesPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            const bool wasActive = m_IsActiveTab;
            m_IsActiveTab = (e.tabName() == "Processes");
            if (!wasActive && m_IsActiveTab)
            {
                // Catch up quickly when tab becomes visible again.
                m_ForceRefresh = true;
            }
            return false;
        });
    dispatcher.dispatch<Core::ThemeChangedEvent>(
        [this](Core::ThemeChangedEvent&)
        {
            // Invalidate text cache and request refresh
            m_TextSizeCache.fontPtr = nullptr;
            m_ForceRefresh = true;
            return false;
        });
    dispatcher.dispatch<Core::FontSizeChangedEvent>(
        [this](Core::FontSizeChangedEvent&)
        {
            m_TextSizeCache.fontPtr = nullptr;
            m_ForceRefresh = true;
            return false;
        });
}

void ProcessesPanel::onUpdate(float deltaTime)
{
    if (!m_ProcessModel || !m_Sampler)
    {
        return;
    }

    const bool interactionRedrawActive = Core::Application::get().isInteractionRedrawActive();
    if (interactionRedrawActive)
    {
        this->m_InteractionHoldSeconds = INTERACTION_INTERVAL_HOLD_SECONDS;
    }
    else if (this->m_InteractionHoldSeconds > 0.0F)
    {
        const float clampedDeltaTime = std::max(0.0F, deltaTime);
        this->m_InteractionHoldSeconds = std::max(0.0F, this->m_InteractionHoldSeconds - clampedDeltaTime);
    }

    const bool throttleForInteraction = interactionRedrawActive || (this->m_InteractionHoldSeconds > 0.0F);
    m_ProcessModel->setInteractionActive(throttleForInteraction);
    const auto desiredInterval = chooseAdaptiveProcessInterval(m_RefreshInterval, m_IsActiveTab, throttleForInteraction);
    if (desiredInterval != m_AppliedSamplerInterval)
    {
        m_AppliedSamplerInterval = desiredInterval;
        m_Sampler->setInterval(m_AppliedSamplerInterval);
    }

    // Force-refresh: ask the background sampler to run an extra enumeration immediately.
    if (m_ForceRefresh)
    {
        m_Sampler->requestRefresh();
        m_ForceRefresh = false;
    }

    // Detect and copy new data in a single lock acquisition.
    std::uint64_t newVersion = m_LastSnapshotVersion;
    if (m_ProcessModel->tryCopySnapshotsIfNewer(m_LastSnapshotVersion, m_CachedRenderSnapshots, newVersion))
    {
        m_LastSnapshotVersion = newVersion;
        m_CachedSnapshotVersion = newVersion;

        // Only rebuild tree if tree view is currently active (avoid CPU waste when showing list).
        // If in list mode, mark tree as stale; will rebuild on-demand when switching to tree view.
        if (m_TreeViewEnabled)
        {
            if (Core::Application::get().isInteractionRedrawActive())
            {
                // Defer rebuild during active resize/move interactions for UI responsiveness
                m_TreeRebuildDeferred = true;
            }
            else
            {
                // Rebuild immediately when not in interaction
                m_CachedTree = buildProcessTree(m_CachedRenderSnapshots);
                m_TreeRebuildDeferred = false;
                m_TreeNeedsRebuild = false;
            }
        }
        else
        {
            // In list mode: mark tree as needing rebuild, don't do expensive work now
            m_TreeNeedsRebuild = true;
            m_TreeRebuildDeferred = false;
        }
    }

    // Process deferred tree rebuild when interaction ends and tree view is still active
    if (m_TreeViewEnabled && m_TreeRebuildDeferred && !Core::Application::get().isInteractionRedrawActive())
    {
        m_CachedTree = buildProcessTree(m_CachedRenderSnapshots);
        m_TreeRebuildDeferred = false;
        m_TreeNeedsRebuild = false;
    }
}

int ProcessesPanel::visibleColumnCount() const
{
    int count = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (m_ColumnSettings.isVisible(col))
        {
            ++count;
        }
    }
    return count;
}

void ProcessesPanel::render(bool* open)
{
    if (!ImGui::Begin(ICON_FA_LIST " Processes", open))
    {
        ImGui::End();
        return;
    }

    renderContent();

    ImGui::End();
}

void ProcessesPanel::renderContent()
{
    if (!m_ProcessModel)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "Process model not initialized");
        return;
    }

    // Skip rendering when tab is inactive (data collection continues in onUpdate)
    if (!m_IsActiveTab)
    {
        return;
    }

    // Ensure text size cache is valid for current font (called once per frame)
    ensureTextSizeCacheValid();

    // Get thread-safe copy of snapshots — only when data has actually changed (version-cached).
    // ProcessModel updates at 1Hz but render runs at 60fps; skip 59/60 redundant deep copies.
    const auto currentVersion = m_ProcessModel->snapshotVersion();
    if (currentVersion != m_CachedSnapshotVersion)
    {
        std::uint64_t copiedVersion = m_CachedSnapshotVersion;
        if (m_ProcessModel->tryCopySnapshotsIfNewer(m_CachedSnapshotVersion, m_CachedRenderSnapshots, copiedVersion))
        {
            m_CachedSnapshotVersion = copiedVersion;
        }
    }
    const auto& currentSnapshots = m_CachedRenderSnapshots;

    // Rebuild row format cache when snapshot data changes (~1Hz), never per frame (60fps).
    // Eliminates heap allocations for slow-changing formatted columns in renderProcessRow.
    if (m_CachedSnapshotVersion != m_RowFormatCacheVersion)
    {
        m_RowFormatCache.clear();
        m_RowFormatCache.reserve(currentSnapshots.size());
        for (const auto& proc : currentSnapshots)
        {
            RowFormatCache fmt;
            fmt.ppid = UI::Format::formatId(proc.parentPid);
            fmt.startTime = UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch);
            fmt.cpuTime = UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds);
            fmt.cpuPercent = formatAlignedPercentString(proc.cpuPercent);
            fmt.memPercent = formatAlignedPercentString(proc.memoryPercent);
            fmt.virtualMem =
                formatAlignedBytesString(static_cast<double>(proc.virtualBytes), UI::Format::unitForTotalBytes(proc.virtualBytes));
            fmt.resident = formatAlignedBytesString(static_cast<double>(proc.memoryBytes), UI::Format::unitForTotalBytes(proc.memoryBytes));
            fmt.peakRss =
                formatAlignedBytesString(static_cast<double>(proc.peakMemoryBytes), UI::Format::unitForTotalBytes(proc.peakMemoryBytes));
            fmt.shared = formatAlignedBytesString(static_cast<double>(proc.sharedBytes), UI::Format::unitForTotalBytes(proc.sharedBytes));
            fmt.ioRead =
                (proc.ioReadBytesPerSec > 0.0)
                    ? formatAlignedBytesPerSecString(proc.ioReadBytesPerSec, UI::Format::unitForBytesPerSecond(proc.ioReadBytesPerSec))
                    : "-";
            fmt.ioWrite =
                (proc.ioWriteBytesPerSec > 0.0)
                    ? formatAlignedBytesPerSecString(proc.ioWriteBytesPerSec, UI::Format::unitForBytesPerSecond(proc.ioWriteBytesPerSec))
                    : "-";
            fmt.netSent =
                (proc.netSentBytesPerSec > 0.0)
                    ? formatAlignedBytesPerSecString(proc.netSentBytesPerSec, UI::Format::unitForBytesPerSecond(proc.netSentBytesPerSec))
                    : "-";
            fmt.netRecv = (proc.netReceivedBytesPerSec > 0.0)
                            ? formatAlignedBytesPerSecString(proc.netReceivedBytesPerSec,
                                                             UI::Format::unitForBytesPerSecond(proc.netReceivedBytesPerSec))
                            : "-";
            fmt.power = formatAlignedPowerString(proc.powerWatts);
            fmt.gpuPercent = (proc.gpuUtilPercent > 0.0) ? formatAlignedPercentString(proc.gpuUtilPercent) : "-";
            fmt.gpuMemory = (proc.gpuMemoryBytes > 0) ? formatAlignedBytesString(static_cast<double>(proc.gpuMemoryBytes),
                                                                                 UI::Format::unitForTotalBytes(proc.gpuMemoryBytes))
                                                      : "-";
            fmt.threads = UI::Format::formatOrDash(proc.threadCount, [](auto v) { return UI::Format::formatIntLocalized(v); });
            fmt.handles = UI::Format::formatOrDash(proc.handleCount, [](auto v) { return UI::Format::formatIntLocalized(v); });
            fmt.pageFaults = UI::Format::formatOrDash(proc.pageFaults, [](auto v) { return UI::Format::formatIntLocalized(v); });
            fmt.affinity = UI::Format::formatCpuAffinityMask(proc.cpuAffinityMask);
            fmt.gdiObjects = proc.gdiObjectCount.has_value() ? UI::Format::formatIntLocalized(*proc.gdiObjectCount) : "-";
            m_RowFormatCache.emplace(proc.uniqueKey, std::move(fmt));
        }
        m_RowFormatCacheVersion = m_CachedSnapshotVersion;
    }

    // Search bar
    const auto& theme = UI::Theme::get();
    ImGui::SetNextItemWidth(200.0F);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.scheme().statusRunning);

    // Reserve initial capacity for search buffer if empty
    if (m_SearchBuffer.capacity() == 0)
    {
        m_SearchBuffer.reserve(256);
    }

    // Resize callback for dynamic string growth
    auto resizeCallback = [](ImGuiInputTextCallbackData* data) -> int
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto* str = static_cast<std::string*>(data->UserData);
            str->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = str->data();
        }
        return 0;
    };

    ImGui::InputTextWithHint("##search",
                             "Filter by name...",
                             m_SearchBuffer.data(),
                             m_SearchBuffer.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize,
                             resizeCallback,
                             &m_SearchBuffer);
    ImGui::PopStyleColor();

    // Clear button
    ImGui::SameLine();
    if (!m_SearchBuffer.empty())
    {
        if (ImGui::SmallButton(ICON_FA_XMARK))
        {
            m_SearchBuffer.clear();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Clear filter");
        }
    }

    // Filter snapshots based on search — cached to avoid O(n) rebuild every 60fps frame.
    // Filtered indices, running count, and summary string are only recomputed when snapshot
    // version or search term changes (typically once per second at the 1Hz refresh rate).
    const std::string_view searchTerm(m_SearchBuffer);
    const bool filterDirty = (currentVersion != m_CachedFilterVersion || searchTerm != m_CachedSearchTerm);
    if (filterDirty)
    {
        m_CachedFilteredIndices.clear();
        m_CachedFilteredIndices.reserve(currentSnapshots.size());

        for (size_t i = 0; i < currentSnapshots.size(); ++i)
        {
            if (searchTerm.empty())
            {
                m_CachedFilteredIndices.push_back(i);
            }
            else
            {
                // Case-insensitive search in process name
                const auto& name = currentSnapshots[i].name;
                bool found = false;
                if (name.size() >= searchTerm.size())
                {
                    for (size_t j = 0; j <= name.size() - searchTerm.size(); ++j)
                    {
                        bool match = true;
                        for (size_t k = 0; k < searchTerm.size(); ++k)
                        {
                            if (lowerAscii(name[j + k]) != lowerAscii(searchTerm[k]))
                            {
                                match = false;
                                break;
                            }
                        }
                        if (match)
                        {
                            found = true;
                            break;
                        }
                    }
                }
                if (found)
                {
                    m_CachedFilteredIndices.push_back(i);
                }
            }
        }

        m_CachedRunningCount = 0;
        for (const auto& proc : currentSnapshots)
        {
            if (proc.displayState == "Running")
            {
                ++m_CachedRunningCount;
            }
        }

        if (searchTerm.empty())
        {
            m_CachedSummaryStr = std::format("{:L} processes, {:L} running",
                                             static_cast<long long>(currentSnapshots.size()),
                                             static_cast<long long>(m_CachedRunningCount));
        }
        else
        {
            m_CachedSummaryStr = std::format("{:L} / {:L} processes",
                                             static_cast<long long>(m_CachedFilteredIndices.size()),
                                             static_cast<long long>(currentSnapshots.size()));
        }

        m_CachedFilterVersion = currentVersion;
        m_CachedSearchTerm = std::string(searchTerm);

        // Reset sorted indices to natural order so the next list-view sort starts from scratch.
        // This keeps m_CachedFilteredIndices always in natural order for tree view.
        m_CachedSortedIndices = m_CachedFilteredIndices;
    }

    // Process count with state summary (filtered/total)
    ImGui::SameLine();

    // Get a stable button width based on the widest possible label so layout doesn't shift when toggling
    // Use cached label widths to avoid repeated CalcTextSize calls
    const float maxLabelWidth = std::max(m_TextSizeCache.treeViewLabelWidth, m_TextSizeCache.listViewLabelWidth);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidthPx = maxLabelWidth + (style.FramePadding.x * 2.0F);

    const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float textW = ImGui::CalcTextSize(m_CachedSummaryStr.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - textW - buttonWidthPx - style.ItemSpacing.x));
    ImGui::TextUnformatted(m_CachedSummaryStr.c_str());

    // Tree view toggle button
    ImGui::SameLine();
    if (ImGui::Button(m_TreeViewEnabled ? LIST_VIEW_LABEL.data() : TREE_VIEW_LABEL.data()))
    {
        m_TreeViewEnabled = !m_TreeViewEnabled;
        if (m_TreeViewEnabled)
        {
            // Build tree immediately when enabling tree view. Rebuild if the tree is stale
            // (new data arrived while in list mode) or if it has never been built yet
            // (m_CachedTree is empty on first toggle after startup).
            if (m_TreeNeedsRebuild || m_CachedTree.empty())
            {
                m_CachedTree = buildProcessTree(currentSnapshots);
                m_TreeNeedsRebuild = false;
            }
            m_TreeRebuildDeferred = false; // Clear any pending deferred rebuild
            spdlog::debug("ProcessesPanel: Switched to tree view");
        }
        else
        {
            // Switching to list view: if a deferred rebuild was pending (new data arrived
            // but tree was not rebuilt yet), carry the staleness forward so that switching
            // back to tree view forces a rebuild even if no new snapshot arrives in the
            // interim. Always mark stale here — the rebuild is cheap and ensures correctness.
            m_TreeNeedsRebuild = m_TreeNeedsRebuild || m_TreeRebuildDeferred;
            m_TreeRebuildDeferred = false;
            spdlog::debug("ProcessesPanel: Switched to flat list view");
        }
    }

    // Always create all columns with stable IDs (using enum value as ID)
    // Hidden columns use ImGuiTableColumnFlags_Disabled
    const int totalColumns = UI::Format::checkedCount(processColumnCount());

    if (ImGui::BeginTable("ProcessTable",
                          totalColumns,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row

        // Setup ALL columns with stable IDs - use enum value as user_id for stable identification
        for (const ProcessColumn col : allProcessColumns())
        {
            const auto info = getColumnInfo(col);
            ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_None;

            // Set default visibility from settings (ImGui will manage the actual state)
            if (!m_ColumnSettings.isVisible(col))
            {
                flags |= ImGuiTableColumnFlags_DefaultHide;
            }

            // PID and Name columns cannot be hidden
            if (!info.canHide)
            {
                flags |= ImGuiTableColumnFlags_NoHide;
            }

            // Default sort on CPU%
            if (col == ProcessColumn::CpuPercent)
            {
                flags |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending;
            }

            // Command column stretches, others have initial width (all can be resized/auto-fitted)
            if (info.defaultWidth > 0.0F)
            {
                // Use menuName for TableSetupColumn (shown in context menu)
                // We render custom headers with info.name below
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, info.defaultWidth, toImGuiId(col));
            }
            else
            {
                flags |= ImGuiTableColumnFlags_WidthStretch;
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, 0.0F, toImGuiId(col));
            }
        }

        // Center headers within their columns for better readability
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        int headerIdx = 0;
        const ImGuiStyle& headerStyle = ImGui::GetStyle();
        for (const ProcessColumn col : allProcessColumns())
        {
            if (!ImGui::TableSetColumnIndex(headerIdx))
            {
                ++headerIdx;
                continue;
            }

            const auto info = getColumnInfo(col);
            const float colWidth = ImGui::GetColumnWidth();
            // Use cached column header width
            const float textWidth = m_TextSizeCache.getHeaderWidth(col);
            const float startX = ImGui::GetCursorPosX();
            const float paddingX = headerStyle.CellPadding.x;
            const float targetX = startX + std::max(0.0F, ((colWidth - textWidth) * 0.5F) - paddingX);
            ImGui::SetCursorPosX(targetX);
            // info.name is a constexpr string literal in ProcessColumnConfig.h.
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - literals are null-terminated
            ImGui::TableHeader(info.name.data());

            // Show tooltip with full column name and description on hover
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr string literals are null-terminated
                ImGui::SetTooltip("%s\n%s", info.menuName.data(), info.description.data());
            }

            ++headerIdx;
        }

        // Handle sorting: Disable in tree view mode to maintain parent-child hierarchy
        if (!m_TreeViewEnabled)
        {
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                // Only re-sort when the sort spec changed (SpecsDirty) or when the filtered data
                // changed (filterDirty). Clearing SpecsDirty prevents a redundant O(n log n) sort
                // on every frame when neither the data nor the sort column has changed.
                if (sortSpecs->SpecsCount > 0 && (sortSpecs->SpecsDirty || filterDirty))
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    // Use ColumnUserID to get ProcessColumn (we set user_id = enum value)
                    const std::optional<ProcessColumn> sortColOpt = columnFromUserId(spec.ColumnUserID);
                    if (!sortColOpt.has_value())
                    {
                        sortSpecs->SpecsDirty = true;
                        ImGui::EndTable();
                        return;
                    }

                    const ProcessColumn sortCol = *sortColOpt;

                    // Sort m_CachedSortedIndices (a copy of natural order) so that
                    // m_CachedFilteredIndices remains in PID/natural order for tree view.
                    m_CachedSortedIndices = m_CachedFilteredIndices;
                    std::ranges::sort(m_CachedSortedIndices,
                                      [&currentSnapshots, sortCol, ascending](size_t a, size_t b)
                                      {
                                          const auto& procA = currentSnapshots[a];
                                          const auto& procB = currentSnapshots[b];

                                          auto compare = [ascending](const auto& lhs, const auto& rhs) -> bool
                                          {
                                              return ascending ? (lhs < rhs) : (rhs < lhs);
                                          };

                                          switch (sortCol)
                                          {
                                          case ProcessColumn::PID:
                                              return compare(procA.pid, procB.pid);
                                          case ProcessColumn::User:
                                              return compare(procA.user, procB.user);
                                          case ProcessColumn::CpuPercent:
                                              return compare(procA.cpuPercent, procB.cpuPercent);
                                          case ProcessColumn::MemPercent:
                                              return compare(procA.memoryPercent, procB.memoryPercent);
                                          case ProcessColumn::Virtual:
                                              return compare(procA.virtualBytes, procB.virtualBytes);
                                          case ProcessColumn::Resident:
                                              return compare(procA.memoryBytes, procB.memoryBytes);
                                          case ProcessColumn::PeakResident:
                                              return compare(procA.peakMemoryBytes, procB.peakMemoryBytes);
                                          case ProcessColumn::Shared:
                                              return compare(procA.sharedBytes, procB.sharedBytes);
                                          case ProcessColumn::CpuTime:
                                              return compare(procA.cpuTimeSeconds, procB.cpuTimeSeconds);
                                          case ProcessColumn::StartTime:
                                              return compare(procA.startTimeEpoch, procB.startTimeEpoch);
                                          case ProcessColumn::State:
                                              return compare(procA.displayState, procB.displayState);
                                          case ProcessColumn::Status:
                                              return compare(procA.status, procB.status);
                                          case ProcessColumn::Name:
                                              return compare(procA.name, procB.name);
                                          case ProcessColumn::PPID:
                                              return compare(procA.parentPid, procB.parentPid);
                                          case ProcessColumn::Priority:
                                              return compare(procA.nice, procB.nice);
                                          case ProcessColumn::Threads:
                                              return compare(procA.threadCount, procB.threadCount);
                                          case ProcessColumn::Handles:
                                              return compare(procA.handleCount, procB.handleCount);
                                          case ProcessColumn::PageFaults:
                                              return compare(procA.pageFaults, procB.pageFaults);
                                          case ProcessColumn::Affinity:
                                              return compare(procA.cpuAffinityMask, procB.cpuAffinityMask);
                                          case ProcessColumn::Command:
                                              return compare(procA.command, procB.command);
                                          case ProcessColumn::IoRead:
                                              return compare(procA.ioReadBytesPerSec, procB.ioReadBytesPerSec);
                                          case ProcessColumn::IoWrite:
                                              return compare(procA.ioWriteBytesPerSec, procB.ioWriteBytesPerSec);
                                          case ProcessColumn::NetSent:
                                              return compare(procA.netSentBytesPerSec, procB.netSentBytesPerSec);
                                          case ProcessColumn::NetReceived:
                                              return compare(procA.netReceivedBytesPerSec, procB.netReceivedBytesPerSec);
                                          case ProcessColumn::Power:
                                              return compare(procA.powerWatts, procB.powerWatts);
                                          case ProcessColumn::GpuPercent:
                                              return compare(procA.gpuUtilPercent, procB.gpuUtilPercent);
                                          case ProcessColumn::GpuMemory:
                                              return compare(procA.gpuMemoryBytes, procB.gpuMemoryBytes);
                                          case ProcessColumn::GpuEngine:
                                          {
                                              // Sort by number of engines, then by first engine name
                                              if (procA.gpuEngines.size() != procB.gpuEngines.size())
                                              {
                                                  return compare(procA.gpuEngines.size(), procB.gpuEngines.size());
                                              }
                                              if (!procA.gpuEngines.empty() && !procB.gpuEngines.empty())
                                              {
                                                  return compare(procA.gpuEngines[0], procB.gpuEngines[0]);
                                              }
                                              return false;
                                          }
                                          case ProcessColumn::GpuDevice:
                                              return compare(procA.gpuDevices, procB.gpuDevices);
                                          case ProcessColumn::Publisher:
                                              return compare(procA.publisher, procB.publisher);
                                          case ProcessColumn::Type:
                                              return compare(procA.processType, procB.processType);
                                          case ProcessColumn::GdiObjects:
                                              return compare(procA.gdiObjectCount, procB.gdiObjectCount);
                                          default:
                                              return false;
                                          }
                                      });
                    sortSpecs->SpecsDirty = false;
                }
            }
        } // End of sorting (disabled in tree view mode)

        // Render process rows - tree view or flat list
        if (m_TreeViewEnabled)
        {
            // Render tree view (tree is rebuilt in onUpdate on refresh timer)
            renderTreeView(currentSnapshots, m_CachedFilteredIndices, m_CachedTree);
        }
        else
        {
            // Render flat list with clipper for performance
            // ImGuiListClipper only renders visible rows, skipping off-screen rows
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_CachedSortedIndices.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& proc = currentSnapshots[m_CachedSortedIndices[static_cast<size_t>(i)]];
                    renderProcessRow(proc, 0, false, false);
                }
            }
        }

        // Sync column visibility from ImGui back to our settings
        // This captures changes made via the right-click context menu
        bool settingsChanged = false;
        int idx = 0;
        for (const ProcessColumn col : allProcessColumns())
        {
            const bool isEnabled = (ImGui::TableGetColumnFlags(idx) & ImGuiTableColumnFlags_IsEnabled) != 0;
            if (m_ColumnSettings.isVisible(col) != isEnabled)
            {
                m_ColumnSettings.setVisible(col, isEnabled);
                settingsChanged = true;
            }
            ++idx;
        }
        if (settingsChanged)
        {
            UserConfig::get().settings().processColumns = m_ColumnSettings;
            // Notify listeners that process column settings have changed
            Core::ProcessColumnsChangedEvent evt;
            Core::Application::get().raiseEvent(evt);
        }

        ImGui::EndTable();
    }
}

size_t ProcessesPanel::processCount() const
{
    return m_ProcessModel ? m_ProcessModel->processCount() : 0;
}

bool ProcessesPanel::hasReducedPrivileges() const
{
    return m_ProcessModel && m_ProcessModel->capabilities().hasReducedPrivileges;
}

std::vector<Domain::ProcessSnapshot> ProcessesPanel::snapshots() const
{
    if (m_ProcessModel)
    {
        return m_ProcessModel->snapshots();
    }
    return {};
}

std::optional<Domain::ProcessSnapshot> ProcessesPanel::findSnapshot(std::int32_t pid) const
{
    if (!m_ProcessModel)
    {
        return std::nullopt;
    }
    for (const auto& snap : m_ProcessModel->snapshots())
    {
        if (snap.pid == pid)
        {
            return snap;
        }
    }
    return std::nullopt;
}

std::unordered_map<std::uint64_t, std::vector<std::size_t>>
ProcessesPanel::buildProcessTree(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> tree;

    // First pass: build uniqueKey lookup for parent resolution
    std::unordered_map<std::int32_t, std::uint64_t> pidToUniqueKey;
    for (const auto& proc : snapshots)
    {
        pidToUniqueKey[proc.pid] = proc.uniqueKey;
    }

    // Second pass: build parent uniqueKey -> children mapping
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        const auto& proc = snapshots[i];
        if (proc.parentPid > 0)
        {
            // Find parent's uniqueKey
            auto parentIt = pidToUniqueKey.find(proc.parentPid);
            if (parentIt != pidToUniqueKey.end())
            {
                const std::uint64_t parentKey = parentIt->second;
                tree[parentKey].push_back(i);
            }
        }
    }

    return tree;
}

void ProcessesPanel::renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded)
{
    ImGui::TableNextRow();

    // Look up pre-formatted strings for this row (built at 1Hz, not 60fps)
    static const RowFormatCache s_EmptyRowCache{};
    const auto fmtIt = m_RowFormatCache.find(proc.uniqueKey);
    const RowFormatCache& fmt = (fmtIt != m_RowFormatCache.end()) ? fmtIt->second : s_EmptyRowCache;

    // Render all columns
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (!ImGui::TableSetColumnIndex(colIdx))
        {
            ++colIdx;
            continue; // Column is hidden or clipped
        }
        ++colIdx;

        // PID column with tree indent and expand/collapse indicator
        if (col == ProcessColumn::PID)
        {
            const bool isSelected = (m_SelectedPid == proc.pid);

            // Indent for tree depth
            if (m_TreeViewEnabled && depth > 0)
            {
                const float indentWidth = TREE_INDENT_WIDTH * static_cast<float>(depth);
                ImGui::Indent(indentWidth);
            }

            // Tree expand/collapse button
            if (m_TreeViewEnabled && hasChildren)
            {
                // Stack-allocated button ID: avoids heap allocation per visible row per frame
                std::array<char, 40> buttonIdBuf{};
                const char buttonChar = isExpanded ? '-' : '+';
                auto btnRes = std::format_to_n(buttonIdBuf.data(), buttonIdBuf.size() - 1, "{}##tree_btn_{}", buttonChar, proc.uniqueKey);
                *btnRes.out = '\0';
                if (ImGui::SmallButton(buttonIdBuf.data()))
                {
                    // Toggle collapsed state using uniqueKey
                    if (isExpanded)
                    {
                        m_CollapsedKeys.insert(proc.uniqueKey);
                    }
                    else
                    {
                        m_CollapsedKeys.erase(proc.uniqueKey);
                    }
                }
                ImGui::SameLine();
            }
            else if (m_TreeViewEnabled)
            {
                // Add spacing for processes without children
                ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0F));
                ImGui::SameLine();
            }

            // Stack-allocated label and selectable ID — avoids heap allocations per visible row per frame
            std::array<char, 16> labelBuf{};
            const auto labelResult = std::to_chars(labelBuf.data(), labelBuf.data() + labelBuf.size() - 1, proc.pid);
            *labelResult.ptr = '\0';
            const std::string_view label(labelBuf.data(), static_cast<std::size_t>(labelResult.ptr - labelBuf.data()));

            std::array<char, 40> selectableIdBuf{};
            auto selRes = std::format_to_n(selectableIdBuf.data(), selectableIdBuf.size() - 1, "##pid_select_{}", proc.uniqueKey);
            *selRes.out = '\0';
            if (ImGui::Selectable(
                    selectableIdBuf.data(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                m_SelectedPid = proc.pid;

                // Emit process selection event for other panels to react
                Core::ProcessSelectedEvent event(proc.pid, proc.uniqueKey);
                Core::Application::get().raiseEvent(event);
            }
            ImGui::SameLine(0.0F, 0.0F);
            // Keep PID text right-aligned in its column in both list and tree modes.
            const float pidTextWidth = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
            const float pidAvailWidth = ImGui::GetContentRegionAvail().x;
            const float pidCurrentX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(pidCurrentX + std::max(0.0F, pidAvailWidth - pidTextWidth));
            ImGui::TextUnformatted(label.data(), label.data() + label.size());

            if (m_TreeViewEnabled && depth > 0)
            {
                const float indentWidth = TREE_INDENT_WIDTH * static_cast<float>(depth);
                ImGui::Unindent(indentWidth);
            }
            continue;
        }

        // Render other columns (same as before)
        switch (col)
        {
        case ProcessColumn::User:
            ImGui::TextUnformatted(proc.user.c_str());
            break;

        case ProcessColumn::CpuPercent:
            renderRightAlignedPercentText(fmt.cpuPercent,
                                          m_TextSizeCache.percentWholeWithDotWidths,
                                          m_TextSizeCache.singleDigitWidth,
                                          m_TextSizeCache.unitPercentWidth);
            break;

        case ProcessColumn::MemPercent:
            renderRightAlignedPercentText(fmt.memPercent,
                                          m_TextSizeCache.percentWholeWithDotWidths,
                                          m_TextSizeCache.singleDigitWidth,
                                          m_TextSizeCache.unitPercentWidth);
            break;

        case ProcessColumn::Virtual:
            renderRightAlignedText(fmt.virtualMem);
            break;

        case ProcessColumn::Resident:
            renderRightAlignedText(fmt.resident);
            break;

        case ProcessColumn::PeakResident:
            renderRightAlignedText(fmt.peakRss);
            break;

        case ProcessColumn::Shared:
            renderRightAlignedText(fmt.shared);
            break;

        case ProcessColumn::CpuTime:
            renderRightAlignedText(fmt.cpuTime);
            break;

        case ProcessColumn::StartTime:
            renderRightAlignedText(fmt.startTime);
            break;

        case ProcessColumn::State:
        {
            const char stateChar = proc.displayState.empty() ? '?' : proc.displayState[0];
            const auto& scheme = UI::Theme::get().scheme();

            // Color based on process state
            ImVec4 stateColor;
            switch (stateChar)
            {
            case 'R': // Running
                stateColor = scheme.statusRunning;
                break;
            case 'S': // Sleeping (interruptible)
                stateColor = scheme.statusSleeping;
                break;
            case 'D': // Disk sleep (uninterruptible)
                stateColor = scheme.statusDiskSleep;
                break;
            case 'Z': // Zombie
                stateColor = scheme.statusZombie;
                break;
            case 'T': // Stopped/Traced
            case 't': // Tracing stop
                stateColor = scheme.statusStopped;
                break;
            case 'I': // Idle kernel thread
                stateColor = scheme.statusIdle;
                break;
            default:
                stateColor = scheme.statusSleeping; // Default to muted
                break;
            }

            // Center the state character in the column
            const std::array<char, 2> stateStr = {stateChar, '\0'};
            const float textWidth = ImGui::CalcTextSize(stateStr.data()).x;
            const float availWidth = ImGui::GetContentRegionAvail().x;
            const float offset = std::max(0.0F, (availWidth - textWidth) * 0.5F);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

            ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
            ImGui::TextUnformatted(stateStr.data());
            ImGui::PopStyleColor();
            break;
        }

        case ProcessColumn::Status:
            if (!proc.status.empty())
            {
                ImGui::TextUnformatted(proc.status.c_str());
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            break;

        case ProcessColumn::Name:
            ImGui::TextUnformatted(proc.name.c_str());
            break;

        case ProcessColumn::PPID:
            renderRightAlignedText(fmt.ppid);
            break;

        case ProcessColumn::Priority:
            // getPriorityLabel returns string_view into static storage — no allocation needed
            renderRightAlignedText(Domain::Priority::getPriorityLabel(proc.nice));
            break;

        case ProcessColumn::Threads:
            renderRightAlignedText(fmt.threads);
            break;

        case ProcessColumn::Handles:
            renderRightAlignedText(fmt.handles);
            break;

        case ProcessColumn::PageFaults:
            renderRightAlignedText(fmt.pageFaults);
            break;

        case ProcessColumn::Affinity:
            renderRightAlignedText(fmt.affinity);
            break;

        case ProcessColumn::Command:
            if (!proc.command.empty())
            {
                ImGui::TextUnformatted(proc.command.c_str());
            }
            else
            {
                // Show name in brackets if no command line available
                ImGui::Text("[%s]", proc.name.c_str());
            }
            break;

        case ProcessColumn::IoRead:
            renderRightAlignedText(fmt.ioRead);
            break;

        case ProcessColumn::IoWrite:
            renderRightAlignedText(fmt.ioWrite);
            break;

        case ProcessColumn::NetSent:
            renderRightAlignedText(fmt.netSent);
            break;

        case ProcessColumn::NetReceived:
            renderRightAlignedText(fmt.netRecv);
            break;

        case ProcessColumn::Power:
            renderRightAlignedText(fmt.power);
            break;

        case ProcessColumn::GpuPercent:
            renderRightAlignedPercentText(fmt.gpuPercent,
                                          m_TextSizeCache.percentWholeWithDotWidths,
                                          m_TextSizeCache.singleDigitWidth,
                                          m_TextSizeCache.unitPercentWidth);
            break;

        case ProcessColumn::GpuMemory:
            renderRightAlignedText(fmt.gpuMemory);
            break;

        case ProcessColumn::GpuEngine:
        {
            if (!proc.gpuEngines.empty())
            {
                std::string enginesStr;
                for (size_t i = 0; i < proc.gpuEngines.size(); ++i)
                {
                    if (i > 0)
                    {
                        enginesStr += ", ";
                    }
                    enginesStr += proc.gpuEngines[i];
                }
                ImGui::TextUnformatted(enginesStr.c_str());
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            break;
        }

        case ProcessColumn::GpuDevice:
        {
            if (!proc.gpuDevices.empty())
            {
                ImGui::TextUnformatted(proc.gpuDevices.c_str());
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            break;
        }

        case ProcessColumn::Publisher:
        {
            if (!proc.publisher.empty())
            {
                // Capture available width before rendering so the comparison
                // uses the full cell width rather than the post-render remainder.
                const float availWidth = ImGui::GetContentRegionAvail().x;
                ImGui::TextUnformatted(proc.publisher.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::CalcTextSize(proc.publisher.c_str()).x > availWidth)
                {
                    ImGui::SetTooltip("%s", proc.publisher.c_str());
                }
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            break;
        }

        case ProcessColumn::Type:
        {
            if (!proc.processType.empty())
            {
                const auto& scheme = UI::Theme::get().scheme();
                ImVec4 typeColor;
                if (proc.processType == "App")
                {
                    typeColor = scheme.statusRunning;
                }
                else if (proc.processType == "Windows Process")
                {
                    typeColor = scheme.textInfo;
                }
                else
                {
                    typeColor = scheme.textMuted;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
                ImGui::TextUnformatted(proc.processType.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            break;
        }

        case ProcessColumn::GdiObjects:
            // Show "-" only when the probe could not read the count (process not accessible).
            // A count of 0 is a valid result for non-GUI background processes and is shown as "0".
            renderRightAlignedText(fmt.gdiObjects);
            break;

        default:
            break;
        }
    }
}

void ProcessesPanel::renderProcessTreeNode(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                           const std::unordered_map<std::uint64_t, std::vector<std::size_t>>& tree,
                                           const std::unordered_set<std::size_t>& filteredSet,
                                           std::size_t procIdx,
                                           int depth)
{
    // Iterative tree rendering using explicit stack to avoid recursion
    struct StackFrame
    {
        std::size_t procIdx;
        int depth;
    };

    std::vector<StackFrame> stack;
    stack.reserve(32); // Reserve space for typical tree depth to avoid reallocations
    stack.push_back(StackFrame{.procIdx = procIdx, .depth = depth});

    while (!stack.empty())
    {
        const StackFrame frame = stack.back();
        stack.pop_back();

        // Prevent excessive depth (may indicate cycles or malformed data)
        if (frame.depth >= MAX_TREE_DEPTH)
        {
            spdlog::warn("ProcessesPanel: Maximum tree depth ({}) exceeded, possible cycle or malformed data", MAX_TREE_DEPTH);
            continue;
        }

        const auto& proc = snapshots[frame.procIdx];

        // Check if this process has children (in the filtered set)
        auto childrenIt = tree.find(proc.uniqueKey);
        bool hasChildren = false;
        std::vector<std::size_t> filteredChildren;

        if (childrenIt != tree.end())
        {
            filteredChildren.reserve(childrenIt->second.size());
            // Only count children that are in the filtered set
            for (const std::size_t childIdx : childrenIt->second)
            {
                if (filteredSet.contains(childIdx))
                {
                    filteredChildren.push_back(childIdx);
                }
            }
            hasChildren = !filteredChildren.empty();
        }

        const bool isExpanded = !m_CollapsedKeys.contains(proc.uniqueKey);

        // Render this process
        renderProcessRow(proc, frame.depth, hasChildren, isExpanded);

        // Add children to stack if expanded (in reverse order for correct rendering)
        if (hasChildren && isExpanded)
        {
            for (const auto& childIdx : std::views::reverse(filteredChildren))
            {
                stack.push_back(StackFrame{.procIdx = childIdx, .depth = frame.depth + 1});
            }
        }
    }
}

void ProcessesPanel::renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                    const std::vector<std::size_t>& filteredIndices,
                                    const std::unordered_map<std::uint64_t, std::vector<std::size_t>>& tree)
{
    // Convert filtered indices to a set for O(1) lookups
    const std::unordered_set<std::size_t> filteredSet(filteredIndices.begin(), filteredIndices.end());

    // Build a uniqueKey-to-index map for O(1) parent lookups within the filtered set
    std::unordered_map<std::uint64_t, std::size_t> keyToIndex;
    for (const std::size_t idx : filteredIndices)
    {
        keyToIndex[snapshots[idx].uniqueKey] = idx;
    }

    // Build parent uniqueKey lookup for filtering roots
    std::unordered_set<std::uint64_t> parentKeys;
    for (const std::size_t idx : filteredIndices)
    {
        const auto& proc = snapshots[idx];
        if (proc.parentPid > 0)
        {
            // Try to find parent in filtered snapshots by PID
            for (const auto& [parentKey, parentIdx] : keyToIndex)
            {
                if (snapshots[parentIdx].pid == proc.parentPid)
                {
                    parentKeys.insert(parentKey);
                    break;
                }
            }
        }
    }

    // Render root processes and their descendants (in the order of filteredIndices to respect PID/natural order)
    for (const std::size_t idx : filteredIndices)
    {
        const auto& proc = snapshots[idx];

        // Check if this process's parent is in the filtered set by checking if parent's uniqueKey exists
        bool parentInFilteredSet = false;
        if (proc.parentPid > 0)
        {
            for (const auto& p : snapshots)
            {
                if (p.pid == proc.parentPid && keyToIndex.contains(p.uniqueKey))
                {
                    parentInFilteredSet = true;
                    break;
                }
            }
        }

        // Only render if this is a root process (parent not in filtered set)
        if (!parentInFilteredSet)
        {
            renderProcessTreeNode(snapshots, tree, filteredSet, idx, 0);
        }
    }
}

} // namespace App
