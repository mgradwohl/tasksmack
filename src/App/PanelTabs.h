#pragma once

#include "Panel.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

/// Registry owns event names and label-provider callables, but not panels.
/// Panels and objects referenced by provider captures must outlive the registry.
/// Providers return null-terminated display labels valid until the caller consumes them.
/// Registration order defines tab/attach/update/event order; detach runs in reverse.
class PanelTabs
{
  public:
    struct Tab
    {
        std::reference_wrapper<Panel> panel;
        std::string eventName;
        std::function<const char*()> label;
        /// The label's visible text, unescaped (no icon, no "###" ID): what TASKSMACK_TAB matches by
        /// name (#1559). Optional; a tab without it is matched by its event name only.
        std::function<std::string_view()> text = nullptr;
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
        // Each panel separately: one that throws must not stop the others stopping their samplers,
        // nor ShellLayer reaching its config save after this (#1124).
        for (const auto& tab : m_Tabs | std::views::reverse)
        {
            try
            {
                tab.panel.get().onDetach();
            }
            catch (const std::exception& e)
            {
                logDetachFailure(tab.eventName, e.what());
            }
            catch (...)
            {
                logDetachFailure(tab.eventName, "unknown exception");
            }
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
    /// Best-effort report of a panel throwing while detaching. Formatting can itself throw (e.g.
    /// out of memory), and that must not interrupt the teardown either (as guardLayerCall does).
    static void logDetachFailure(const std::string& panel, const char* what) noexcept
    {
        try
        {
            spdlog::error("Panel '{}' threw while detaching: {}", panel, what);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - logging is best effort and must not throw
        {}
    }

    std::vector<Tab> m_Tabs;
    std::size_t m_ActiveIndex = 0;
};

} // namespace App
