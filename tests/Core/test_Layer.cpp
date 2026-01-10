/// @file test_Layer.cpp
/// @brief Tests for Core::Layer base class
///
/// Tests cover:
/// - Layer construction and naming
/// - Default implementations of lifecycle methods
/// - Virtual method overriding
/// - Copy and move semantics

#include "Core/Layer.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

/// Concrete layer for testing
class ConcreteLayer : public Core::Layer
{
  public:
    explicit ConcreteLayer(const std::string& name = "ConcreteLayer") : Layer(name)
    {
    }

    void onAttach() override
    {
        attachCount++;
    }

    void onDetach() override
    {
        detachCount++;
    }

    void onUpdate(float deltaTime) override
    {
        updateCount++;
        lastDelta = deltaTime;
    }

    void onRender() override
    {
        renderCount++;
    }

    void onPostRender() override
    {
        postRenderCount++;
    }

    int attachCount = 0;
    int detachCount = 0;
    int updateCount = 0;
    int renderCount = 0;
    int postRenderCount = 0;
    float lastDelta = 0.0F;
};

} // namespace

// =============================================================================
// Construction Tests
// =============================================================================

TEST(LayerTest, ConstructWithDefaultName)
{
    Core::Layer layer;
    EXPECT_EQ(layer.getName(), "Layer");
}

TEST(LayerTest, ConstructWithCustomName)
{
    Core::Layer layer("CustomLayer");
    EXPECT_EQ(layer.getName(), "CustomLayer");
}

TEST(LayerTest, ConstructConcreteLayer)
{
    ConcreteLayer layer("TestLayer");
    EXPECT_EQ(layer.getName(), "TestLayer");
}

// =============================================================================
// Default Lifecycle Behavior Tests
// =============================================================================

TEST(LayerTest, DefaultOnAttachDoesNothing)
{
    Core::Layer layer;
    layer.onAttach(); // Should not crash
    SUCCEED();
}

TEST(LayerTest, DefaultOnDetachDoesNothing)
{
    Core::Layer layer;
    layer.onDetach(); // Should not crash
    SUCCEED();
}

TEST(LayerTest, DefaultOnUpdateDoesNothing)
{
    Core::Layer layer;
    layer.onUpdate(0.016F); // Should not crash
    SUCCEED();
}

TEST(LayerTest, DefaultOnRenderDoesNothing)
{
    Core::Layer layer;
    layer.onRender(); // Should not crash
    SUCCEED();
}

TEST(LayerTest, DefaultOnPostRenderDoesNothing)
{
    Core::Layer layer;
    layer.onPostRender(); // Should not crash
    SUCCEED();
}

// =============================================================================
// Virtual Method Override Tests
// =============================================================================

TEST(LayerTest, OnAttachCanBeOverridden)
{
    ConcreteLayer layer;
    EXPECT_EQ(layer.attachCount, 0);

    layer.onAttach();
    EXPECT_EQ(layer.attachCount, 1);

    layer.onAttach();
    EXPECT_EQ(layer.attachCount, 2);
}

TEST(LayerTest, OnDetachCanBeOverridden)
{
    ConcreteLayer layer;
    EXPECT_EQ(layer.detachCount, 0);

    layer.onDetach();
    EXPECT_EQ(layer.detachCount, 1);
}

TEST(LayerTest, OnUpdateCanBeOverridden)
{
    ConcreteLayer layer;
    EXPECT_EQ(layer.updateCount, 0);

    layer.onUpdate(0.016F);
    EXPECT_EQ(layer.updateCount, 1);
    EXPECT_FLOAT_EQ(layer.lastDelta, 0.016F);
}

TEST(LayerTest, OnRenderCanBeOverridden)
{
    ConcreteLayer layer;
    EXPECT_EQ(layer.renderCount, 0);

    layer.onRender();
    EXPECT_EQ(layer.renderCount, 1);

    layer.onRender();
    EXPECT_EQ(layer.renderCount, 2);
}

TEST(LayerTest, OnPostRenderCanBeOverridden)
{
    ConcreteLayer layer;
    EXPECT_EQ(layer.postRenderCount, 0);

    layer.onPostRender();
    EXPECT_EQ(layer.postRenderCount, 1);
}

