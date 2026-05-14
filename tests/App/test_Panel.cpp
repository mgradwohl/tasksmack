#include "App/Panel.h"
#include "Core/WindowEvents.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace App
{
namespace
{

class TestPanel final : public Panel
{
  public:
    explicit TestPanel(std::string name = "TestPanel") : Panel(std::move(name))
    {}

    void render(bool* /*open*/) override
    {}
};

TEST(PanelTest, NameAndDefaultVisibility)
{
    TestPanel panel("PanelName");
    EXPECT_EQ(panel.name(), "PanelName");
    EXPECT_TRUE(panel.isVisible());
}

TEST(PanelTest, VisibilitySetAndToggle)
{
    TestPanel panel;
    panel.setVisible(false);
    EXPECT_FALSE(panel.isVisible());

    panel.toggleVisible();
    EXPECT_TRUE(panel.isVisible());

    panel.toggleVisible();
    EXPECT_FALSE(panel.isVisible());
}

TEST(PanelTest, DefaultLifecycleMethodsAreNoOps)
{
    TestPanel panel;
    Core::WindowCloseEvent event;

    panel.onAttach();
    panel.onUpdate(0.016F);
    panel.onEvent(event);
    panel.onDetach();

    EXPECT_FALSE(event.isHandled());
}

TEST(PanelTest, RenderCanBeInvokedWithNullOpenFlag)
{
    TestPanel panel;
    EXPECT_NO_THROW(panel.render(nullptr));
}

} // namespace
} // namespace App
