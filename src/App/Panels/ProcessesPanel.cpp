#include "ProcessesPanel.h"

#include "App/Panel.h"
#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/ProcessDetailsLayout.h"
#include "App/Panels/ProcessDisplayFreeze.h"
#include "App/Panels/ProcessFilterCache.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessSortUtils.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTableFlags.h"
#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/ProcessTableSettings.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/Numeric.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTable: the laid-out column widths have no public accessor
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/spdlog.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{

namespace
{

// The tree's indent per level and the name width it must leave are font-relative; see
// ProcessTreeIndent::INDENT_PER_LEVEL_EM and MIN_NAME_WIDTH_EM.
//
// The name reservation bounds the tree indent so the expand/collapse button can never be pushed
// out of its cell, which would make a deep parent impossible to toggle (#906). The indent gives
// way first, because depth is also conveyed by the expander column alignment and the PPID column,
// whereas a truncated name has no other source (#906, #913).

constexpr float INTERACTION_INTERVAL_HOLD_SECONDS = 0.40F;

// The units the mixed-unit columns can show, exactly as their cells print them. Each column's unit
// slot is as wide as the widest of its units, measured in the current font (#1201).
constexpr std::array<std::string_view, 3> POWER_UNITS = {" W", " mW", " µW"};

// Static UI labels (cached for text size measurements)
constexpr std::string_view TREE_VIEW_LABEL = "Tree View";
constexpr std::string_view LIST_VIEW_LABEL = "List View";
constexpr const char* FILTER_HINT = "Filter by name...";
// Shown beside the process count while a held Ctrl freezes the pane (#928).
constexpr const char* FROZEN_LABEL = ICON_FA_PAUSE " Paused (Ctrl)";
// The narrow-window form: measureToolbarMinimumWidth() reserves room for this one.
constexpr const char* FROZEN_ICON = ICON_FA_PAUSE;

/// True when any keyboard key other than a modifier is held: Ctrl with one of these is a shortcut
/// (Ctrl+C, Ctrl+=), not the freeze gesture (#928). Only the keyboard block of ImGuiKey is walked;
/// the gamepad and mouse keys follow ImGuiKey_Oem102.
[[nodiscard]] bool anyNonModifierKeyDown()
{
    for (int k = ImGuiKey_Tab; k <= ImGuiKey_Oem102; ++k)
    {
        if (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper)
        {
            continue;
        }
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k)))
        {
            return true;
        }
    }
    return false;
}

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

/// Tooltips for clipped cells wrap at this many ems, so a long command line reads as a paragraph
/// instead of one line running off the screen.
constexpr float CLIPPED_CELL_TOOLTIP_WRAP_EM = 60.0F;

/// Draws `text` in the current table cell, left- or right-aligned, using an already-measured
/// `textWidth`. When the text does not fit it is drawn with an ellipsis and gets a tooltip carrying
/// the full value (#914): a hard-clipped value is indistinguishable from one that is genuinely that
/// short, which is the wrong failure mode for a table whose rows Terminate and Kill act on.
///
/// This submits the item itself rather than calling ImGui::TextUnformatted(), which would measure
/// the text a second time -- ImFontCalcTextSizeEx showed up as a real cost in interactive-frame
/// profiling. The steps mirror ImGui::TextEx()'s common path: same position, same item size, so a
/// cell that fits is laid out exactly as before.
void renderCellText(std::string_view text, float textWidth, bool rightAligned)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }

    const float availWidth = ImGui::GetContentRegionAvail().x;
    const bool clipped = ProcessTableLayout::isCellTextClipped(textWidth, availWidth);

    // A clipped cell fills the space it has and starts at its left edge in either alignment: the
    // leading characters are the ones that identify the value.
    const float itemWidth = clipped ? std::max(availWidth, 0.0F) : textWidth;
    const float offsetX = (rightAligned && !clipped) ? std::max(0.0F, availWidth - textWidth) : 0.0F;

    const char* textBegin = text.data();
    const char* textEnd = text.data() + text.size();
    const ImVec2 textPos(window->DC.CursorPos.x + offsetX, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 itemSize(itemWidth, ImGui::GetFontSize());
    const ImRect bounds(textPos, ImVec2(textPos.x + itemSize.x, textPos.y + itemSize.y));

    // Advance the layout cursor past the alignment offset as well as the text, as SetCursorPosX()
    // followed by a text item did.
    ImGui::ItemSize(ImVec2(offsetX + itemSize.x, itemSize.y), 0.0F);
    if (!ImGui::ItemAdd(bounds, 0))
    {
        return;
    }

    if (!clipped)
    {
        ImGui::RenderText(textPos, textBegin, textEnd, false);
        return;
    }

    const ImVec2 measuredSize(textWidth, itemSize.y);
    ImGui::RenderTextEllipsis(window->DrawList, bounds.Min, bounds.Max, bounds.Max.x, textBegin, textEnd, &measuredSize);

    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * CLIPPED_CELL_TOOLTIP_WRAP_EM);
        ImGui::TextUnformatted(textBegin, textEnd);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Renders `text` right-aligned within the remaining cell width, using an already-measured
/// `textWidth` instead of calling ImGui::CalcTextSize() itself -- callers own measuring (and,
/// for RowFormatCache-backed columns, caching) that width. See the AlignedCellText
/// overload below for the common case.
void renderRightAlignedText(std::string_view text, float textWidth)
{
    renderCellText(text, textWidth, /*rightAligned=*/true);
}

/// Common case: a RowFormatCache-backed cell whose text is built lazily, on demand, only for
/// rows that are actually rendered (see renderProcessRow()'s get-or-build lookup), and whose
/// width is likewise measured lazily -- the first time this specific cell is actually drawn --
/// and cached into `cell.width` (mutable; see AlignedCellText's doc comment for why this must be
/// lazy rather than done for every process at cache-population time). ImFontCalcTextSizeEx and
/// ImGui::ItemSize showed up as real, non-trivial costs in interactive-frame profiling; this
/// keeps that cost paid at most once per visible row per cache generation, instead of every
/// frame or (worse) for every process regardless of visibility.
void renderRightAlignedText(const AlignedCellText& cell)
{
    if (cell.width < 0.0F)
    {
        cell.width = ImGui::CalcTextSize(cell.text.c_str(), cell.text.c_str() + cell.text.size()).x;
    }
    renderRightAlignedText(cell.text, cell.width);
}