// =============================================================================
// Lifecycle Sequence Tests
// =============================================================================

TEST(LayerTest, LifecycleSequence)
{
    ConcreteLayer layer;

    // Typical lifecycle
    layer.onAttach();
    EXPECT_EQ(layer.attachCount, 1);

    layer.onUpdate(0.016F);
    EXPECT_EQ(layer.updateCount, 1);

    layer.onRender();
    EXPECT_EQ(layer.renderCount, 1);

    layer.onPostRender();
    EXPECT_EQ(layer.postRenderCount, 1);

    layer.onDetach();
    EXPECT_EQ(layer.detachCount, 1);
}

TEST(LayerTest, MultipleUpdateCycles)
{
    ConcreteLayer layer;

    layer.onAttach();

    for (int i = 0; i < 10; ++i)
    {
        layer.onUpdate(0.016F);
        layer.onRender();
        layer.onPostRender();
    }

    EXPECT_EQ(layer.updateCount, 10);
    EXPECT_EQ(layer.renderCount, 10);
    EXPECT_EQ(layer.postRenderCount, 10);

    layer.onDetach();
}

// =============================================================================
// Name Access Tests
// =============================================================================

TEST(LayerTest, GetNameReturnsCorrectName)
{
    Core::Layer layer1("Layer1");
    Core::Layer layer2("Layer2");

    EXPECT_EQ(layer1.getName(), "Layer1");
    EXPECT_EQ(layer2.getName(), "Layer2");
}

TEST(LayerTest, GetNameReturnsConstReference)
{
    Core::Layer layer("TestLayer");
    const std::string& name = layer.getName();

    EXPECT_EQ(name, "TestLayer");
    EXPECT_EQ(&name, &layer.getName()); // Same reference
}

// =============================================================================
// Copy and Move Semantics Tests
// =============================================================================

TEST(LayerTest, LayerIsCopyable)
{
    EXPECT_TRUE(std::is_copy_constructible_v<Core::Layer>);
    EXPECT_TRUE(std::is_copy_assignable_v<Core::Layer>);
}

TEST(LayerTest, LayerIsMovable)
{
    EXPECT_TRUE(std::is_move_constructible_v<Core::Layer>);
    EXPECT_TRUE(std::is_move_assignable_v<Core::Layer>);
}

TEST(LayerTest, CopyConstructorPreservesName)
{
    Core::Layer layer1("Original");
    Core::Layer layer2(layer1); // Intentional copy to exercise copy constructor in this test
                                 // NOLINT(performance-unnecessary-copy-initialization)

    EXPECT_EQ(layer2.getName(), "Original");
}

TEST(LayerTest, CopyAssignmentPreservesName)
{
    Core::Layer layer1("Original");
    Core::Layer layer2("Other");

    layer2 = layer1;

    EXPECT_EQ(layer2.getName(), "Original");
}

TEST(LayerTest, MoveConstructorPreservesName)
{
    Core::Layer layer1("Original");
    Core::Layer layer2(std::move(layer1));

    EXPECT_EQ(layer2.getName(), "Original");
}

TEST(LayerTest, MoveAssignmentPreservesName)
{
    Core::Layer layer1("Original");
    Core::Layer layer2("Other");

    layer2 = std::move(layer1);

    EXPECT_EQ(layer2.getName(), "Original");
}

// =============================================================================
// Polymorphism Tests
// =============================================================================

TEST(LayerTest, PolymorphicBehavior)
{
    std::unique_ptr<Core::Layer> layer = std::make_unique<ConcreteLayer>("Polymorphic");

    EXPECT_EQ(layer->getName(), "Polymorphic");

    // Call virtual methods through base pointer
    layer->onAttach();
    layer->onUpdate(0.016F);
    layer->onRender();
    layer->onPostRender();
    layer->onDetach();

    // Dynamic cast to check derived behavior
    auto* concrete = dynamic_cast<ConcreteLayer*>(layer.get());
    ASSERT_NE(concrete, nullptr);
    EXPECT_EQ(concrete->attachCount, 1);
    EXPECT_EQ(concrete->updateCount, 1);
    EXPECT_EQ(concrete->renderCount, 1);
    EXPECT_EQ(concrete->postRenderCount, 1);
    EXPECT_EQ(concrete->detachCount, 1);
}
