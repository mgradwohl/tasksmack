#pragma once

// Scrolling region for the body of one tab item, so the tab bar above it stays put.
//
// The system and process-details panels drew their sub-tab bar and the selected tab's content
// straight into the shell's single scrolling content area. Scrolling a tall tab therefore scrolled
// its own tab bar out of view, and changing sub-tab meant scrolling back to the top first (#968).
// Giving each tab's body its own child window moves the scrolling below the tab bar: the bar is
// laid out in the non-scrolling parent, and only the body scrolls.

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: a window's recorded content extent has no public accessor
// clang-format on

#include <algorithm>

namespace UI::Widgets
{

/// RAII scope for a tab item's body. Construct it right after ImGui::BeginTabItem() succeeds and
/// let it go out of scope before ImGui::EndTabItem().
///
/// The child is laid out so the tab looks exactly as it did when the parent did the scrolling:
///
///   - It spans the parent's full width, not just its content region, and carries the parent's
///     horizontal padding as its own. The content therefore sits where it always did, and the
///     scrollbar stays at the window's edge. A child confined to the content region would put the
///     scrollbar a gutter's width inside the window with the content hard against it.
///   - It draws no background of its own: it sits inside the shell's content area, which has
///     already painted one, and a second, translucent, theme-defined ChildBg on top would darken
///     every tab.
class TabContentScope
{
  public:
    explicit TabContentScope(const char* id)
    {
        const ImVec2 parentPos = ImGui::GetWindowPos();
        const ImVec2 parentSize = ImGui::GetWindowSize();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();

        const ImGuiWindow* parent = ImGui::GetCurrentWindow();
        m_ParentContentMaxX = parent->DC.CursorMaxPos.x;
        m_ParentIdealMaxX = parent->DC.IdealMaxPos.x;

        // The parent's horizontal padding, measured rather than read from the style: the cursor is
        // at the start of a line, so its offset from the window edge is the padding the parent was
        // actually begun with, whatever the style says by now.
        const float gutter = cursor.x - parentPos.x;

        // A child is clipped by its parent's clip rectangle as it stands when the child begins,
        // and that rectangle is inset by half the parent's padding -- which would shave the outer
        // edge off a scrollbar sitting in the gutter. Widen it to the parent's own bounds for the
        // lifetime of the child.
        ImGui::PushClipRect(parentPos, ImVec2(parentPos.x + parentSize.x, parentPos.y + parentSize.y), false);
        ImGui::SetCursorScreenPos(ImVec2(parentPos.x, cursor.y));

        // Both style changes are popped again before any content is submitted, so nested children
        // (chart grid cells, info blocks) still get the theme's ChildBg and the style's padding.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gutter, 0.0F));

        // BeginChild()'s return value (false when the child is fully clipped) is deliberately not
        // surfaced. The tab bodies do more than draw -- they advance chart smoothing and measure
        // their own layout for the next frame -- so they run every frame as they did before, and
        // ImGui skips the item submission itself when nothing is visible.
        ImGui::BeginChild(id, ImVec2(parentSize.x, 0.0F), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ~TabContentScope()
    {
        ImGui::EndChild();
        ImGui::PopClipRect();

        // The child reaches into the parent's right-hand padding, and ImGui records every item's
        // extent as content: left alone, that makes the parent a gutter's width wider than its own
        // view, i.e. horizontally scrollable where it never was. A sideways wheel or touchpad swipe
        // over the tab would then slide the tab bar off-screen -- and each scroll would widen the
        // content again, so there was no limit to it. Report the child as ending where the
        // parent's content region ends, which is what the layout means.
        ImGuiWindow* parent = ImGui::GetCurrentWindow();
        parent->DC.CursorMaxPos.x = std::max(m_ParentContentMaxX, std::min(parent->DC.CursorMaxPos.x, parent->WorkRect.Max.x));
        parent->DC.IdealMaxPos.x = std::max(m_ParentIdealMaxX, std::min(parent->DC.IdealMaxPos.x, parent->WorkRect.Max.x));
    }

    TabContentScope(const TabContentScope&) = delete;
    TabContentScope& operator=(const TabContentScope&) = delete;
    TabContentScope(TabContentScope&&) = delete;
    TabContentScope& operator=(TabContentScope&&) = delete;

  private:
    // The parent's content extent before the child was submitted; see the destructor.
    float m_ParentContentMaxX = 0.0F;
    float m_ParentIdealMaxX = 0.0F;
};

} // namespace UI::Widgets
