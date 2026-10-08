#include "ProcessesPanel.h"

#include "App/KeyboardInput.h"
#include "App/Panel.h"
#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDisplayFreeze.h"
#include "App/Panels/ProcessFilterCache.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessSortUtils.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTableFlags.h"
#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/ProcessTableNavigation.h"
#include "App/Panels/ProcessTableSettings.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/ProcessColumnConfig.h"
#include "App/SyntheticScenario.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/GPUModel.h"
#include "Domain/Numeric.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"
#include "Domain/SamplingConfig.h"
#include "Platform/CurrentProcess.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ThreadName.h"
#include "UI/EmptyState.h"
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
#include <cmath>
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

// Static UI labels (cached for text size measurements). The view-mode control is two segments, the
// current mode's drawn selected, so it shows the mode it is in rather than the one it switches to (#1209).
constexpr std::string_view LIST_VIEW_LABEL = ICON_FA_LIST " List";
constexpr std::string_view TREE_VIEW_LABEL = ICON_FA_SITEMAP " Tree";
constexpr std::string_view COLUMNS_LABEL = ICON_FA_TABLE_COLUMNS " Columns";
constexpr const char* COLUMNS_POPUP_ID = "##ColumnsMenu";
constexpr const char* ROW_MENU_POPUP_ID = "##ProcessRowMenu";
constexpr const char* FILTER_HINT = ICON_FA_MAGNIFYING_GLASS "  Filter by name...";
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

// How long a row-menu action's result stays in the toolbar, like the Actions block's (#1209).
constexpr float ROW_ACTION_RESULT_SECONDS = 5.0F;

[[nodiscard]] float measureTextWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// One segment of the List | Tree view-mode control (#1209): a button of `width`, drawn in the
/// selection colour while `selected`. Returns true when an unselected segment is pressed.
bool viewModeSegment(std::string_view label, bool selected, float width)
{
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    }
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - the labels are constexpr literals
    const bool pressed = ImGui::Button(label.data(), ImVec2(width, 0.0F));
    if (selected)
    {
        ImGui::PopStyleColor(3);
    }
    return pressed && !selected;
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

/// Column header tooltips wrap at this many ems.
constexpr float HEADER_TOOLTIP_WRAP_EM = 32.0F;

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

/// Draws `text` (already measured as `textWidth`) at the cursor in at most `width`: as is when it
/// fits, otherwise ellipsized with the full text as its tooltip, as a clipped table cell is. For the
/// toolbar's status text, which must not grow past its slot (#1209).
void renderBoundedText(std::string_view text, float textWidth, float width)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }
    const bool clipped = ProcessTableLayout::isCellTextClipped(textWidth, width);
    const float itemWidth = clipped ? std::max(width, 0.0F) : textWidth;
    const char* textBegin = text.data();
    const char* textEnd = text.data() + text.size();
    const ImVec2 textPos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 itemSize(itemWidth, ImGui::GetFontSize());
    const ImRect bounds(textPos, ImVec2(textPos.x + itemSize.x, textPos.y + itemSize.y));
    ImGui::ItemSize(itemSize, 0.0F);
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

/// Draws the cells of a measured zero or a missing value muted (#1210) for as long as it lives;
/// pushes nothing for a measured value.
class MutedCellScope
{
  public:
    explicit MutedCellScope(bool muted) : m_Muted(muted)
    {
        if (m_Muted)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
        }
    }
    ~MutedCellScope()
    {
        if (m_Muted)
        {
            ImGui::PopStyleColor();
        }
    }
    MutedCellScope(const MutedCellScope&) = delete;
    MutedCellScope& operator=(const MutedCellScope&) = delete;
    MutedCellScope(MutedCellScope&&) = delete;
    MutedCellScope& operator=(MutedCellScope&&) = delete;

  private:
    bool m_Muted;
};

/// The tooltip of a cell with no value, saying why (#1210); nothing for a cell that has one.
void setUnavailableReasonTooltip(const char* reason)
{
    if (reason != nullptr)
    {
        ImGui::SetItemTooltip("%s", reason);
    }
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
    {
        const MutedCellScope muted(cell.tone != ProcessRowFormat::CellTone::Value);
        renderRightAlignedText(cell.text, cell.width);
    }
    setUnavailableReasonTooltip(cell.unavailableReason);
}

/// Renders a cell of a mixed-unit column ("512.0 B", "1.5 KiB", "3.2 MiB") decimal-aligned (#1201):
/// the unit in a slot `unitSlotWidth` wide at the cell's right edge, the number right-aligned
/// against it, so the decimal points of every row line up (see
/// ProcessTableLayout::layoutUnitAlignedCell()). A cell with no unit (the unavailable dash) is
/// right-aligned against the slot too, under the numbers; one too narrow for its number and the slot
/// is drawn clipped, as renderRightAlignedText() does. Widths are measured once per cache entry,
/// like renderRightAlignedText()'s.
void renderUnitAlignedText(const AlignedCellText& cell, float unitSlotWidth)
{
    const std::string_view number = cell.number();
    const std::string_view unit = cell.unit();
    if (cell.width < 0.0F)
    {
        cell.width = ImGui::CalcTextSize(cell.text.c_str(), cell.text.c_str() + cell.text.size()).x;
    }
    if (cell.numberWidth < 0.0F)
    {
        cell.numberWidth = cell.hasUnit() ? ImGui::CalcTextSize(number.data(), number.data() + number.size()).x : cell.width;
    }
    const auto layout = ProcessTableLayout::layoutUnitAlignedCell(
        cell.numberWidth, cell.width - cell.numberWidth, unitSlotWidth, ImGui::GetContentRegionAvail().x);
    if (!layout.fits)
    {
        renderRightAlignedText(cell);
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
    {
        const MutedCellScope muted(cell.tone != ProcessRowFormat::CellTone::Value);
        ImGui::RenderText(numberPos, number.data(), number.data() + number.size(), false);
        if (!unit.empty())
        {
            ImGui::RenderText(ImVec2(origin.x + layout.unitX, origin.y), unit.data(), unit.data() + unit.size(), false);
        }
    }
    setUnavailableReasonTooltip(cell.unavailableReason);
}

/// A free-text cell with no value (#1210): the unavailable dash, muted, left-aligned, with `reason`
/// as its tooltip. `dashWidth` is ProcessRowFormat::UNAVAILABLE_CELL_TEXT's width in the current font.
void renderUnavailableTextCell(const char* reason, float dashWidth)
{
    {
        const MutedCellScope muted(true);
        renderCellText(ProcessRowFormat::UNAVAILABLE_CELL_TEXT, dashWidth, /*rightAligned=*/false);
    }
    setUnavailableReasonTooltip(reason);
}

void renderLeftAlignedText(std::string_view text, const ProcessRowFormat::LazyTextWidth& width);

/// A free-text cell as ProcessColumnAvailability::textCell() decided: the text, nothing, or the
/// "not available on this system" dash.
void renderTextCell(ProcessColumnAvailability::TextCell content,
                    std::string_view text,
                    const ProcessRowFormat::LazyTextWidth& width,
                    float dashWidth)
{
    switch (content)
    {
    case ProcessColumnAvailability::TextCell::Text:
        renderLeftAlignedText(text, width);
        break;
    case ProcessColumnAvailability::TextCell::Unavailable:
        renderUnavailableTextCell(ProcessRowFormat::UNSUPPORTED_CELL_REASON, dashWidth);
        break;
    case ProcessColumnAvailability::TextCell::Blank:
        break;
    }
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

/// Free text from the snapshot, left-aligned; blank when the process has none, and the unavailable
/// dash when this system cannot fill the column at all (`Supported`, ProcessColumnAvailability::
/// textCell(), #1210). Blank, not a dash, for an empty value: no status is a process in neither
/// state the column names, no GPU device is one using none, and an empty publisher is either an
/// executable without one or one that could not be read -- the probe cannot tell which.
template<std::string Domain::ProcessSnapshot::* Text,
         ProcessRowFormat::LazyTextWidth RowFormatCache::* Width,
         bool RowFormatCache::* Supported>
void leftAlignedOrBlankCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderTextCell(
        ProcessColumnAvailability::textCell(fmt.*Supported, !(proc.*Text).empty()), proc.*Text, fmt.*Width, widths.unavailableText);
}

/// A GPU text cell (engines, devices): blank for none, the "not available on this system" dash where
/// per-process GPU usage cannot be observed, and the unreadable dash where it can but this
/// generation's read failed or the process started since the last read (#1210).
void renderGpuTextCell(std::string_view text, const ProcessRowFormat::LazyTextWidth& width, const RowFormatCache& fmt, float dashWidth)
{
    if (fmt.gpuSupported && (fmt.gpuUnreadReason != nullptr))
    {
        renderUnavailableTextCell(fmt.gpuUnreadReason, dashWidth);
        return;
    }
    renderTextCell(ProcessColumnAvailability::textCell(fmt.gpuSupported, !text.empty()), text, width, dashWidth);
}

/// The GPU engines in use: RowFormatCache's comma-joined list, left-aligned (renderGpuTextCell()).
void gpuEngineCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderGpuTextCell(fmt.gpuEngines, fmt.gpuEnginesWidth, fmt, widths.unavailableText);
}