/// Renders a cell of a mixed-unit column ("512.0 B", "1.5 KiB", "3.2 MiB") decimal-aligned (#1201):
/// the unit in a slot `unitSlotWidth` wide at the cell's right edge, the number right-aligned
/// against it, so the decimal points of every row line up (see
/// ProcessTableLayout::layoutUnitAlignedCell()). A cell with no unit ("-", "N/A") is right-aligned;
/// one too narrow for its number and the slot is drawn clipped, as renderRightAlignedText() does.
/// Widths are measured once per cache entry, like renderRightAlignedText()'s.
void renderUnitAlignedText(const AlignedCellText& cell, float unitSlotWidth)
{
    if (!cell.hasUnit())
    {
        renderRightAlignedText(cell);
        return;
    }
    const std::string_view number = cell.number();
    const std::string_view unit = cell.unit();
    if (cell.width < 0.0F)
    {
        cell.width = ImGui::CalcTextSize(cell.text.c_str(), cell.text.c_str() + cell.text.size()).x;
    }
    if (cell.numberWidth < 0.0F)
    {
        cell.numberWidth = ImGui::CalcTextSize(number.data(), number.data() + number.size()).x;
    }
    const auto layout = ProcessTableLayout::layoutUnitAlignedCell(
        cell.numberWidth, cell.width - cell.numberWidth, unitSlotWidth, ImGui::GetContentRegionAvail().x);
    if (!layout.fits)
    {
        renderRightAlignedText(cell.text, cell.width);
        return;
    }

    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }
    // The same item placement as renderCellText(): one item from the number's left edge to the
    // unit's right edge, the cursor advanced past the alignment offset as well.
    const ImVec2 origin(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 numberPos(origin.x + layout.numberX, origin.y);
    const ImVec2 itemSize(layout.itemWidth, ImGui::GetFontSize());
    const ImRect bounds(numberPos, ImVec2(numberPos.x + itemSize.x, numberPos.y + itemSize.y));
    ImGui::ItemSize(ImVec2(layout.numberX + itemSize.x, itemSize.y), 0.0F);
    if (!ImGui::ItemAdd(bounds, 0))
    {
        return;
    }
    ImGui::RenderText(numberPos, number.data(), number.data() + number.size(), false);
    ImGui::RenderText(ImVec2(origin.x + layout.unitX, origin.y), unit.data(), unit.data() + unit.size(), false);
}

/// Width of the widest of `units` in the current font.
template<typename Units> [[nodiscard]] float widestTextWidth(const Units& units)
{
    float widest = 0.0F;
    for (const std::string_view unit : units)
    {
        widest = std::max(widest, ImGui::CalcTextSize(unit.data(), unit.data() + unit.size()).x);
    }
    return widest;
}

/// The width of `text` as drawn in the current font, from `width` once it has been measured: measured
/// here the first time the cell is drawn, then reused until its RowFormatCache entry is rebuilt for a
/// new snapshot or font (#1141).
[[nodiscard]] float cachedTextWidth(std::string_view text, const ProcessRowFormat::LazyTextWidth& width)
{
    return width.get([text] { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; });
}

/// Renders free text (a name, a user, a command line) left-aligned in the current cell. `width` is
/// the cell's slot in its row's RowFormatCache entry, so the text is measured once per entry rather
/// than every frame (#1141): command lines run to thousands of characters, each a glyph lookup.
void renderLeftAlignedText(std::string_view text, const ProcessRowFormat::LazyTextWidth& width)
{
    renderCellText(text, cachedTextWidth(text, width), /*rightAligned=*/false);
}

// ============================================================================
// Per-column cell renderers (#1382)
// ============================================================================
//
// Every column but PID and Name is drawn by one small function, looked up by
// column in CELL_RENDERERS below: its format (which RowFormatCache or snapshot
// field), alignment (left, right, centred, or decimal-aligned against a unit
// slot) and N/A handling live in that one entry. PID (the row's selectable) and
// Name (the tree indent and expander) need the panel's selection and tree
// state, so renderProcessRow() draws them itself. A plain function pointer per
// column: no allocation and no std::function per cell, and the lookup is an
// array index.

/// Draws one cell of the current row in the current table column.
using ProcessCellRenderer = void (*)(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths);

/// A RowFormatCache cell right-aligned: a number, a duration, a date, an ID, a
/// percentage, or "-".
template<AlignedCellText RowFormatCache::* Cell>
void rightAlignedCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    renderRightAlignedText(fmt.*Cell);
}

/// A RowFormatCache cell of a mixed-unit column, decimal-aligned against the
/// column's unit slot (#1201).
template<AlignedCellText RowFormatCache::* Cell, float ProcessCellWidths::* UnitSlot>
void unitAlignedCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderUnitAlignedText(fmt.*Cell, widths.*UnitSlot);
}

/// Free text from the snapshot, left-aligned.
template<std::string Domain::ProcessSnapshot::* Text, ProcessRowFormat::LazyTextWidth RowFormatCache::* Width>
void leftAlignedCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    renderLeftAlignedText(proc.*Text, fmt.*Width);
}

/// Free text from the snapshot, left-aligned, or "-" when the snapshot has none
/// (a platform or a process that does not report it).
template<std::string Domain::ProcessSnapshot::* Text, ProcessRowFormat::LazyTextWidth RowFormatCache::* Width>
void leftAlignedOrDashCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    if (!(proc.*Text).empty())
    {
        renderLeftAlignedText(proc.*Text, fmt.*Width);
    }
    else
    {
        ImGui::TextUnformatted("-");
    }
}

/// The GPU engines in use: RowFormatCache's comma-joined list, left-aligned.
void gpuEngineCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    renderLeftAlignedText(fmt.gpuEngines, fmt.gpuEnginesWidth);
}

/// The kernel-style state code, centred, in its state colour.
void stateCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& /*fmt*/, const ProcessCellWidths& /*widths*/)
{
    // The kernel-style code, not the name's first letter: Stopped is T, Dead is X
    // (#1352).
    const char stateChar = Domain::processStateCode(proc.displayState);
    const ImVec4 stateColor = processStateColor(stateChar, UI::Theme::get().scheme());

    // Center the state character in the column
    const std::array<char, 2> stateStr = {stateChar, '\0'};
    const float textWidth = ImGui::CalcTextSize(stateStr.data()).x;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float offset = std::max(0.0F, (availWidth - textWidth) * 0.5F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(stateStr.data());
    ImGui::PopStyleColor();
}

/// The priority label for the nice value, right-aligned.
void priorityCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& /*fmt*/, const ProcessCellWidths& widths)
{
    // getPriorityLabel returns string_view into static storage — no allocation
    // needed. Not RowFormatCache-backed (it's a direct nice-value lookup, not a
    // per-row formatted string), so its width comes from ProcessCellWidths' small
    // fixed-label width cache instead of an AlignedCellText.
    const std::string_view priorityLabel = Domain::Priority::getPriorityLabel(proc.nice);
    renderRightAlignedText(priorityLabel, widths.priorityLabelWidth(priorityLabel));
}

