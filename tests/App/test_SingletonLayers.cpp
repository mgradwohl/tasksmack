#include "gtest/gtest.h"

#include <memory>

namespace
{

// ========== Mock Singleton Layer for Testing ==========

class MockLayer
{
  public:
    MockLayer() = default;
    virtual ~MockLayer() = default;

    MockLayer(const MockLayer&) = delete;
    MockLayer& operator=(const MockLayer&) = delete;
    MockLayer(MockLayer&&) = delete;
    MockLayer& operator=(MockLayer&&) = delete;

    virtual void onAttach()
    {
    }
    virtual void onDetach()
    {
        if (s_Instance == this)
        {
            s_Instance = nullptr;
        }
    }

    [[nodiscard]] static auto instance() -> MockLayer*
    {
        return s_Instance;
    }

    // Non-owning singleton registration (pattern used by AboutLayer/SettingsLayer)
    void registerInstance()
    {
        s_Instance = this;
    }

  private:
    // Non-owning singleton pointer; points to a layer owned by the application's layer stack
    static MockLayer* s_Instance;
};

// Static member definition
MockLayer* MockLayer::s_Instance = nullptr;

} // namespace

// ========== Singleton Pattern Unit Tests ==========

class NonOwningSingletonTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        // Ensure singleton is cleared after each test
        if (auto* layer = MockLayer::instance())
        {
            layer->onDetach();
        }
    }
};

TEST_F(NonOwningSingletonTest, InstanceReturnsNullptrByDefault)
{
    // New test instance should have no singleton set
    EXPECT_EQ(MockLayer::instance(), nullptr);
}

TEST_F(NonOwningSingletonTest, SetInstanceStoresNonOwningReference)
{
    MockLayer layer;
    MockLayer::setInstance(layer);

    EXPECT_EQ(MockLayer::instance(), &layer);
}

TEST_F(NonOwningSingletonTest, OnDetachClearsSingleton)
{
    MockLayer layer;
    MockLayer::setInstance(layer);

    EXPECT_EQ(MockLayer::instance(), &layer);

    layer.onDetach();

    EXPECT_EQ(MockLayer::instance(), nullptr);
}

TEST_F(NonOwningSingletonTest, MultipleInstancesCanBeSetSequentially)
{
    MockLayer layer1;
    MockLayer layer2;

    // Set and clear layer1
    MockLayer::setInstance(layer1);
    EXPECT_EQ(MockLayer::instance(), &layer1);

    layer1.onDetach();
    EXPECT_EQ(MockLayer::instance(), nullptr);

    // Now set layer2
    MockLayer::setInstance(layer2);
    EXPECT_EQ(MockLayer::instance(), &layer2);

    layer2.onDetach();
    EXPECT_EQ(MockLayer::instance(), nullptr);
}

TEST_F(NonOwningSingletonTest, SetInstanceCanReplaceExistingInstance)
{
    MockLayer layer1;
    MockLayer layer2;

    MockLayer::setInstance(layer1);
    EXPECT_EQ(MockLayer::instance(), &layer1);

    // Replace with layer2
    MockLayer::setInstance(layer2);
    EXPECT_EQ(MockLayer::instance(), &layer2);

    layer2.onDetach();
    EXPECT_EQ(MockLayer::instance(), nullptr);
}

TEST_F(NonOwningSingletonTest, OnDetachOnlyIfInstanceMatches)
{
    MockLayer layer1;
    MockLayer layer2;

    MockLayer::setInstance(layer1);
    EXPECT_EQ(MockLayer::instance(), &layer1);

    // Replace with layer2
    MockLayer::setInstance(layer2);
    EXPECT_EQ(MockLayer::instance(), &layer2);

    // Calling onDetach on layer1 should not clear singleton (it's not the current instance)
    layer1.onDetach();
    EXPECT_EQ(MockLayer::instance(), &layer2);

    // Only calling onDetach on layer2 should clear it
    layer2.onDetach();
    EXPECT_EQ(MockLayer::instance(), nullptr);
}