/// The GPUs the process uses, left-aligned (renderGpuTextCell()).
void gpuDeviceCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderGpuTextCell(proc.gpuDevices, fmt.gpuDevicesWidth, fmt, widths.unavailableText);
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

/// The priority label, right-aligned: the platform's class name where it has one (Windows:
/// Realtime is not High, #1280), else the nice value's label.
void priorityCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& /*fmt*/, const ProcessCellWidths& widths)
{
    // getProcessPriorityLabel returns string_view into static storage — no allocation
    // needed. Not RowFormatCache-backed (it's a direct lookup, not a per-row formatted
    // string), so its width comes from ProcessCellWidths' small fixed-label width cache
    // instead of an AlignedCellText.
    const std::string_view priorityLabel = Domain::Priority::getProcessPriorityLabel(proc.priorityClass, proc.nice);
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
/// Details, #1180); the unavailable dash where this system cannot classify
/// processes, or could not classify this one (#1210).
void typeCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    if (!fmt.processTypeSupported)
    {
        renderUnavailableTextCell(ProcessRowFormat::UNSUPPORTED_CELL_REASON, widths.unavailableText);
    }
    else if (!proc.processType.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, processTypeColor(proc.processType, UI::Theme::get().scheme()));
        renderLeftAlignedText(proc.processType, fmt.processTypeWidth);
        ImGui::PopStyleColor();
    }
    else
    {
        renderUnavailableTextCell(ProcessRowFormat::UNREADABLE_CELL_REASON, widths.unavailableText);
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
     .render = &leftAlignedOrBlankCell<&Domain::ProcessSnapshot::publisher,
                                       &RowFormatCache::publisherWidth,
                                       &RowFormatCache::publisherSupported>},
    // State
    {.column = ProcessColumn::State, .render = &stateCell},
    {.column = ProcessColumn::Status,
     .render = &leftAlignedOrBlankCell<&Domain::ProcessSnapshot::status, &RowFormatCache::statusWidth, &RowFormatCache::statusSupported>},
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
    // Unavailable only when the probe could not read the count (process not accessible).
    // A count of 0 is a valid result for non-GUI background processes and is
    // shown as a muted "0" (#1210).
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
    {.column = ProcessColumn::GpuDevice, .render = &gpuDeviceCell},
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
    // Tree view's automatic Name width is not the list's: the next launch opens in list view, so save
    // the width Name had before tree view widened it, unless the user has resized it since (#1209).
    if (const ImGuiTable* table = ImGui::TableFindByID(m_TableId); table != nullptr)
    {
        const auto nameIdx = static_cast<int>(toIndex(ProcessColumn::Name));
        if (nameIdx < table->ColumnsCount)
        {
            if (const std::optional<float> listWidth = ProcessTreeIndent::nameWidthToSave(
                    m_TreeViewEnabled, m_NameWidthSetForTree, table->Columns[nameIdx].WidthRequest, m_NameWidthBeforeTree))
            {
                layout =
                    ProcessTableSettings::withColumnWidth(layout, toIndex(ProcessColumn::Name), static_cast<int>(std::lround(*listWidth)));
            }
        }
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
    treeViewLabelWidth = measureTextWidth(TREE_VIEW_LABEL);
    listViewLabelWidth = measureTextWidth(LIST_VIEW_LABEL);
    columnsLabelWidth = measureTextWidth(COLUMNS_LABEL);
    caretRightWidth = measureTextWidth(ICON_FA_CARET_RIGHT);
    caretDownWidth = measureTextWidth(ICON_FA_CARET_DOWN);
    cells.unavailableText = measureTextWidth(ProcessRowFormat::UNAVAILABLE_CELL_TEXT);

    // Cache Domain::Priority::getProcessPriorityLabel()'s fixed label widths
    for (std::size_t i = 0; i < PRIORITY_LABELS.size(); ++i)
    {
        const auto& label = PRIORITY_LABELS[i];
        cells.priorityLabels[i] = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    }
    cells.widestPriorityLabel = std::ranges::max(cells.priorityLabels);
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
    // The Columns button and the two view-mode segments, which sit flush against each other (#1209).
    const float columnsButton = measureTextWidth(COLUMNS_LABEL) + (style.FramePadding.x * 2.0F);
    const float viewModeControl = measureTextWidth(LIST_VIEW_LABEL) + measureTextWidth(TREE_VIEW_LABEL) + (style.FramePadding.x * 4.0F);
    // While Ctrl freezes the pane the paused indicator joins the row (#928). Only its icon-only form
    // is reserved: the full label shows when the real count leaves room, which the worst-case count
    // above nearly always does (ProcessTableLayout::choosePausedLabelForm()).
    const float pausedIcon = ImGui::CalcTextSize(FROZEN_ICON).x;
    const float rest = (style.ItemSpacing.x * 5.0F) + clearButton + pausedIcon + count + columnsButton + viewModeControl;
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
    // destructor's cleanup.
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    this->m_InteractionHoldSeconds = 0.0F;
    m_ProcessModel.reset();
}