/// The command line, left-aligned, or the name in brackets when there is none
/// (a kernel thread, or a process whose command line could not be read).
void commandCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    if (!proc.command.empty())
    {
        renderLeftAlignedText(proc.command, fmt.commandWidth);
    }
    else
    {
        // Show name in brackets if no command line available
        ImGui::Text("[%s]", proc.name.c_str());
    }
}

/// The process type, left-aligned in its type colour (shared with Process
/// Details, #1180), or "-".
void typeCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    if (!proc.processType.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, processTypeColor(proc.processType, UI::Theme::get().scheme()));
        renderLeftAlignedText(proc.processType, fmt.processTypeWidth);
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextUnformatted("-");
    }
}

/// One CELL_RENDERERS entry: the column it draws (checked against its index
/// below) and its renderer, or nullptr for a column renderProcessRow() draws.
struct ProcessCellEntry
{
    ProcessColumn column;
    ProcessCellRenderer render;
};

/// The renderer for every column but PID and Name, indexed by toIndex(column).
/// Initialised directly, not by a consteval builder, so static analysers
/// (CodeQL cpp/unused-static-function, #1403) see every renderer referenced.
constexpr std::array<ProcessCellEntry, processColumnCount()> CELL_RENDERERS = {{
    {.column = ProcessColumn::PID, .render = nullptr},  // drawn by renderProcessRow() (the selectable)
    {.column = ProcessColumn::Name, .render = nullptr}, // drawn by renderProcessRow() (tree indent and expander)
    // Identity
    {.column = ProcessColumn::User, .render = &leftAlignedCell<&Domain::ProcessSnapshot::user, &RowFormatCache::userWidth>},
    {.column = ProcessColumn::PPID, .render = &rightAlignedCell<&RowFormatCache::ppid>},
    {.column = ProcessColumn::Publisher,
     .render = &leftAlignedOrDashCell<&Domain::ProcessSnapshot::publisher, &RowFormatCache::publisherWidth>},
    // State
    {.column = ProcessColumn::State, .render = &stateCell},
    {.column = ProcessColumn::Status, .render = &leftAlignedOrDashCell<&Domain::ProcessSnapshot::status, &RowFormatCache::statusWidth>},
    {.column = ProcessColumn::Type, .render = &typeCell},
    // Resources
    {.column = ProcessColumn::CpuPercent, .render = &rightAlignedCell<&RowFormatCache::cpuPercent>},
    {.column = ProcessColumn::MemPercent, .render = &rightAlignedCell<&RowFormatCache::memPercent>},
    {.column = ProcessColumn::Resident, .render = &unitAlignedCell<&RowFormatCache::resident, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Virtual, .render = &unitAlignedCell<&RowFormatCache::virtualMem, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Shared, .render = &unitAlignedCell<&RowFormatCache::shared, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::PeakResident, .render = &unitAlignedCell<&RowFormatCache::peakRss, &ProcessCellWidths::unitBytes>},
    // Scheduling
    {.column = ProcessColumn::Priority, .render = &priorityCell},
    {.column = ProcessColumn::Affinity, .render = &rightAlignedCell<&RowFormatCache::affinity>},
    {.column = ProcessColumn::Threads, .render = &rightAlignedCell<&RowFormatCache::threads>},
    {.column = ProcessColumn::Handles, .render = &rightAlignedCell<&RowFormatCache::handles>},
    // "-" only when the probe could not read the count (process not accessible).
    // A count of 0 is a valid result for non-GUI background processes and is
    // shown as "0".
    {.column = ProcessColumn::GdiObjects, .render = &rightAlignedCell<&RowFormatCache::gdiObjects>},
    // Time
    {.column = ProcessColumn::CpuTime, .render = &rightAlignedCell<&RowFormatCache::cpuTime>},
    {.column = ProcessColumn::StartTime, .render = &rightAlignedCell<&RowFormatCache::startTime>},
    // I/O
    {.column = ProcessColumn::IoRead, .render = &unitAlignedCell<&RowFormatCache::ioRead, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::IoWrite, .render = &unitAlignedCell<&RowFormatCache::ioWrite, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::PageFaults, .render = &rightAlignedCell<&RowFormatCache::pageFaults>},
    // Network
    {.column = ProcessColumn::NetSent, .render = &unitAlignedCell<&RowFormatCache::netSent, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::NetReceived, .render = &unitAlignedCell<&RowFormatCache::netRecv, &ProcessCellWidths::unitBytesPerSec>},
    // Power
    {.column = ProcessColumn::Power, .render = &unitAlignedCell<&RowFormatCache::power, &ProcessCellWidths::unitPower>},
    // GPU
    {.column = ProcessColumn::GpuPercent, .render = &rightAlignedCell<&RowFormatCache::gpuPercent>},
    {.column = ProcessColumn::GpuMemory, .render = &unitAlignedCell<&RowFormatCache::gpuMemory, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::GpuEngine, .render = &gpuEngineCell},
    {.column = ProcessColumn::GpuDevice,
     .render = &leftAlignedOrDashCell<&Domain::ProcessSnapshot::gpuDevices, &RowFormatCache::gpuDevicesWidth>},
    // Command
    {.column = ProcessColumn::Command, .render = &commandCell},
}};

// Every entry sits at its column's index, every column but PID and Name has a
// renderer, and those two have none: a column added to ProcessColumn without
// an entry, or an entry out of order, fails to compile here instead of drawing
// the wrong cell or an empty one.
static_assert(std::ranges::all_of(allProcessColumns(),
                                  [](ProcessColumn col)
                                  {
                                      const ProcessCellEntry& entry = CELL_RENDERERS[toIndex(col)];
                                      const bool drawnByRow = (col == ProcessColumn::PID) || (col == ProcessColumn::Name);
                                      return (entry.column == col) && ((entry.render != nullptr) != drawnByRow);
                                  }),
              "CELL_RENDERERS needs one entry per Processes column, in ProcessColumn "
              "order, with a renderer for every column but PID and Name");

} // namespace

// ============================================================================
// Column layout persistence (#952)
// ============================================================================

void ProcessesPanel::restoreTableLayout(std::string_view stored)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    // Filtered before ImGui sees it: the text comes from a user-editable file, and only a single
    // table section may be passed on -- never a window position or a docking layout. The default
    // column order is made explicit, or a layout saved with only a sort line -- by this version or an
    // older one -- would come back with the sorted column first (#1393).
    const std::string layout = ProcessTableSettings::withExplicitOrder(stored);
    if (!layout.empty())
    {
        ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    }
}

