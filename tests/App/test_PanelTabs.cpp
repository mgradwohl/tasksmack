#include "App/Panel.h"
#include "App/PanelTabs.h"
#include "Core/Event.h"
#include "Core/WindowEvents.h"

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace App
{
namespace
{

class RecordingPanel final : public Panel
{
  public:
    RecordingPanel(const std::string& name, std::vector<std::string>& calls) : Panel(name), m_Calls(calls)
    {}

    void onAttach() override
    {
        m_Calls.get().push_back(name() + ":attach");
    }

    void onDetach() override
    {
        m_Calls.get().push_back(name() + ":detach");
    }

    void onUpdate(float deltaTime) override
    {
        lastDeltaTime = deltaTime;
        m_Calls.get().push_back(name() + ":update");
    }

    void onEvent(Core::Event& event) override
    {
        lastEvent = &event;
        m_Calls.get().push_back(name() + ":event");
    }

    void render(bool* /*open*/) override
    {
        m_Calls.get().push_back(name() + ":window");
    }

    void renderContent() override
    {
        m_Calls.get().push_back(name() + ":content");
    }

    float lastDeltaTime = 0.0F;
    Core::Event* lastEvent = nullptr;

  private:
    std::reference_wrapper<std::vector<std::string>> m_Calls;
};

TEST(PanelTabsTest, RegistrationDrivesSelectionLabelsAndContent)
{
    std::vector<std::string> calls;
    RecordingPanel system("system", calls);
    RecordingPanel processes("processes", calls);
    RecordingPanel details("details", calls);
    RecordingPanel extra("extra", calls);
    std::string detailsLabel = "Select a process";
    PanelTabs tabs{{.panel = system, .eventName = "SystemOverview", .label = [] { return "hostname"; }},
                   {.panel = processes, .eventName = "Processes", .label = [] { return "Processes"; }},
                   {.panel = details, .eventName = "ProcessDetails", .label = [&detailsLabel] { return detailsLabel.c_str(); }},
                   {.panel = extra, .eventName = "Extra", .label = [] { return "Fourth tab"; }}};

    EXPECT_EQ(tabs.tabs().size(), 4U);
    EXPECT_EQ(&tabs.activeTab().panel.get(), &system);
    EXPECT_EQ(tabs.activeTab().eventName, "SystemOverview");
    tabs.renderContent();

    tabs.select(1);
    EXPECT_EQ(tabs.activeTab().eventName, "Processes");
    tabs.renderContent();

    tabs.select(2);
    EXPECT_EQ(tabs.activeTab().eventName, "ProcessDetails");
    EXPECT_STREQ(tabs.activeTab().label(), "Select a process");
    // Force label storage to change; the provider must not retain an old c_str().
    detailsLabel = std::string(128, 'x');
    EXPECT_STREQ(tabs.activeTab().label(), detailsLabel.c_str());
    tabs.renderContent();

    tabs.select(3);
    EXPECT_EQ(tabs.activeTab().eventName, "Extra");
    EXPECT_STREQ(tabs.activeTab().label(), "Fourth tab");
    tabs.renderContent();
    EXPECT_EQ(calls, (std::vector<std::string>{"system:content", "processes:content", "details:content", "extra:content"}));

    const auto* previousTab = &tabs.activeTab();
    tabs.select(3);
    EXPECT_EQ(previousTab, &tabs.activeTab());
    tabs.select(0);
    EXPECT_NE(previousTab, &tabs.activeTab());
}

TEST(PanelTabsTest, OwnsEventNamesAndLabelProviderValues)
{
    std::vector<std::string> calls;
    RecordingPanel panel("panel", calls);
    std::string eventName = "OriginalEvent";
    PanelTabs tabs{{.panel = panel, .eventName = eventName, .label = [] { return "Panel"; }},
                   {.panel = panel,
                    .eventName = std::string("TemporaryEvent"),
                    .label = [label = std::string("Owned label")] { return label.c_str(); }}};

    eventName.front() = 'X';
    EXPECT_EQ(tabs.activeTab().eventName, "OriginalEvent");

    tabs.select(1);
    EXPECT_EQ(tabs.activeTab().eventName, "TemporaryEvent");
    EXPECT_STREQ(tabs.activeTab().label(), "Owned label");
}

TEST(PanelTabsTest, ForwardsLifecycleToInactiveTabsAndDetachesInReverse)
{
    std::vector<std::string> calls;
    RecordingPanel first("first", calls);
    RecordingPanel second("second", calls);
    PanelTabs tabs{{.panel = first, .eventName = "First", .label = [] { return "First"; }},
                   {.panel = second, .eventName = "Second", .label = [] { return "Second"; }}};
    Core::WindowCloseEvent event;
    constexpr float DELTA_TIME = 0.25F;

    tabs.onAttach();
    tabs.onUpdate(DELTA_TIME);
    tabs.onEvent(event);
    tabs.onDetach();

    EXPECT_EQ(calls,
              (std::vector<std::string>{"first:attach",
                                        "second:attach",
                                        "first:update",
                                        "second:update",
                                        "first:event",
                                        "second:event",
                                        "second:detach",
                                        "first:detach"}));
    EXPECT_FLOAT_EQ(first.lastDeltaTime, DELTA_TIME);
    EXPECT_FLOAT_EQ(second.lastDeltaTime, DELTA_TIME);
    EXPECT_EQ(first.lastEvent, &event);
    EXPECT_EQ(second.lastEvent, &event);
}

TEST(PanelTabsTest, RejectsEmptyRegistryAndInvalidSelection)
{
    EXPECT_THROW((PanelTabs{}), std::invalid_argument);

    std::vector<std::string> calls;
    RecordingPanel panel("panel", calls);
    PanelTabs tabs{{.panel = panel, .eventName = "Panel", .label = [] { return "Panel"; }}};
    EXPECT_THROW(tabs.select(tabs.tabs().size()), std::out_of_range);
    EXPECT_EQ(&tabs.activeTab().panel.get(), &panel);
}

} // namespace
} // namespace App