void ProcessesPanel::onAttach()
{
    // Load column settings from user config
    m_ColumnSettings = UserConfig::get().settings().processColumns;
    m_ColumnSettings.keepUnhideableColumnsVisible(); // Whatever the settings were given (#1209)

    // m_RefreshInterval starts at the SamplingConfig default, and the model at its built-in history
    // length; ShellLayer raises the configured values as events on its first update (#1079).
    m_AppliedSamplerInterval = m_RefreshInterval;
    this->m_InteractionHoldSeconds = 0.0F;
    m_ForceRefresh = false;

    // Create probe and transfer it to the ProcessModel. The model itself is then
    // passed to BackgroundSampler so enumeration runs off the main thread.
    // The synthetic scenario's probe when TASKSMACK_SYNTHETIC selects one (#1413), else the platform's.
    const Synthetic::Scenario* scenario = Synthetic::activeScenario();
    auto processProbe = Synthetic::makeProcessProbe(scenario);

    const int socketStatsCacheTtlMs = UserConfig::get().settings().socketStatsCacheTtlMs;
    processProbe->setSocketStatsCacheTtl(std::chrono::milliseconds(socketStatsCacheTtlMs));

    m_ProcessModel = std::make_shared<Domain::ProcessModel>(std::move(processProbe));

    // The row menu's actions (#1209), with what this platform can do, so it offers nothing it can't.
    m_ProcessActions = Synthetic::makeProcessActions(scenario);
    m_ActionCapabilities = m_ProcessActions ? m_ProcessActions->actionCapabilities() : Platform::ProcessActionCapabilities{};
    m_OwnPid = Platform::currentProcessId(); // A batch naming TaskSmack itself says so (#804)
    // Config-file only (not in Settings), so applied once here, before the first refresh (#1123).
    m_ProcessModel->setMaxSaneNetworkRate(UserConfig::get().settings().maxSaneRateBps);

    // The synthetic scenario starts with its whole history window filled (#1413), before the seed
    // read below continues it. Nothing happens without a scenario.
    if (scenario != nullptr)
    {
        m_ProcessModel->setMaxHistorySeconds(Synthetic::startupHistorySeconds(scenario, Domain::Sampling::HISTORY_SECONDS_DEFAULT));
        Synthetic::preloadProcessHistory(scenario, *m_ProcessModel, std::chrono::steady_clock::now());
    }

    // Seed with one synchronous read so the first background callback produces valid CPU
    // deltas instead of all-zero percentages (first call establishes the prev-sample
    // baseline; second call - coming from the background thread - computes the delta).
    m_ProcessModel->refresh();

    // Wire sampler: polls the ProcessModel on each interval tick.
    // Seeded synchronously above, so the first background sample waits a full interval (#1102).
    Domain::SamplerConfig const samplerCfg{.interval = m_AppliedSamplerInterval,
                                           .firstSampleAfterInterval = true,
                                           .threadName = std::string(Platform::PROCESS_SAMPLER_THREAD_NAME)};
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(samplerCfg);
    m_Sampler->addSamplable(m_ProcessModel, "processes");
    m_Sampler->start();

    // Ensure the initial seed snapshots are loaded into the render cache so the UI
    // isn't empty before the first background sample arrives.
    adoptNewerSnapshots();

    // A column this system cannot fill is hidden by default, unless its visibility was chosen (#1210).
    // Before the table is first drawn, since its default visibility is set from these settings.
    m_ColumnDefaultsCapabilities = processCapabilities();
    m_GpuSupport = gpuSupport();
    m_ColumnDefaultsGpuSupport = m_GpuSupport;
    ProcessColumnAvailability::applyCapabilityDefaults(m_ColumnSettings, m_ColumnDefaultsCapabilities, m_GpuSupport);
    // Handed to the table on its first frame, as a Columns menu request is: ShellLayer restores the
    // saved ImGui layout after this, and its visibility would otherwise win over DefaultHide.
    m_RequestedColumns = m_ColumnSettings;
    m_TableShowsColumnSettings = false;

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
    if (m_RowActionResultSeconds > 0.0F)
    {
        m_RowActionResultSeconds = std::max(0.0F, m_RowActionResultSeconds - std::max(0.0F, deltaTime));
    }

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

    // Only the sampling interval is throttled during interaction: the GPU merge is a read of the GPU
    // sampler's publication now, cheap enough not to need its own throttle (#1417).
    const bool throttleForInteraction = interactionRedrawActive || (this->m_InteractionHoldSeconds > 0.0F);
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
    if (m_ProcessModel->tryCopySnapshotsIfNewer(
            m_CachedSnapshotVersion, m_CachedRenderSnapshots, newVersion, &m_CachedCapabilities, &m_CachedGpuSupport))
    {
        m_CachedSnapshotVersion = newVersion;
        // Selected processes that have exited drop out of the selection (#804): once per generation,
        // one O(1) lookup per process. The primary process stays, so Process Details can show it exited.
        (void) m_Selection.retainPresent(*m_CachedRenderSnapshots);
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

void ProcessesPanel::renderContent()
{
    // An F9 asked for on a frame the table is not drawn is dropped, never kept for a later one (#170).
    const bool killRequested = m_KillShortcut.take();

    if (!m_ProcessModel)
    {
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Process list unavailable",
                                      "The process model is not initialized, so no processes can be listed.");
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

    // Keyboard navigation (#160), read here in the panel's window: while it is hovered or focused and
    // nothing is typed or open, the navigation keys are taken from ImGui's own keyboard navigation and
    // move the selection instead. Applied inside the table, once the visible order is known.
    auto navCommand = ProcessTableNavigation::NavCommand::None;
    bool selectAllRequested = false;
    if (KeyboardInput::tableNavigationArmed())
    {
        KeyboardInput::claimNavigationKeys(ImGui::GetID("##ProcessTableKeys"), m_TreeViewEnabled);
        navCommand = KeyboardInput::pollNavigationCommand(m_TreeViewEnabled);
        selectAllRequested = KeyboardInput::pollSelectAll(); // Ctrl+A (#804)
    }

    // Get thread-safe copy of snapshots — only when data has actually changed (version-cached).
    // ProcessModel updates at 1Hz but render runs at 60fps; skip 59/60 redundant deep copies.
    // onUpdate() adopts too, but onUpdate() is skipped without a sampler and a generation can be
    // published between the two; with nothing new this is one atomic load. Everything below is keyed
    // on m_CachedSnapshotVersion, the generation adopted with the vector, not on a version read
    // before adopting (#1394).
    adoptNewerSnapshots();
    const std::uint64_t adoptedVersion = m_CachedSnapshotVersion;
    const auto& currentSnapshots = *m_CachedRenderSnapshots;

    // The probe's capabilities can change mid-run (#1254): columns whose visibility was not chosen
    // follow them, through the same request path as the Columns menu (#1210).
    // Per-process GPU support comes from the GPU probe, known only once it has started (#1210).
    m_GpuSupport = gpuSupport();
    if (const Platform::ProcessCapabilities caps = processCapabilities();
        caps != m_ColumnDefaultsCapabilities || m_GpuSupport != m_ColumnDefaultsGpuSupport)
    {
        // The cached cell texts were formatted for the old capabilities (a GPU cell as a measured
        // value or as "not available"), and a cache entry is otherwise rebuilt only for a new process
        // generation; GPU support comes from another model, so it can change between generations.
        m_RowFormatCache.clear();
        m_ColumnDefaultsCapabilities = caps;
        m_ColumnDefaultsGpuSupport = m_GpuSupport;
        if (auto changed =
                ProcessColumnAvailability::capabilityDefaultChanges(m_RequestedColumns.value_or(m_ColumnSettings), caps, m_GpuSupport))
        {
            m_RequestedColumns = *changed;
        }
    }

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
    // The hint in the muted text colour: the running-process green read as a status (#1196).
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.scheme().textMuted);

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

    // Process count with state summary (filtered/total), or for a few seconds the result of an action
    // taken from a row's menu (#1209). Then the Columns button and the List | Tree control.
    ImGui::SameLine();

    // Fixed widths from cached label widths, so the row does not shift when the mode changes.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float columnsButtonWidth = m_TextSizeCache.columnsLabelWidth + (style.FramePadding.x * 2.0F);
    const float listSegmentWidth = m_TextSizeCache.listViewLabelWidth + (style.FramePadding.x * 2.0F);
    const float treeSegmentWidth = m_TextSizeCache.treeViewLabelWidth + (style.FramePadding.x * 2.0F);
    const float controlsWidth = columnsButtonWidth + style.ItemSpacing.x + listSegmentWidth + treeSegmentWidth;

    // An action's result takes no more room than the count text it stands in for, ellipsized with
    // the full text as its tooltip, so a long platform error cannot push the controls off-screen.
    const bool showActionResult = (m_RowActionResultSeconds > 0.0F) && !m_RowActionResult.empty();
    const std::string& statusText = showActionResult ? m_RowActionResult.text : m_CachedSummaryStr;
    const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float summaryW = ImGui::CalcTextSize(m_CachedSummaryStr.c_str(), m_CachedSummaryStr.c_str() + m_CachedSummaryStr.size()).x;
    const float textW = showActionResult ? ImGui::CalcTextSize(statusText.c_str(), statusText.c_str() + statusText.size()).x : summaryW;
    const float statusMinX = ImGui::GetCursorPosX();
    const ProcessTableLayout::ToolbarStatusLayout statusLayout =
        ProcessTableLayout::layoutToolbarStatus(statusMinX, rightEdgeX, controlsWidth, style.ItemSpacing.x, textW, summaryW);
    // While Ctrl freezes the pane (#928) the paused label sits left of the status text, so the text
    // and the controls keep their places. On a row too narrow for the full label it shrinks to its icon.
    auto pausedForm = ProcessTableLayout::PausedLabelForm::Hidden;
    if (m_DisplayFreeze.frozen())
    {
        pausedForm = ProcessTableLayout::choosePausedLabelForm(statusLayout.x - statusMinX,
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
    if (pausedText != nullptr)
    {
        const float pausedW = ImGui::CalcTextSize(pausedText).x + style.ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(statusMinX, statusLayout.x - pausedW));
        ImGui::TextColored(theme.scheme().textWarning, "%s", pausedText);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Updates are paused while Ctrl is held.\nSampling continues; release Ctrl to resume.");
        }
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(statusLayout.x);
    {
        const bool colored = showActionResult;
        if (colored)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, m_RowActionResult.ok ? theme.scheme().textSuccess : theme.scheme().textError);
        }
        renderBoundedText(statusText, textW, statusLayout.width);
        if (colored)
        {
            ImGui::PopStyleColor();
        }
    }

    // Column chooser, beside ImGui's own header right-click menu (#1209)
    ImGui::SameLine();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr literal
    if (ImGui::Button(COLUMNS_LABEL.data(), ImVec2(columnsButtonWidth, 0.0F)))
    {
        ImGui::OpenPopup(COLUMNS_POPUP_ID);
    }
    ImGui::SetItemTooltip("Show, hide or reset columns (also on a column header's right-click menu)");
    // Opens below the button, right edges aligned, rather than at the pointer, where it covered the
    // button that opened it.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing, ImVec2(1.0F, 0.0F));
    if (ImGui::BeginPopup(COLUMNS_POPUP_ID))
    {
        renderColumnsMenu();
        ImGui::EndPopup();
    }

    // View mode: List | Tree, the current one drawn selected (#1209)
    ImGui::SameLine();
    if (viewModeSegment(LIST_VIEW_LABEL, !m_TreeViewEnabled, listSegmentWidth))
    {
        setTreeView(false);
    }
    ImGui::SetItemTooltip("List view (F5): every process, sortable by any column");
    ImGui::SameLine(0.0F, 0.0F);
    if (viewModeSegment(TREE_VIEW_LABEL, m_TreeViewEnabled, treeSegmentWidth))
    {
        setTreeView(true);
    }
    ImGui::SetItemTooltip("Tree view (F5): processes under their parents");

    // The row menu's batch priority dialog (#1484); a picked value goes on to the confirmation below.
    renderBatchPriorityDialog();

    // A row menu's Suspend, Resume, Terminate or Kill, confirmed as in the Actions block (#1209), or a
    // batch priority change (#1484)
    renderRowActionConfirm();

    // The row menu (#1209), for the process m_RowMenuTarget holds. One popup at panel level rather
    // than one per row, opened by ID from the row (m_RowMenuPopupId, this same ID stack).
    m_RowMenuPopupId = ImGui::GetID(ROW_MENU_POPUP_ID);
    if (ImGui::BeginPopup(ROW_MENU_POPUP_ID))
    {
        if (m_RowMenuTarget.has_value())
        {
            renderRowContextMenu(*m_RowMenuTarget);
        }
        ImGui::EndPopup();
    }
    else
    {
        m_RowMenuTarget.reset();
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
                // Priority starts wide enough for its longest label, measured in the current font (#1280).
                const float defaultWidth =
                    (col == ProcessColumn::Priority)
                        ? contentFittedWidth(scaledDefaultWidth(info, emPx), m_TextSizeCache.cells.widestPriorityLabel, emPx)
                        : scaledDefaultWidth(info, emPx);
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, defaultWidth, toImGuiId(col));
            }
            else
            {
                flags |= ImGuiTableColumnFlags_WidthStretch;
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, 0.0F, toImGuiId(col));
            }
        }

        // The toolbar's Columns menu and a view-mode change, while the layout can still change (#1209)
        const bool columnsChangedByMenu = applyColumnRequests();
        syncNameWidthForViewMode();

        // Headers are aligned like their cells: a numeric header over its right-aligned numbers (#1209)
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        int headerIdx = 0;
        const ImGuiStyle& headerStyle = ImGui::GetStyle();
        const Platform::ProcessCapabilities headerCaps = processCapabilities();
        const float sortArrowReserve =
            ProcessTableLayout::sortArrowReserve(ImGui::GetFontSize(), headerStyle.FramePadding.x, headerStyle.CellPadding.x);
        for (const ProcessColumn col : allProcessColumns())
        {
            if (!ImGui::TableSetColumnIndex(headerIdx))
            {
                ++headerIdx;
                continue;
            }

            const auto info = getColumnInfo(col);
            // ImGui draws the sort arrow at the sorted header's right edge; tree view is not sortable.
            const bool showsSortArrow = !m_TreeViewEnabled && (ImGui::TableGetColumnFlags(headerIdx) & ImGuiTableColumnFlags_IsSorted) != 0;
            // The header item keeps the whole cell as its sort and right-click target: it is submitted
            // at the cell's start with a hidden label, and the name is drawn at its aligned position
            // over it. Moving the cursor before TableHeader() moved the item's start too, so an
            // aligned header lost its blank left part as a click target (#1365 review).
            const ImVec2 cellStart = ImGui::GetCursorScreenPos();
            const float cellWidth = ImGui::GetContentRegionAvail().x;
            const float labelOffset = ProcessTableLayout::headerLabelOffset(
                columnAlignment(col), cellWidth, m_TextSizeCache.getHeaderWidth(col), showsSortArrow ? sortArrowReserve : 0.0F);
            // A column this system cannot fill is headed muted, like its cells (#1210).
            const bool columnSupported = ProcessColumnAvailability::isSupported(col, headerCaps, m_GpuSupport);
            {
                const MutedCellScope muted(!columnSupported);
                ImGui::PushID(static_cast<int>(col));
                ImGui::TableHeader("##header");
                ImGui::PopID();
                // TableHeader() draws its label at the cell's cursor position; this draws ours there,
                // shifted by the alignment offset, in the (possibly muted) text colour. Ellipsized,
                // as TableHeader()'s own label is, short of the sort arrow it draws at the right
                // edge, so a narrow sorted column's name never covers its arrow (#1365 review).
                const float labelRight = cellStart.x + std::max(0.0F, cellWidth - (showsSortArrow ? sortArrowReserve : 0.0F));
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
                                          ImVec2(cellStart.x + labelOffset, cellStart.y),
                                          ImVec2(labelRight, cellStart.y + ImGui::GetTextLineHeight()),
                                          labelRight,
                                          info.name.data(),
                                          info.name.data() + info.name.size(),
                                          nullptr);
            }

            // Show tooltip with full column name and description on hover, and what the column's
            // missing values mean (#1210). Every line is a constexpr literal: nothing is formatted.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip())
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * HEADER_TOOLTIP_WRAP_EM);
                const auto line = [](std::string_view text)
                {
                    if (!text.empty())
                    {
                        ImGui::TextUnformatted(text.data(), text.data() + text.size());
                    }
                };
                line(info.menuName);
                line(info.description);
                // The TCP-only note says nothing more for a column that has no values at all.
                if (columnSupported)
                {
                    line(columnCapabilityNote(col, headerCaps.hasUdpNetworkCounters));
                }
                line(ProcessColumnAvailability::unavailableValuesNote(col, headerCaps, m_GpuSupport));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ++headerIdx;
        }

        // Measure this frame's layout for the next frame's inner-width decision. The layout is
        // locked once the header row has been submitted, so these are final for the frame.
        if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr)
        {
            m_TableId = table->ID;
            m_TableHasDefaultOrder = table->IsDefaultDisplayOrder;
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

        // Arrow keys, j/k, Page Up/Down, Home/End, g/G and F9, against the rows as they will be drawn
        applyKeyboardInput(currentSnapshots, navCommand, killRequested, selectAllRequested);

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
            if (m_ScrollSelectedIntoView && m_ScrollTargetRow < m_CachedSortedIndices.size())
            {
                clipper.IncludeItemByIndex(static_cast<int>(m_ScrollTargetRow)); // Drawn, so it can be scrolled to
            }
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
        m_ScrollSelectedIntoView = false; // Done by the selected row if it was drawn; never carried over
        // A row clicked this frame (#804): applied now the rows are drawn, so a Shift+click's range can
        // read the visible order without rebuilding the tree rows under the loop above.
        applyPendingClick(currentSnapshots);

        // Sync column visibility from ImGui back to our settings
        // This captures changes made via the right-click context menu. Not on a frame the Columns
        // menu changed it: ImGui applies that next frame, so its flags still show the old state.
        // Only once the table has been drawn with our settings is a difference the user's own toggle;
        // before that it is ImGui's restored layout, which must not mark a column chosen (#1210).
        if (!columnsChangedByMenu)
        {
            bool settingsChanged = false;
            int idx = 0;
            for (const ProcessColumn col : allProcessColumns())
            {
                const bool isEnabled = (ImGui::TableGetColumnFlags(idx) & ImGuiTableColumnFlags_IsEnabled) != 0;
                settingsChanged = m_ColumnSettings.adoptTableVisibility(col, isEnabled, m_TableShowsColumnSettings) || settingsChanged;
                ++idx;
            }
            if (settingsChanged)
            {
                UserConfig::get().settings().processColumns = m_ColumnSettings;
            }
        }
        m_TableShowsColumnSettings = true;

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

void ProcessesPanel::setGpuModel(const std::shared_ptr<const Domain::GPUModel>& gpuModel)
{
    m_GpuModel = gpuModel;
}

ProcessColumnAvailability::GpuSupport ProcessesPanel::gpuSupport() const
{
    const std::shared_ptr<const Domain::GPUModel> gpuModel = m_GpuModel.lock();
    return ProcessColumnAvailability::gpuSupport(gpuModel != nullptr,
                                                 gpuModel != nullptr && gpuModel->perProcessMetricsKnownUnsupported(),
                                                 gpuModel != nullptr && gpuModel->perProcessUtilizationKnownUnsupported());
}

bool ProcessesPanel::hasPerProcessGpuMetrics() const
{
    return gpuSupport().perProcess;
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
    const RowFormatCache& fmt = ProcessRowFormat::getOrBuildRowFormatCache(
        m_RowFormatCache,
        proc,
        m_CachedSnapshotVersion,
        m_TextSizeCache.stamp,
        ProcessColumnAvailability::rowFormatOptions(caps,
                                                    ProcessColumnAvailability::gpuSupportOfGeneration(m_CachedGpuSupport.perProcess,
                                                                                                      m_CachedGpuSupport.utilization,
                                                                                                      m_CachedGpuSupport.readFailed)));

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

    // By identity -- PID and start time, exactly (#1503) -- not PID alone: after
    // the selected process exits, a new process given its PID is a different row
    // and must not inherit the highlight. Every selected row is highlighted
    // (#804), an O(1) lookup.
    const ProcessSelection::Identity id = ProcessSelection::identityOf(proc);
    const bool isSelected = m_Selection.contains(id);

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

    // The ImGui ID from the full identity too, so two rows can never share one (#1503):
    // "##pid_select_" + an int32 + '_' + a uint64 fits in 64.
    std::array<char, 64> selectableIdBuf{};
    auto selRes = std::format_to_n(selectableIdBuf.data(), selectableIdBuf.size() - 1, "##pid_select_{}_{}", proc.pid, proc.startTimeTicks);
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
        // Plain, Ctrl or Shift click (#804), applied once the rows are drawn (applyPendingClick()).
        const ImGuiIO& io = ImGui::GetIO();
        m_PendingClick = PendingClick{.id = id, .kind = ProcessSelection::clickKindFor(io.KeyCtrl, io.KeyShift)};
    }
    // A keyboard move selected this row (#160): bring it into view below the frozen header.
    if (m_ScrollSelectedIntoView && id == primaryIdentity())
    {
        KeyboardInput::scrollLastItemIntoView();
        m_ScrollSelectedIntoView = false;
    }
    // A right-press selects the row and opens its menu for that same process, in the same
    // frame. ImGui's context-item popup opens on the release instead, by which time the
    // table may have re-sorted and put another process under the pointer (#1365). On a row
    // of a multi-selection the selection is kept, so the menu can act on all of it (#804).
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        if (!isSelected || m_Selection.size() < 2)
        {
            m_Selection.selectOnly(id);
        }
        selectProcess(proc);
        m_RowMenuTarget = proc;
        ImGui::OpenPopup(m_RowMenuPopupId);
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
        // A caret, pointing down when expanded and right when collapsed (#1209), centred in the
        // same slot a leaf's spacer takes, as tall as the text so the row keeps its height.
        // Stack-allocated button ID: avoids heap allocation per visible row per frame
        std::array<char, 40> buttonIdBuf{};
        auto btnRes = std::format_to_n(buttonIdBuf.data(), buttonIdBuf.size() - 1, "##tree_btn_{}", proc.uniqueKey);
        *btnRes.out = '\0';
        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const float slotWidth = ImGui::GetFrameHeight();
        const bool toggled = ImGui::InvisibleButton(buttonIdBuf.data(), ImVec2(slotWidth, ImGui::GetTextLineHeight()));
        const auto& scheme = UI::Theme::get().scheme();
        const float caretWidth = isExpanded ? m_TextSizeCache.caretDownWidth : m_TextSizeCache.caretRightWidth;
        ImGui::GetWindowDrawList()->AddText(ImVec2(slotPos.x + std::floor((slotWidth - caretWidth) * 0.5F), slotPos.y),
                                            ImGui::ColorConvertFloat4ToU32(ImGui::IsItemHovered() ? scheme.textPrimary : scheme.textMuted),
                                            isExpanded ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
        ImGui::SetItemTooltip(isExpanded ? "Collapse" : "Expand");
        if (toggled)
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
    if (m_ScrollSelectedIntoView && m_ScrollTargetRow < rows.size())
    {
        clipper.IncludeItemByIndex(static_cast<int>(m_ScrollTargetRow)); // Drawn, so it can be scrolled to
    }
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const ProcessTreeFlatten::ProcessTreeRow& row = rows[static_cast<std::size_t>(i)];
            renderProcessRow(snapshots[row.procIdx], row.depth, row.hasChildren, row.isExpanded);
        }
    }
}