std::string ProcessesPanel::captureTableLayout() const
{
    if (m_TableId == 0 || ImGui::GetCurrentContext() == nullptr)
    {
        return {};
    }

    std::size_t iniSize = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
    if (ini == nullptr)
    {
        return {};
    }
    std::string layout = ProcessTableSettings::extractTableSection(std::string_view(ini, iniSize), m_TableId);

    // A section with no sort at all lost it to tree view (see m_SortBackupLayout); put back the
    // sort list view last had. A section that has a sort is newer and is left alone.
    if (!m_SortBackupLayout.empty())
    {
        layout = ProcessTableSettings::carrySortForward(layout, m_SortBackupLayout);
    }
    // Last, so a sort line carried into a section with no order lines gets them too (#1393).
    return ProcessTableSettings::withExplicitOrder(layout);
}

// ============================================================================
// TextSizeCache implementation
// ============================================================================

bool ProcessesPanel::TextSizeCache::isValid() const noexcept
{
    // Cache invalid if not yet populated, if the font has changed, or if the font atlas was rebuilt
    // (which can give the new font the old one's address, #943)
    return fontPtr != nullptr && fontPtr == ImGui::GetFont() && fontGeneration == UI::Theme::get().fontGeneration();
}

void ProcessesPanel::TextSizeCache::populate()
{
    // Store current font pointer and atlas generation for invalidation detection, and a new stamp
    // for the row-format cache
    fontPtr = ImGui::GetFont();
    fontGeneration = UI::Theme::get().fontGeneration();
    ++stamp;

    // Cache column header widths
    for (const ProcessColumn col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        columnHeaderWidths[toIndex(col)] = ImGui::CalcTextSize(info.name.data(), info.name.data() + info.name.size()).x;
    }

    // The unit slot of each mixed-unit column: its widest unit, as the cells print them (#1201)
    std::array<std::string_view, UI::Format::BYTE_UNITS.size()> byteUnits{};
    std::array<std::string_view, UI::Format::BYTE_UNITS.size()> byteRateUnits{};
    for (std::size_t i = 0; i < UI::Format::BYTE_UNITS.size(); ++i)
    {
        byteUnits[i] = UI::Format::cellUnitSuffix(*UI::Format::BYTE_UNITS[i], false);
        byteRateUnits[i] = UI::Format::cellUnitSuffix(*UI::Format::BYTE_UNITS[i], true);
    }
    cells.unitBytes = widestTextWidth(byteUnits);
    cells.unitBytesPerSec = widestTextWidth(byteRateUnits);
    cells.unitPower = widestTextWidth(POWER_UNITS);

    // Cache static label widths
    treeViewLabelWidth = ImGui::CalcTextSize(TREE_VIEW_LABEL.data(), TREE_VIEW_LABEL.data() + TREE_VIEW_LABEL.size()).x;
    listViewLabelWidth = ImGui::CalcTextSize(LIST_VIEW_LABEL.data(), LIST_VIEW_LABEL.data() + LIST_VIEW_LABEL.size()).x;

    // Cache Domain::Priority::getPriorityLabel()'s fixed label widths
    for (std::size_t i = 0; i < PRIORITY_LABELS.size(); ++i)
    {
        const auto& label = PRIORITY_LABELS[i];
        cells.priorityLabels[i] = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    }
}

float ProcessesPanel::measureToolbarMinimumWidth()
{
    // Mirrors render()'s toolbar row; see ProcessTableLayout::computeToolbarMinimumWidth().
    const ImGuiStyle& style = ImGui::GetStyle();
    const float emPx = ImGui::GetFontSize();
    const float hintWidth = ImGui::CalcTextSize(FILTER_HINT).x;
    const float filterForHint = hintWidth + (style.FramePadding.x * 2.0F);
    const float filterWanted = ProcessTableLayout::computeFilterWidth(hintWidth, style.FramePadding.x, emPx, 0.0F);

    // The clear button only shows while filtering, but the row must not overlap when it does. The
    // count is the wider of its two forms at a large, fixed count, so the minimum stays put.
    const float clearButton = ImGui::CalcTextSize(ICON_FA_XMARK).x + (style.FramePadding.x * 2.0F);
    const float count =
        std::max(ImGui::CalcTextSize("99,999 processes, 9,999 running").x, ImGui::CalcTextSize("99,999 / 99,999 processes").x);
    const float toggleButton = std::max(ImGui::CalcTextSize(TREE_VIEW_LABEL.data(), TREE_VIEW_LABEL.data() + TREE_VIEW_LABEL.size()).x,
                                        ImGui::CalcTextSize(LIST_VIEW_LABEL.data(), LIST_VIEW_LABEL.data() + LIST_VIEW_LABEL.size()).x) +
                               (style.FramePadding.x * 2.0F);
    // While Ctrl freezes the pane the paused indicator joins the row (#928). Only its icon-only form
    // is reserved: the full label shows when the real count leaves room, which the worst-case count
    // above nearly always does (ProcessTableLayout::choosePausedLabelForm()).
    const float pausedIcon = ImGui::CalcTextSize(FROZEN_ICON).x;
    const float rest = (style.ItemSpacing.x * 4.0F) + clearButton + pausedIcon + count + toggleButton;
    return ProcessTableLayout::computeToolbarMinimumWidth(filterWanted, filterForHint, rest);
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
    // Order doesn't matter for safety here: BackgroundSampler observes m_ProcessModel via a
    // weak_ptr, so it's never left holding a dangling pointer regardless of which is destroyed
    // first. Stopping the sampler first is still done so no sample() call races the rest of this
    // destructor's cleanup (setInteractionActive below).
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

    // m_RefreshInterval starts at the SamplingConfig default, and the model at its built-in history
    // length; ShellLayer raises the configured values as events on its first update (#1079).
    m_AppliedSamplerInterval = m_RefreshInterval;
    this->m_InteractionHoldSeconds = 0.0F;
    m_ForceRefresh = false;

    // Create probe and transfer it to the ProcessModel. The model itself is then
    // passed to BackgroundSampler so enumeration runs off the main thread.
    auto processProbe = Platform::makeProcessProbe();

    const int socketStatsCacheTtlMs = UserConfig::get().settings().socketStatsCacheTtlMs;
    processProbe->setSocketStatsCacheTtl(std::chrono::milliseconds(socketStatsCacheTtlMs));

    m_ProcessModel = std::make_shared<Domain::ProcessModel>(std::move(processProbe));
    // Config-file only (not in Settings), so applied once here, before the first refresh (#1123).
    m_ProcessModel->setMaxSaneNetworkRate(UserConfig::get().settings().maxSaneRateBps);

    // Seed with one synchronous read so the first background callback produces valid CPU
    // deltas instead of all-zero percentages (first call establishes the prev-sample
    // baseline; second call - coming from the background thread - computes the delta).
    m_ProcessModel->refresh();

    // Wire sampler: polls the ProcessModel on each interval tick.
    // Seeded synchronously above, so the first background sample waits a full interval (#1102).
    Domain::SamplerConfig const samplerCfg{.interval = m_AppliedSamplerInterval, .firstSampleAfterInterval = true};
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(samplerCfg);
    m_Sampler->addSamplable(m_ProcessModel);
    m_Sampler->start();

    // Ensure the initial seed snapshots are loaded into the render cache so the UI
    // isn't empty before the first background sample arrives.
    adoptNewerSnapshots();

    spdlog::info("ProcessesPanel: initialized with background sampler ({}ms interval)", m_AppliedSamplerInterval.count());
}

