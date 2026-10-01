#include "App/DialogGeometry.h"
#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

// These assert the *production* constants, not literals supplied by the test.
//
// An earlier version of this guard passed its own 45.0F to computeDialogWidth() and claimed to prove
// the dialog was unchanged at the reference configuration. It proved only that the arithmetic works:
// changing ELEVATION_WIDTH_EM would have left it green while the dialog restyled. Asserting the
// symbols is what makes it a real guard.

using UI::DialogMetrics::computeActionButtonWidth;
using UI::DialogMetrics::computeDialogWidth;

constexpr float UNCONSTRAINED_VIEWPORT = 100000.0F;

// "OK" is far narrower than either button floor, so the floor is what decides at the reference size.
constexpr float NARROW_LABEL_PX = 14.0F;

TEST(DialogGeometryTest, ReferenceEmIsTheMediumPresetAt96Dpi)
{
    // 8pt at 96 DPI. Everything below is derived against this, so if it ever changes, every pixel
    // expectation in this file changes with it and should be re-derived rather than adjusted.
    EXPECT_FLOAT_EQ(REFERENCE_EM_PX, 8.0F * 96.0F / 72.0F);
}

TEST(DialogGeometryTest, AboutMarginReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(ABOUT_MARGIN_EM * REFERENCE_EM_PX, 32.0F);
}

TEST(DialogGeometryTest, AboutIconReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(ABOUT_ICON_EM * REFERENCE_EM_PX, 96.0F);
}

TEST(DialogGeometryTest, AboutButtonReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX, ABOUT_BUTTON_MIN_EM), 120.0F);
}

TEST(DialogGeometryTest, ElevationNoticeReproducesItsFormerPixelWidth)
{
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 480.0F);
}

TEST(DialogGeometryTest, ElevationButtonReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX, ELEVATION_BUTTON_MIN_EM), 100.0F);
}

TEST(DialogGeometryTest, EveryDialogConstantScalesWithTheFont)
{
    // The whole point of the change: double the font, double the geometry. A constant accidentally
    // left as a pixel literal would not move here.
    EXPECT_FLOAT_EQ(ABOUT_MARGIN_EM * (REFERENCE_EM_PX * 2.0F), 64.0F);
    EXPECT_FLOAT_EQ(ABOUT_ICON_EM * (REFERENCE_EM_PX * 2.0F), 192.0F);
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX * 2.0F, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 960.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX * 2.0F, ABOUT_BUTTON_MIN_EM), 240.0F);
}

} // namespace
} // namespace App