// ============================================================================
// Discoverable table affordances (#1209)
// ============================================================================

void ProcessesPanel::renderColumnsMenu()
{
    // The menu shows a request ImGui has not applied yet, so a click is reflected at once.
    const ProcessColumnSettings shown = m_RequestedColumns.value_or(m_ColumnSettings);
    const Platform::ProcessCapabilities caps = processCapabilities();

    // Stays open while columns are ticked on and off, as ImGui's header menu does.
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    for (const ProcessColumn col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        const bool visible = shown.isVisible(col);
        ImGui::BeginDisabled(!info.canHide); // PID and Name are always shown
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr literals are null-terminated
        if (ImGui::MenuItem(info.menuName.data(), nullptr, visible))
        {
            ProcessColumnSettings next = shown;
            next.requestVisible(col, !visible);
            m_RequestedColumns = next;
        }
        ImGui::EndDisabled();
        if (!ProcessColumnAvailability::isSupported(col, caps, m_GpuSupport))
        {
            const std::string_view note = ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE;
            ImGui::SetItemTooltip("%.*s", static_cast<int>(note.size()), note.data());
        }
    }
    ImGui::PopItemFlag();

    ImGui::Separator();
    const bool canReset = !ProcessColumnAvailability::hasDefaultColumns(shown, caps, m_GpuSupport) || !m_TableHasDefaultOrder;
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " Reset columns", nullptr, false, canReset))
    {
        // This system's defaults: a column it cannot fill is hidden (#1210)
        m_RequestedColumns = ProcessColumnAvailability::defaultColumns(caps, m_GpuSupport);
        m_ResetColumnOrderRequested = true;
    }
    ImGui::SetItemTooltip("Show the default columns, in their default order");
}