void ProcessesPanel::setSamplingInterval(std::chrono::milliseconds interval, bool forceSample)
{
    if (interval == m_RefreshInterval)
    {
        return;
    }
    m_RefreshInterval = interval;
    m_AppliedSamplerInterval = interval;
    if (m_Sampler)
    {
        m_Sampler->setInterval(m_AppliedSamplerInterval);
    }
    m_ForceRefresh = m_ForceRefresh || forceSample;
}

void ProcessesPanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void ProcessesPanel::onDetach()
{
    // Save column settings to user config
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    // Order doesn't matter for safety here, same as the destructor above: BackgroundSampler
    // observes m_ProcessModel via a weak_ptr, so it's never left holding a dangling pointer
    // regardless of which is reset first. Stopping the sampler first still avoids a sample()
    // call racing the rest of this teardown.
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
            const bool wasShown = m_ProcessDataShown;
            m_IsActiveTab = (e.tabName() == "Processes");
            m_ProcessDataShown = AdaptiveIntervalUtils::showsProcessData(e.tabName());
            // A freeze belongs to the pane being looked at; leaving it must not leave it frozen (#928).
            m_DisplayFreeze.reset();
            if (!wasShown && m_ProcessDataShown)
            {
                // Catch up straight away when coming back from a tab that showed no process data,
                // where the sampler was relaxed. Between tabs that all show it the sampler ran at
                // the full rate, and an extra sample would land just after the last one (#1102).
                m_ForceRefresh = true;
            }
            return false;
        });
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            // The startup value (#1079) is applied without forcing a sample right after the seed (#1102).
            setSamplingInterval(std::chrono::milliseconds(e.getIntervalMs()), !e.isInitial());
            return false;
        });
    // This panel owns the process model, so it sets the model's history length (#1078).
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            if (m_ProcessModel)
            {
                m_ProcessModel->setMaxHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            }
            return false;
        });
    // A theme or font-size change needs no handling here (#1178): TextSizeCache::isValid() notices a
    // new font (or a rebuilt atlas) on the next frame and remeasures, which also restamps the row
    // format cache, and colours are read from the theme every frame. Neither needs new process data.
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
    const auto desiredInterval =
        AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(m_RefreshInterval, m_ProcessDataShown, throttleForInteraction);
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

    adoptNewerSnapshots();
}

void ProcessesPanel::adoptNewerSnapshots()
{
    // Held Ctrl (#928): keep the adopted generation, so the filter, sort and row caches keyed on it
    // stay as they are. The model keeps sampling; releasing Ctrl adopts its latest generation.
    if (m_DisplayFreeze.frozen() && m_CachedSnapshotVersion != std::numeric_limits<std::uint64_t>::max())
    {
        return;
    }
    // Detect and copy new data in a single lock acquisition. tryCopySnapshotsIfNewer() checks the
    // published version lock-free first, so a call with nothing new costs one atomic load.
    std::uint64_t newVersion = m_CachedSnapshotVersion;
    if (m_ProcessModel->tryCopySnapshotsIfNewer(m_CachedSnapshotVersion, m_CachedRenderSnapshots, newVersion, &m_CachedCapabilities))
    {
        m_CachedSnapshotVersion = newVersion;
    }
}

