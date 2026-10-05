#include "App/FpsCounter.h"

#include <gtest/gtest.h>

#include <cmath>

namespace App
{
namespace
{

TEST(FpsCounterTest, NonPositiveAveragingWindowIsClampedToAvoidDivideByZero)
{
    // A zero or negative window would otherwise let update() divide by an accumulator that
    // can still be exactly zero (e.g. the very first frame's deltaTime being 0.0F).
    FpsCounter zeroWindow(0.0F);
    zeroWindow.update(0.0F);
    EXPECT_TRUE(std::isfinite(zeroWindow.displayedFps()));

    FpsCounter negativeWindow(-1.0F);
    negativeWindow.update(0.0F);
    EXPECT_TRUE(std::isfinite(negativeWindow.displayedFps()));
}

TEST(FpsCounterTest, InitialStateIsZero)
{
    const FpsCounter counter;
    EXPECT_FLOAT_EQ(counter.frameTime(), 0.0F);
    EXPECT_FLOAT_EQ(counter.displayedFps(), 0.0F);
}

TEST(FpsCounterTest, FrameTimeTracksMostRecentUpdate)
{
    FpsCounter counter(0.5F);
    counter.update(0.016F);
    EXPECT_FLOAT_EQ(counter.frameTime(), 0.016F);
    counter.update(0.033F);
    EXPECT_FLOAT_EQ(counter.frameTime(), 0.033F);
}

TEST(FpsCounterTest, DisplayedFpsUnchangedBeforeWindowElapses)
{
    FpsCounter counter(0.5F);
    // Well under the 0.5s averaging window - displayedFps should still be its initial 0.0.
    counter.update(0.1F);
    counter.update(0.1F);
    EXPECT_FLOAT_EQ(counter.displayedFps(), 0.0F);
}

TEST(FpsCounterTest, DisplayedFpsComputedOnceWindowElapses)
{
    FpsCounter counter(0.5F);
    // 5 frames of 0.1s each = 0.5s accumulated, exactly at the window boundary: 5 frames / 0.5s = 10 FPS.
    for (int i = 0; i < 5; ++i)
    {
        counter.update(0.1F);
    }
    EXPECT_FLOAT_EQ(counter.displayedFps(), 10.0F);
}

TEST(FpsCounterTest, AccumulatorResetsAfterWindowElapses)
{
    FpsCounter counter(0.5F);
    for (int i = 0; i < 5; ++i)
    {
        counter.update(0.1F);
    }
    const float firstFps = counter.displayedFps();

    // One more small frame: not enough on its own to trigger a recompute, so the
    // previously-computed FPS should still be reported unchanged.
    counter.update(0.05F);
    EXPECT_FLOAT_EQ(counter.displayedFps(), firstFps);
}

TEST(FpsCounterTest, HighFrameRateProducesHighDisplayedFps)
{
    FpsCounter counter(0.5F);
    // 60 FPS: ~0.0166667s per frame. 30 frames ~= 0.5s.
    constexpr float delta = 1.0F / 60.0F;
    for (int i = 0; i < 30; ++i)
    {
        counter.update(delta);
    }
    EXPECT_NEAR(counter.displayedFps(), 60.0F, 1.0F);
}

TEST(FpsCounterTest, DisplayedFrameTimeAveragesTheSameWindowAsFps)
{
    // #1037: the status bar paired the window's FPS with the last single frame's time. Fast frames
    // then one slow one: the displayed frame time is the window's mean, consistent with the FPS.
    FpsCounter counter(0.5F);
    EXPECT_FLOAT_EQ(counter.displayedFrameTime(), 0.0F);
    for (int i = 0; i < 4; ++i)
    {
        counter.update(0.05F);
    }
    counter.update(0.3F); // window: 5 frames over 0.5 s
    EXPECT_FLOAT_EQ(counter.frameTime(), 0.3F);
    EXPECT_FLOAT_EQ(counter.displayedFps(), 10.0F);
    EXPECT_FLOAT_EQ(counter.displayedFrameTime(), 0.1F);
    EXPECT_FLOAT_EQ(counter.displayedFrameTime() * counter.displayedFps(), 1.0F);

    // Unchanged until the next window completes.
    counter.update(0.01F);
    EXPECT_FLOAT_EQ(counter.displayedFrameTime(), 0.1F);
}

TEST(FpsCounterTest, CustomAveragingWindowIsRespected)
{
    // A 1-second window shouldn't fire after only 0.5s of accumulated frames.
    FpsCounter counter(1.0F);
    counter.update(0.5F);
    EXPECT_FLOAT_EQ(counter.displayedFps(), 0.0F);

    counter.update(0.5F);
    // 2 frames over 1.0s = 2 FPS.
    EXPECT_FLOAT_EQ(counter.displayedFps(), 2.0F);
}

TEST(FpsCounterTest, FramesSlowerThanATenthOfASecondReadTrue)
{
    // #1152: fed the real 150 ms frame time (not the 0.1 s-capped animation delta), a 6.7 FPS stall
    // reads 6.7 FPS, not 10.
    FpsCounter counter(0.5F);
    for (int i = 0; i < 4; ++i)
    {
        counter.update(0.15F);
    }
    EXPECT_NEAR(counter.displayedFps(), 1.0F / 0.15F, 1e-3F);
    EXPECT_NEAR(counter.displayedFrameTime(), 0.15F, 1e-6F);
}

} // namespace
} // namespace App