bool ProcessesPanel::applyColumnRequests()
{
    if (m_ResetColumnOrderRequested)
    {
        // ImGui's own "Reset order", applied when the columns are next set up.
        if (ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr)
        {
            table->IsResetDisplayOrderRequest = true;
        }
        m_ResetColumnOrderRequested = false;
    }

    if (!m_RequestedColumns.has_value())
    {
        return false;
    }
    // TableSetColumnEnabled(false) ignores a column's NoHide flag, so PID and Name are kept shown
    // here whatever the request says (#1209).
    m_RequestedColumns->keepUnhideableColumnsVisible();
    int idx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        ImGui::TableSetColumnEnabled(idx, m_RequestedColumns->isVisible(col));
        ++idx;
    }
    m_ColumnSettings = *m_RequestedColumns;
    m_RequestedColumns.reset();
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    return true;
}

void ProcessesPanel::syncNameWidthForViewMode()
{
    if (!m_NameWidthSyncPending)
    {
        return;
    }
    m_NameWidthSyncPending = false;
    const ImGuiTable* table = ImGui::GetCurrentTable();
    if (table == nullptr)
    {
        return;
    }
    const auto nameIdx = static_cast<int>(toIndex(ProcessColumn::Name));
    const float currentWidth = table->Columns[nameIdx].WidthGiven;
    if (m_TreeViewEnabled)
    {
        const float treeWidth = ProcessTreeIndent::treeViewNameWidth(currentWidth, ImGui::GetFontSize());
        m_NameWidthBeforeTree = currentWidth;
        m_NameWidthSetForTree = (treeWidth > currentWidth) ? treeWidth : 0.0F;
        if (m_NameWidthSetForTree > 0.0F)
        {
            ImGui::TableSetColumnWidth(nameIdx, treeWidth);
        }
        return;
    }
    if (ProcessTreeIndent::shouldRestoreNameWidth(m_NameWidthSetForTree, currentWidth))
    {
        ImGui::TableSetColumnWidth(nameIdx, m_NameWidthBeforeTree);
    }
    m_NameWidthSetForTree = 0.0F;
}