void ProcessesPanel::updateDisplayFreeze()
{
    const ImGuiIO& io = ImGui::GetIO();
    const ProcessDisplayFreeze::Inputs inputs{
        .appFocused = !io.AppFocusLost,
        .ctrlHeld = io.KeyCtrl,
        .otherModifierHeld = io.KeyShift || io.KeyAlt || io.KeySuper,
        .otherKeyHeld = anyNonModifierKeyDown(),
        .textInputActive = io.WantTextInput,
        .panelHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows),
        .panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows),
    };
    const bool wasFrozen = m_DisplayFreeze.frozen();
    if (m_DisplayFreeze.update(inputs, Core::Application::getTime()) != wasFrozen)
    {
        spdlog::debug("ProcessesPanel: display {} (Ctrl)", wasFrozen ? "resumed" : "frozen");
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

    // Decide the Ctrl freeze before adopting, so a frozen frame keeps the generation it shows (#928).
    updateDisplayFreeze();

    // Get thread-safe copy of snapshots — only when data has actually changed (version-cached).
    // ProcessModel updates at 1Hz but render runs at 60fps; skip 59/60 redundant deep copies.
    // onUpdate() adopts too, but onUpdate() is skipped without a sampler and a generation can be
    // published between the two; with nothing new this is one atomic load. Everything below is keyed
    // on m_CachedSnapshotVersion, the generation adopted with the vector, not on a version read
    // before adopting (#1394).
    adoptNewerSnapshots();
    const std::uint64_t adoptedVersion = m_CachedSnapshotVersion;
    const auto& currentSnapshots = *m_CachedRenderSnapshots;

    // Prune row format cache entries for processes no longer present, once per new snapshot
    // generation (not per frame). Entries themselves are built lazily, on demand, by
    // renderProcessRow() -- see m_RowFormatCache's doc comment -- so this pass only bounds the
    // map's size; it does not rebuild anything. Live keys are collected into a set first so the
    // prune is O(n) overall rather than O(cache size * process count).
    if (m_CachedSnapshotVersion != m_RowFormatCachePrunedVersion)
    {
        std::unordered_set<std::uint64_t> liveKeys;
        liveKeys.reserve(currentSnapshots.size());
        for (const auto& proc : currentSnapshots)
        {
            liveKeys.insert(proc.uniqueKey);
        }
        std::erase_if(m_RowFormatCache, [&liveKeys](const auto& entry) { return !liveKeys.contains(entry.first); });
        m_RowFormatCachePrunedVersion = m_CachedSnapshotVersion;
    }

    // Search bar
    const auto& theme = UI::Theme::get();
    // Sized from the font and the hint it has to show, not a fixed 200px (#965).
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.scheme().statusRunning);

    ImGui::InputTextWithHint("##search", FILTER_HINT, &m_SearchBuffer);
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
    const bool filterDirty = ProcessFilterCache::isStale(adoptedVersion, m_CachedFilterVersion, searchTerm, m_CachedSearchTerm);
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

        m_CachedFilterVersion = adoptedVersion;
        m_CachedSearchTerm = std::string(searchTerm);
        ++m_FilterGeneration; // The tree's rows are rebuilt from the new indices (#1138)

        // Reset sorted indices to natural order so the next list-view sort starts from scratch.
        // This keeps m_CachedFilteredIndices always in natural order for tree view.
        m_CachedSortedIndices = m_CachedFilteredIndices;
        m_SortPending = true; // In tree view the sort below is skipped; leaving it must still sort (#1174)
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
    const float countAndToggleW = textW + style.ItemSpacing.x + buttonWidthPx;
    // The paused label sits left of the count, so the count and the button keep their places. On a
    // row too narrow for the full label it shrinks to its icon (#928).
    auto pausedForm = ProcessTableLayout::PausedLabelForm::Hidden;
    if (m_DisplayFreeze.frozen())
    {
        pausedForm = ProcessTableLayout::choosePausedLabelForm(rightEdgeX - ImGui::GetCursorPosX() - countAndToggleW,
                                                               ImGui::CalcTextSize(FROZEN_LABEL).x + style.ItemSpacing.x,
                                                               ImGui::CalcTextSize(FROZEN_ICON).x + style.ItemSpacing.x);
    }
    const char* pausedText = nullptr;
    if (pausedForm == ProcessTableLayout::PausedLabelForm::Full)
    {
        pausedText = FROZEN_LABEL;
    }
    else if (pausedForm == ProcessTableLayout::PausedLabelForm::IconOnly)
    {
        pausedText = FROZEN_ICON;
    }
    const float pausedW = (pausedText != nullptr) ? ImGui::CalcTextSize(pausedText).x + style.ItemSpacing.x : 0.0F;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - pausedW - countAndToggleW));
    if (pausedText != nullptr)
    {
        ImGui::TextColored(theme.scheme().textWarning, "%s", pausedText);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Updates are paused while Ctrl is held.\nSampling continues; release Ctrl to resume.");
        }
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(m_CachedSummaryStr.c_str());

    // Tree view toggle button
    ImGui::SameLine();
    if (ImGui::Button(m_TreeViewEnabled ? LIST_VIEW_LABEL.data() : TREE_VIEW_LABEL.data()))
    {
        m_TreeViewEnabled = !m_TreeViewEnabled;
        if (m_TreeViewEnabled)
        {
            // Tree view is not sortable, and ImGui leaves the sort out of a table's settings while
            // it is not. Keep the layout as it stands now, in list view, so a layout saved later
            // can still carry the user's sort (#952). Kept after returning to list view too: a
            // resize made in tree view leaves ImGui's stored settings without a sort, and merely
            // becoming sortable again does not rewrite them, so the gap outlasts tree view itself.
            m_SortBackupLayout = captureTableLayout();
            spdlog::debug("ProcessesPanel: Switched to tree view");
        }
        else
        {
            // ImGui need not mark the sort specs dirty on the way back, so force the sort (#1174).
            m_SortPending = true;
            spdlog::debug("ProcessesPanel: Switched to flat list view");
        }
    }

    // Always create all columns with stable IDs (using enum value as ID)
    // Hidden columns use ImGuiTableColumnFlags_Disabled
    const int totalColumns = UI::Format::checkedCount(processColumnCount());

    // Explicit outer_size keeps horizontal/vertical scroll extents aligned with the panel's current content region.
    // This adapts to whichever parent layout is active instead of assuming a fixed child height contract.
    const ImVec2 tableOuterSize = ImGui::GetContentRegionAvail();

    // Sortable only in list view -- tree view ignores sort specs entirely, so offering sortable
    // headers there would accept the click and do nothing (#926).
    // Command absorbs whatever width the other columns leave, but never shrinks below its default;
    // past that point the table scrolls instead. See ProcessTableLayout.h for why this takes an
    // explicit inner width rather than just a stretch column (#924).
    const float emPx = ImGui::GetFontSize();
    // A hidden Command column reserves nothing: its minimum would otherwise be added back on top of
    // a measurement that already excludes it, giving a table that fits a scrollbar and an empty
    // scroll extent it does not need.
    const float commandMinWidth =
        m_ColumnSettings.isVisible(ProcessColumn::Command) ? scaledDefaultWidth(getColumnInfo(ProcessColumn::Command), emPx) : 0.0F;
    const float tableInnerWidth = ProcessTableLayout::computeInnerWidth(m_OtherColumnsWidth, commandMinWidth, m_TableVisibleWidth);

    if (ImGui::BeginTable(
            "ProcessTable", totalColumns, ProcessTableFlags::forProcessTable(m_TreeViewEnabled), tableOuterSize, tableInnerWidth))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row

        // Setup ALL columns with stable IDs - use enum value as user_id for stable identification
        int commandColumnIdx = -1;
        int setupIdx = 0;
        for (const ProcessColumn col : allProcessColumns())
        {
            const auto info = getColumnInfo(col);
            ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_None;
            if (col == ProcessColumn::Command)
            {
                commandColumnIdx = setupIdx;
            }
            ++setupIdx;

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

            // Command is the one stretch column: it takes the width the others leave. Its default
            // width is not an initial width here but the floor enforced through tableInnerWidth.
            //
            // It is also pinned as the trailing column. ImGui makes the right-most enabled column
            // non-resizable whenever the table has a stretch column, which is harmless while that
            // column is Command itself; were Command dragged elsewhere, whichever fixed column ended
            // up last would silently lose its resize handle. NoReorder keeps Command in place and
            // stops other columns crossing over it.
            if (col == ProcessColumn::Command)
            {
                flags |= ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoReorder;
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, 1.0F, toImGuiId(col));
            }
            // Columns with a positive default width are initialized as width-based columns.
            else if (info.defaultWidth > 0.0F)
            {
                // Use menuName for TableSetupColumn (shown in context menu)
                // We render custom headers with info.name below
                // The default is authored at the reference font; scale it to the current one (#913).
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, scaledDefaultWidth(info, emPx), toImGuiId(col));
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
                const std::string_view note = columnCapabilityNote(col, processCapabilities().hasUdpNetworkCounters);
                if (note.empty())
                {
                    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr string literals are null-terminated
                    ImGui::SetTooltip("%s\n%s", info.menuName.data(), info.description.data());
                }
                else
                {
                    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr string literals are null-terminated
                    ImGui::SetTooltip("%s\n%s\n%s", info.menuName.data(), info.description.data(), note.data());
                }
            }

            ++headerIdx;
        }

        // Measure this frame's layout for the next frame's inner-width decision. The layout is
        // locked once the header row has been submitted, so these are final for the frame.
        if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr)
        {
            m_TableId = table->ID;
        }
        if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr && commandColumnIdx >= 0)
        {
            const ImGuiTableColumn& commandColumn = table->Columns[commandColumnIdx];
            const float commandWidth = commandColumn.IsEnabled ? commandColumn.WidthGiven : 0.0F;
            m_OtherColumnsWidth = table->ColumnsGivenWidth - commandWidth;
            m_TableVisibleWidth = table->InnerClipRect.GetWidth();
        }

        // Handle sorting: Disable in tree view mode to maintain parent-child hierarchy
        if (!m_TreeViewEnabled)
        {
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                // Only re-sort when the sort spec changed (SpecsDirty) or when the filtered data
                // changed (filterDirty). Clearing SpecsDirty prevents a redundant O(n log n) sort
                // on every frame when neither the data nor the sort column has changed.
                // m_SortPending covers rows reset to natural order while in tree view (#1174).
                if (sortSpecs->SpecsCount > 0 && (sortSpecs->SpecsDirty || filterDirty || m_SortPending))
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
                    std::ranges::sort(
                        m_CachedSortedIndices,
                        [&currentSnapshots, sortCol, ascending](size_t a, size_t b)
                        { return ProcessSortUtils::compareByColumn(currentSnapshots[a], currentSnapshots[b], sortCol, ascending); });
                    sortSpecs->SpecsDirty = false;
                    m_SortPending = false;
                }
            }
        } // End of sorting (disabled in tree view mode)

        // Render process rows - tree view or flat list
        if (m_TreeViewEnabled)
        {
            // Render tree view (tree is rebuilt in onUpdate on refresh timer)
            renderTreeView(currentSnapshots, m_CachedFilteredIndices);
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
                    // The indices are rebuilt for every adopted generation, so they always fit the
                    // vector; checked anyway, so a keying mistake can't read past its end (#1394).
                    const std::size_t procIdx = m_CachedSortedIndices[static_cast<size_t>(i)];
                    if (procIdx >= currentSnapshots.size())
                    {
                        continue;
                    }
                    renderProcessRow(currentSnapshots[procIdx], 0, false, false);
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
    return processCapabilities().hasReducedPrivileges;
}

