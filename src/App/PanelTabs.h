#pragma once

#include "Panel.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace App
{

/// Non-owning tab registry. Panels, event-name storage, and objects captured by label
/// providers must outlive the registry. Providers return null-terminated display labels.
/// Registration order defines tab/attach/update/event order; detach runs in reverse.
class PanelTabs
{
  public:
    struct Tab
    {
        std::reference_wrapper<Panel> panel;
        std::string_view eventName;
        std::function<const char*()> label;
    };

    explicit PanelTabs(std::initializer_list<Tab> tabs) : m_Tabs(tabs)
    {
        if (m_Tabs.empty())
        {
            throw std::invalid_argument("PanelTabs requires at least one tab");
        }
    }

    [[nodiscard]] std::span<const Tab> tabs() const
    {
        return m_Tabs;
    }

    [[nodiscard]] const Tab& activeTab() const
    {
        return m_Tabs.at(m_ActiveIndex);
    }

    void select(std::size_t index)
    {
        // Validate before changing selection so an invalid index preserves the active tab.
        if (index >= m_Tabs.size())
        {
            throw std::out_of_range("PanelTabs selection is out of range");
        }
        m_ActiveIndex = index;
    }

    void onAttach()
    {
        for (const auto& tab : m_Tabs)
        {
            tab.panel.get().onAttach();
        }
    }

    void onDetach()
    {
        for (const auto& tab : m_Tabs | std::views::reverse)
        {
            tab.panel.get().onDetach();
        }
    }

    void onUpdate(float deltaTime)
    {
        for (const auto& tab : m_Tabs)
        {
            tab.panel.get().onUpdate(deltaTime);
        }
    }

    void onEvent(Core::Event& event)
    {
        for (const auto& tab : m_Tabs)
        {
            tab.panel.get().onEvent(event);
        }
    }

    void renderContent() const
    {
        activeTab().panel.get().renderContent();
    }

  private:
    std::vector<Tab> m_Tabs;
    std::size_t m_ActiveIndex = 0;
};

} // namespace App