void ProcessesPanel::setTreeView(bool enabled)
{
    if (enabled == m_TreeViewEnabled)
    {
        return;
    }
    m_TreeViewEnabled = enabled;
    m_NameWidthSyncPending = true;
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

void ProcessesPanel::collectVisibleRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                        std::vector<std::size_t>& visible,
                                        std::vector<ProcessTableNavigation::TreeRowShape>* shapes)
{
    // The sorted list, or the flattened tree (from the same cache renderTreeView() draws, so this costs
    // no rebuild).
    visible.clear();
    if (shapes != nullptr)
    {
        shapes->clear();
    }
    if (m_TreeViewEnabled)
    {
        const auto& rows = m_TreeRowsCache.rows(
            {
                .snapshotVersion = m_CachedSnapshotVersion,
                .filterGeneration = m_FilterGeneration,
                .collapseGeneration = m_CollapseGeneration,
            },
            snapshots,
            m_CachedFilteredIndices,
            m_CollapsedKeys);
        visible.reserve(rows.size());
        if (shapes != nullptr)
        {
            shapes->reserve(rows.size());
        }
        for (const ProcessTreeFlatten::ProcessTreeRow& row : rows)
        {
            visible.push_back(row.procIdx);
            if (shapes != nullptr)
            {
                shapes->push_back({.depth = row.depth, .hasChildren = row.hasChildren, .isExpanded = row.isExpanded});
            }
        }
    }
    else
    {
        visible = m_CachedSortedIndices;
    }
    // The indices fit the adopted generation; checked anyway, as the row loops do (#1394). A dropped
    // index would misalign the tree shapes, so Left/Right are then left to do nothing.
    std::erase_if(visible, [&snapshots](std::size_t idx) { return idx >= snapshots.size(); });
    if (shapes != nullptr && visible.size() != shapes->size())
    {
        shapes->clear();
    }
}

std::vector<ProcessSelection::Identity> ProcessesPanel::visibleIdentities(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    std::vector<std::size_t> visible;
    collectVisibleRows(snapshots, visible, nullptr);
    std::vector<ProcessSelection::Identity> ids;
    ids.reserve(visible.size());
    for (const std::size_t idx : visible)
    {
        ids.push_back(ProcessSelection::identityOf(snapshots[idx]));
    }
    return ids;
}

void ProcessesPanel::applyPendingClick(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    if (!m_PendingClick.has_value())
    {
        return;
    }
    const PendingClick click = *m_PendingClick;
    m_PendingClick.reset();

    using ProcessSelection::ClickKind;
    const bool isRange = (click.kind == ClickKind::Range || click.kind == ClickKind::AddRange);
    // Only a range reads the visible order: an O(rows) pass on a Shift+click, never on a plain one.
    const std::vector<ProcessSelection::Identity> ids = isRange ? visibleIdentities(snapshots) : std::vector<ProcessSelection::Identity>{};
    m_Selection.click(click.kind, click.id, ids);
    // The clicked row becomes the primary process, unless a Ctrl+click just removed it from the
    // selection: Process Details then keeps showing what it showed.
    if (m_Selection.contains(click.id))
    {
        selectProcess(click.id);
    }
    else if (m_Selection.size() == 1 && !m_Selection.contains(primaryIdentity()))
    {
        // Ctrl+click removed the primary row and left one: that row is the selection now, so it
        // becomes the primary too, or F9 (which acts on the primary) would refuse (#804 review).
        const ProcessSelection::Identity remaining = *m_Selection.selected().begin();
        if (std::ranges::any_of(snapshots,
                                [&remaining](const Domain::ProcessSnapshot& s) { return ProcessSelection::identityOf(s) == remaining; }))
        {
            selectProcess(remaining);
        }
    }
}