Platform::ProcessCapabilities ProcessesPanel::processCapabilities() const
{
    if (!m_ProcessModel)
    {
        return Platform::ProcessCapabilities{};
    }
    // The capabilities published with the cached snapshot generation, copied with it under the same
    // lock (#1254), so a per-frame reader takes no lock. Before the first generation is cached, the
    // model's own (current) copy.
    if (m_CachedSnapshotVersion == std::numeric_limits<std::uint64_t>::max())
    {
        return m_ProcessModel->capabilities();
    }
    return m_CachedCapabilities;
}

std::optional<Domain::ProcessSnapshot> ProcessesPanel::findSnapshot(std::int32_t pid) const
{
    if (!m_ProcessModel)
    {
        return std::nullopt;
    }
    // Delegates to ProcessModel::findSnapshot(), which searches under its own lock and
    // copies only the matching entry, instead of copying the entire snapshot vector
    // just to scan it here. Deliberately does NOT use
    // m_CachedRenderSnapshots: that cache is only refreshed while this panel's tab is
    // active (see renderContent()), so it would go stale while e.g. the Process Details
    // tab is showing — this must always reflect the latest published snapshot.
    return m_ProcessModel->findSnapshot(pid);
}

void ProcessesPanel::watchProcess(std::int32_t pid)
{
    if (m_ProcessModel)
    {
        m_ProcessModel->watchProcess(pid);
    }
}

bool ProcessesPanel::watchedSamplesSince(std::uint64_t lastSeenVersion, std::vector<Domain::ProcessSample>& outSamples) const
{
    return m_ProcessModel && m_ProcessModel->watchedSamplesSince(lastSeenVersion, outSamples);
}

void ProcessesPanel::renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded)
{
    ImGui::TableNextRow();

    // Get-or-build the pre-formatted strings for this row. Built lazily here --
    // the first time this specific row is actually rendered after its snapshot
    // data or the current font changes -- rather than eagerly for every process
    // whenever the snapshot version advances, so cost scales with visible rows
    // (bounded by ImGuiListClipper), not total process count (perf-plan #843). A
    // font/size/DPI change alone, with no new data version, must still force a
    // rebuild of this entry, or its cached AlignedCellText widths (measured for
    // the old font) would stay wrong until the next ~1Hz data refresh happens to
    // land. The get-or-build decision itself lives in ProcessRowFormat.h
    // (ImGui-free) so it's directly unit-testable. The font is passed as an
    // identity value, not a pointer: RowFormatCache only ever compares this stamp
    // for equality, so it stores a std::uintptr_t and no address escapes into the
    // long-lived cache map (see #904). The stamp changes on every
    // TextSizeCache::populate(), not with the font's address, which a rebuilt
    // font atlas can reuse (#943).
    const Platform::ProcessCapabilities caps = processCapabilities();
    const RowFormatCache& fmt = ProcessRowFormat::getOrBuildRowFormatCache(m_RowFormatCache,
                                                                           proc,
                                                                           m_CachedSnapshotVersion,
                                                                           m_TextSizeCache.stamp,
                                                                           {
                                                                               .hasPowerUsage = caps.hasPowerUsage,
                                                                               .hasSharedMemory = caps.hasSharedMemory,
                                                                           });

    // Render all columns
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool columnVisible = ImGui::TableSetColumnIndex(colIdx);
        ++colIdx;

        // The PID column anchors the row's selectable, so it is entered even when
        // it is not visible. TableSetColumnIndex() returns false for a column
        // scrolled out of view as well as for a hidden one, and skipping PID on
        // that basis skipped the only item that makes the row clickable: with the
        // table scrolled right, rows could not be selected and the selected row
        // lost its highlight (#962). The selectable spans all columns and is drawn
        // and hit-tested against the whole table, not this cell, so it works from a
        // clipped column; PID cannot be hidden (canHide is false), so it is never a
        // disabled one.
        if (col == ProcessColumn::PID)
        {
            renderPidCell(proc, fmt, columnVisible);
            continue;
        }
        if (!columnVisible)
        {
            continue; // Column is hidden or clipped
        }

        // Name carries the tree indent and expander, so it is drawn with the
        // panel's tree state; every other column is a small function looked up by
        // column (#1382).
        if (col == ProcessColumn::Name)
        {
            renderNameCell(proc, fmt, depth, hasChildren, isExpanded);
            continue;
        }
        CELL_RENDERERS[toIndex(col)].render(proc, fmt, m_TextSizeCache.cells);
    }
}