void ProcessesPanel::applyKeyboardInput(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                        ProcessTableNavigation::NavCommand command,
                                        bool killRequested,
                                        bool selectAllRequested)
{
    namespace Nav = ProcessTableNavigation;
    if (command == Nav::NavCommand::None && !killRequested && !selectAllRequested)
    {
        return; // The common frame: no key, so the visible order is not even looked at
    }

    // The visible rows in drawn order, as indices into snapshots.
    std::vector<std::size_t> visible;
    std::vector<Nav::TreeRowShape> shapes;
    collectVisibleRows(snapshots, visible, &shapes);

    // Their identities, PID and start time (#1503), parallel to visible.
    std::vector<ProcessSelection::Identity> ids;
    ids.reserve(visible.size());
    for (const std::size_t idx : visible)
    {
        ids.push_back(ProcessSelection::identityOf(snapshots[idx]));
    }

    // Ctrl+A (#804): every row shown -- filtered, and in tree view without collapsed branches.
    if (selectAllRequested)
    {
        m_Selection.selectAll(ids);
    }

    const std::optional<std::size_t> current =
        (m_SelectedPid == -1) ? std::nullopt : Nav::indexOfKey<ProcessSelection::Identity>(ids, primaryIdentity());

    // F9: the row menu's Kill, confirmed in its dialog for the target captured here. Only for a
    // selected row the user can see, only when the platform can kill, and never over a row action
    // already requested or open: a menu action's dialog opens on the next frame, before ImGui knows
    // of it, so the popup stack alone would let F9 replace its action and target (#170).
    // With several rows selected (#804), F9 asks to kill all of them, in the batch confirm; with one,
    // the one highlighted row, when it is visible.
    // A batch priority change (#1484), in its dialog or its confirmation, is a row action too.
    const bool rowActionPending = m_ShowRowActionConfirm || m_RowAction.pending() || m_BatchPriorityDialog.isPending();
    if (killRequested && m_Selection.size() > 1)
    {
        // Only while at least one selected row is visible: with every selected row filtered out, F9
        // must not open a kill for rows the user cannot see (#804 review).
        if (Detail::isActionAvailable(m_ActionCapabilities, Detail::ProcessAction::Kill) && !rowActionPending &&
            m_Selection.anyVisible(ids))
        {
            requestSelectionAction(Detail::ProcessAction::Kill);
        }
    }
    else if (killRequested && m_Selection.size() == 1)
    {
        // The one highlighted row, found by its identity rather than taken to be the primary: an
        // exited primary is kept while the selection moves on, and Ctrl+A over a filter can select one
        // row without making it primary, yet F9 must act on what is highlighted (#804 review). The
        // identity is PID and start time, compared exactly, so only that process's row can match
        // (#1503).
        const ProcessSelection::Identity selectedId = *m_Selection.selected().begin();
        if (const std::optional<std::size_t> row = Nav::indexOfKey<ProcessSelection::Identity>(ids, selectedId); row.has_value())
        {
            const Domain::ProcessSnapshot& proc = snapshots[visible[*row]];
            if (Detail::killShortcutAllowed(m_ActionCapabilities, selectedId, rowActionPending))
            {
                requestRowAction(Detail::ProcessAction::Kill, proc);
            }
        }
    }
    // No movement key this frame (e.g. a bare F9): nothing below may select or scroll, so a refused F9
    // leaves the table exactly as it was.
    if (command == Nav::NavCommand::None)
    {
        return;
    }

    const auto selectRow = [&](std::size_t row)
    {
        // A move selects that row alone, also out of a multi-selection (#804), and anchors a later
        // Shift+click there.
        m_Selection.selectOnly(ids[row]);
        if (row != current)
        {
            selectProcess(snapshots[visible[row]]);
        }
        m_ScrollSelectedIntoView = true;
        m_ScrollTargetRow = row;
    };

    if (command == Nav::NavCommand::Left || command == Nav::NavCommand::Right)
    {
        // With no visible selection this selects the first row, like every other move.
        const Nav::TreeStep step = Nav::treeStepFrom(shapes, current, command);
        switch (step.kind)
        {
        case Nav::TreeStepKind::Collapse:
            // Tree collapse state stays keyed by uniqueKey: it only decides what is drawn, never
            // which process is selected or acted on.
            m_CollapsedKeys.insert(snapshots[visible[step.index]].uniqueKey);
            ++m_CollapseGeneration; // renderTreeView() rebuilds the rows from it this frame (#1138)
            break;
        case Nav::TreeStepKind::Expand:
            m_CollapsedKeys.erase(snapshots[visible[step.index]].uniqueKey);
            ++m_CollapseGeneration;
            break;
        case Nav::TreeStepKind::Select:
            selectRow(step.index);
            break;
        case Nav::TreeStepKind::None:
            break;
        }
        return;
    }

    // A row is a line of text plus the cell padding above and below it.
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const std::size_t page = Nav::pageStep(KeyboardInput::tableScrollViewHeight(), rowHeight);
    if (const std::optional<std::size_t> next = Nav::stepSelection(current, command, visible.size(), page); next.has_value())
    {
        selectRow(*next);
    }
}

void ProcessesPanel::selectProcess(const Domain::ProcessSnapshot& proc)
{
    selectProcess(ProcessSelection::identityOf(proc));
}

void ProcessesPanel::selectProcess(const ProcessSelection::Identity& id)
{
    m_SelectedPid = id.pid;
    m_SelectedStartTicks = id.startTimeTicks;

    // Emit process selection event for other panels to react, with the exact identity (#1503)
    Core::ProcessSelectedEvent event(id.pid, id.startTimeTicks);
    Core::Application::get().raiseEvent(event);
}