void ProcessesPanel::renderPidCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, bool columnVisible)
{
    // The PID column: the row's selectable anchor and its right-aligned PID text.
    // The tree indent and expand/collapse control deliberately do NOT live here
    // -- see the comment below and #906.

    // By identity, not PID alone: after the selected process exits, a new process
    // given its PID is a different row and must not inherit the highlight.
    const bool isSelected = ProcessDetailsLayout::snapshotIsSelectedProcess(m_SelectedPid, m_SelectedUniqueKey, proc.pid, proc.uniqueKey);

    // The tree indent and expand/collapse button live in the Name column, not
    // here. This column is a fixed 60px, so indenting it pushed the PID text past
    // the cell's clip rect and silently truncated digits at depth >= 1 -- 589
    // rendered as "5" (see #906). Name is fixed-width too (120px), so it cannot
    // absorb the indent for free -- the indent is clamped against the cell
    // instead, see renderNameCell(). Indenting the name is what comparable
    // process viewers do.

    // Stack-allocated label and selectable ID — avoids heap allocations per
    // visible row per frame
    std::array<char, 16> labelBuf{};
    const auto labelResult = std::to_chars(labelBuf.data(), labelBuf.data() + labelBuf.size() - 1, proc.pid);
    *labelResult.ptr = '\0';
    const std::string_view label(labelBuf.data(), static_cast<std::size_t>(labelResult.ptr - labelBuf.data()));

    std::array<char, 40> selectableIdBuf{};
    auto selRes = std::format_to_n(selectableIdBuf.data(), selectableIdBuf.size() - 1, "##pid_select_{}", proc.uniqueKey);
    *selRes.out = '\0';
    // ImGui fills a hovered row with HeaderHovered even when it is selected, so
    // the row just clicked would show the weaker hover tint until the pointer
    // left it. Keep the selected fill while hovered (#1190).
    if (isSelected)
    {
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    }
    const bool clicked =
        ImGui::Selectable(selectableIdBuf.data(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
    if (isSelected)
    {
        ImGui::PopStyleColor();
    }
    if (clicked)
    {
        m_SelectedPid = proc.pid;
        m_SelectedUniqueKey = proc.uniqueKey;

        // Emit process selection event for other panels to react
        Core::ProcessSelectedEvent event(proc.pid, proc.uniqueKey);
        Core::Application::get().raiseEvent(event);
    }
    if (!columnVisible)
    {
        return; // Scrolled out of view: the row stays selectable, the PID text is
                // not drawn
    }
    ImGui::SameLine(0.0F, 0.0F);
    // Keep PID text right-aligned in its column in both list and tree modes.
    renderRightAlignedText(label, cachedTextWidth(label, fmt.pidWidth));
}

void ProcessesPanel::renderNameCell(
    const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, int depth, bool hasChildren, bool isExpanded)
{
    // Tree depth is expressed here rather than in the PID column, which is a
    // fixed 60px and truncated PIDs once the indent was inside it (#906).
    //
    // Name is fixed-width too (120px by default), so the indent has to come out
    // of the name's own room; it cannot be granted extra width per frame.
    // TableSetupColumn()'s init_width_or_weight is applied to a *resizable*
    // column only while the table is initializing (imgui_tables.cpp:989 gates it
    // on !column_is_resizable), so passing a depth-dependent width on later
    // frames is silently ignored.
    //
    // So the indent yields instead: it is clamped against the cell's actual
    // width, always reserving the expander slot plus MIN_NAME_WIDTH_EM of name.
    // Deep rows therefore show less indentation than their depth would suggest
    // rather than losing the name or, worse, the expander -- a parent whose
    // expander is pushed out of the cell cannot be collapsed back to a usable
    // width, which is a dead end rather than a cosmetic clip. Restoring full
    // indent fidelity needs a wider default Name column, tracked in #913. The
    // expander slot is the control itself plus the ItemSpacing.x that SameLine()
    // adds after it -- both the button and the leaf Dummy are followed by
    // SameLine(), so the spacing is always paid and must be reserved or the name
    // keeps less room than promised.
    const float expanderSlotWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float treeEmPx = ImGui::GetFontSize();
    const float reservedForControls = expanderSlotWidth + (ProcessTreeIndent::MIN_NAME_WIDTH_EM * treeEmPx);
    const float indentWidth =
        m_TreeViewEnabled
            ? ProcessTreeIndent::clampedIndent(
                  depth, ProcessTreeIndent::INDENT_PER_LEVEL_EM * treeEmPx, ImGui::GetContentRegionAvail().x, reservedForControls)
            : 0.0F;
    const bool indented = indentWidth > 0.0F;
    if (indented)
    {
        ImGui::Indent(indentWidth);
    }

    if (m_TreeViewEnabled && hasChildren)
    {
        // Stack-allocated button ID: avoids heap allocation per visible row per
        // frame
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
            ++m_CollapseGeneration; // The tree's rows are rebuilt next frame (#1138)
        }
        ImGui::SameLine();
    }
    else if (m_TreeViewEnabled)
    {
        // Keep names aligned with their siblings that do have an expander.
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0F));
        ImGui::SameLine();
    }

    renderLeftAlignedText(proc.name, fmt.nameWidth);

    if (indented)
    {
        ImGui::Unindent(indentWidth);
    }
}

void ProcessesPanel::renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& filteredIndices)
{
    // The expanded, filtered tree flattened into render order (roots in the order of filteredIndices,
    // to respect PID/natural order), so ImGuiListClipper below can bound the expensive part --
    // renderProcessRow(), which measures/renders every column -- to visible rows only. The rows are
    // rebuilt only when a new snapshot is adopted, the filter result is rebuilt, or a node is
    // collapsed or expanded, not every frame (#1138); see ProcessTreeFlatten::ProcessTreeRowsCache.
    // Held by reference while rendering: an expander toggled below only advances
    // m_CollapseGeneration, so the rows are rebuilt next frame, never during this loop.
    const std::vector<ProcessTreeFlatten::ProcessTreeRow>& rows = m_TreeRowsCache.rows(
        {
            .snapshotVersion = m_CachedSnapshotVersion,
            .filterGeneration = m_FilterGeneration,
            .collapseGeneration = m_CollapseGeneration,
        },
        snapshots,
        filteredIndices,
        m_CollapsedKeys);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const ProcessTreeFlatten::ProcessTreeRow& row = rows[static_cast<std::size_t>(i)];
            renderProcessRow(snapshots[row.procIdx], row.depth, row.hasChildren, row.isExpanded);
        }
    }
}

} // namespace App