void ProcessesPanel::renderRowContextMenu(const Domain::ProcessSnapshot& proc)
{
    // Opened on a row of a multi-selection (#804): its actions are for every selected process; Details
    // and Copy stay with the row it was opened on.
    const std::size_t batchCount =
        (m_Selection.size() > 1 && m_Selection.contains(ProcessSelection::identityOf(proc))) ? m_Selection.size() : 0;

    // Only while the menu is open, so formatting here costs nothing on an ordinary frame.
    if (batchCount > 0)
    {
        ImGui::TextDisabled("%zu processes selected", batchCount);
    }
    ImGui::TextDisabled("%s (PID %d)", proc.name.c_str(), proc.pid);
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO " Details"))
    {
        selectProcess(proc);
        Core::ShowProcessDetailsEvent event;
        Core::Application::get().raiseEvent(event);
    }

    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY " Copy PID"))
    {
        std::array<char, 16> pidText{};
        const auto result = std::to_chars(pidText.data(), pidText.data() + pidText.size() - 1, proc.pid);
        *result.ptr = '\0';
        ImGui::SetClipboardText(pidText.data());
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Name"))
    {
        ImGui::SetClipboardText(proc.name.c_str());
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Command Line", nullptr, false, !proc.command.empty()))
    {
        ImGui::SetClipboardText(proc.command.c_str());
    }

    // "Kill..." for one process, "Kill 5 processes..." for a selection; the "###" part keeps the item's
    // ID the same whichever it says.
    const auto actionItem = [batchCount](Detail::ProcessAction action, const char* icon, const char* shortcut)
    {
        const std::string label =
            (batchCount > 0)
                ? std::format("{} {} {} processes...###{}", icon, Detail::actionLabel(action), batchCount, Detail::actionLabel(action))
                : std::format("{} {}...###{}", icon, Detail::actionLabel(action), Detail::actionLabel(action));
        return ImGui::MenuItem(label.c_str(), shortcut);
    };
    const auto request = [this, &proc, batchCount](Detail::ProcessAction action)
    {
        if (batchCount > 0)
        {
            requestSelectionAction(action);
        }
        else
        {
            requestRowAction(action, proc);
        }
    };

    // Only what this platform can do (ProcessActionCapabilities), as in the Actions block.
    const Platform::ProcessActionCapabilities& can = m_ActionCapabilities;
    if (can.canStop || can.canContinue)
    {
        ImGui::Separator();
        if (can.canStop && actionItem(Detail::ProcessAction::Stop, ICON_FA_PAUSE, nullptr))
        {
            request(Detail::ProcessAction::Stop);
        }
        if (can.canContinue && actionItem(Detail::ProcessAction::Resume, ICON_FA_PLAY, nullptr))
        {
            request(Detail::ProcessAction::Resume);
        }
    }
    // One priority for the whole selection (#1484): picked in its own dialog, then confirmed as a batch.
    // A single process's priority stays in Process Details' Actions block.
    if (ProcessBatch::offersBatchPriority(can, batchCount))
    {
        ImGui::Separator();
        const std::string label = std::format("{} Set priority for {} processes...###SetPriority", ICON_FA_GAUGE_HIGH, batchCount);
        if (ImGui::MenuItem(label.c_str()))
        {
            m_BatchPriorityDialog.open(batchCount);
        }
    }
    if (can.canTerminate || can.canKill)
    {
        // Ending a process can lose its work: in the danger colour, and confirmed in the dialog's
        // danger-coloured button, as in the Actions block (#1273).
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textError);
        if (can.canTerminate && actionItem(Detail::ProcessAction::Terminate, ICON_FA_XMARK, nullptr))
        {
            request(Detail::ProcessAction::Terminate);
        }
        if (can.canKill && actionItem(Detail::ProcessAction::Kill, ICON_FA_SKULL, "F9"))
        {
            request(Detail::ProcessAction::Kill);
        }
        ImGui::PopStyleColor();
    }
}

void ProcessesPanel::requestRowAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc)
{
    m_RowAction.action = action;
    m_RowAction.priorityNice.reset();
    // This row's identity: the platform refuses the action if the PID has since been reused (#973).
    m_RowAction.targets.assign(
        1, ProcessBatch::BatchTarget{.target = {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks}, .name = proc.name});
    m_RowAction.title = Detail::confirmTitle(action, proc.name, proc.pid);
    m_RowAction.question = Detail::confirmBody(action, proc.name, proc.pid);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestSelectionAction(Detail::ProcessAction action)
{
    // The selected processes still listed, each by PID and start time (#973): one that has exited since
    // it was selected is not among them, and one whose PID is reused is refused by the platform.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    if (targets.size() == 1)
    {
        const ProcessBatch::BatchTarget& only = targets.front();
        m_RowAction.title = Detail::confirmTitle(action, only.name, only.target.pid);
        m_RowAction.question = Detail::confirmBody(action, only.name, only.target.pid);
    }
    else
    {
        m_RowAction.title = ProcessBatch::confirmTitle(action, targets.size());
        m_RowAction.question = ProcessBatch::confirmBody(action, targets, m_OwnPid);
    }
    m_RowAction.action = action;
    m_RowAction.priorityNice.reset();
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestSelectionPriority(std::int32_t nice)
{
    // As requestSelectionAction(): the selected processes still listed, each by PID and start time, so
    // one that has exited since the dialog opened is not among them and a reused PID is refused.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    m_RowAction.action = Detail::ProcessAction::None;
    m_RowAction.priorityNice = nice;
    m_RowAction.title = ProcessBatch::priorityConfirmTitle(targets.size());
    m_RowAction.question = ProcessBatch::priorityConfirmBody(nice, targets, m_OwnPid);
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::renderBatchPriorityDialog()
{
    if (const std::optional<std::int32_t> nice = m_BatchPriorityDialog.render(); nice.has_value())
    {
        requestSelectionPriority(*nice);
    }
}

void ProcessesPanel::renderRowActionConfirm()
{
    const bool isPriority = m_RowAction.priorityNice.has_value();
    const ProcessActionConfirm::Outcome outcome =
        isPriority ? ProcessActionConfirm::renderLabelled(
                         m_ShowRowActionConfirm, ProcessBatch::PRIORITY_CONFIRM_LABEL, false, m_RowAction.title, m_RowAction.question)
                   : ProcessActionConfirm::renderText(m_ShowRowActionConfirm, m_RowAction.action, m_RowAction.title, m_RowAction.question);
    if (outcome == ProcessActionConfirm::Outcome::Cancelled)
    {
        m_RowAction = {}; // Nothing is pending any more: F9 may ask again (#170)
        return;
    }
    if (outcome != ProcessActionConfirm::Outcome::Confirmed || m_RowAction.targets.empty())
    {
        return;
    }
    bool anySucceeded = false;
    if (isPriority)
    {
        // Every selected process in turn, by identity, TaskSmack itself last; one summary line naming
        // the priority applied (#1484).
        const std::int32_t nice = *m_RowAction.priorityNice;
        if (m_ProcessActions)
        {
            const ProcessBatch::BatchResult result = ProcessBatch::runBatchPriority(*m_ProcessActions, m_RowAction.targets, nice, m_OwnPid);
            m_RowActionResult = ProcessBatch::formatBatchPriorityResultMessage(nice, result);
            anySucceeded = result.succeeded > 0;
        }
        else
        {
            m_RowActionResult = {.ok = false, .text = "Process actions unavailable"};
        }
    }
    else if (m_RowAction.targets.size() == 1)
    {
        const Platform::ProcessTarget& target = m_RowAction.targets.front().target;
        const Platform::ProcessActionResult result = m_ProcessActions
                                                       ? Detail::dispatchProcessAction(*m_ProcessActions, m_RowAction.action, target)
                                                       : Platform::ProcessActionResult::error("Process actions unavailable");
        m_RowActionResult = Detail::formatActionResultMessage(m_RowAction.action, target.pid, result);
        anySucceeded = result.success;
    }
    else if (m_ProcessActions)
    {
        // Every selected process in turn, each checked by the platform as a single action is, then one
        // summary line rather than a message per process (#804).
        const ProcessBatch::BatchResult result =
            ProcessBatch::runBatchAction(*m_ProcessActions, m_RowAction.action, m_RowAction.targets, m_OwnPid);
        m_RowActionResult = ProcessBatch::formatBatchResultMessage(m_RowAction.action, result);
        anySucceeded = result.succeeded > 0;
    }
    else
    {
        m_RowActionResult = {.ok = false, .text = "Process actions unavailable"};
    }
    m_RowActionResultSeconds = ROW_ACTION_RESULT_SECONDS;
    m_RowAction = {}; // Acted on once; nothing is pending any more
    if (anySucceeded)
    {
        requestRefresh(); // Show the processes suspended, resumed or gone without waiting for the next sample
    }
}

} // namespace App
