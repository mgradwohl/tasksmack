#include "App/DialogGeometry.h"
#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>

#include <limits>

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
using UI::DialogMetrics::computeFittedDialogWidth;

constexpr float UNCONSTRAINED_VIEWPORT = 100000.0F;

// "OK" is far narrower than either button floor, so the floor is what decides at the reference size.
constexpr float NARROW_LABEL_PX = 14.0F;

TEST(DialogGeometryTest, ReferenceEmIsTheMediumPresetAt96Dpi)
{
    // 8pt at 96 DPI. Everything below is derived against this, so if it ever changes, every pixel
    // expectation in this file changes with it and should be re-derived rather than adjusted.
    EXPECT_FLOAT_EQ(REFERENCE_EM_PX, 8.0F * 96.0F / 72.0F);
}

// The About box's compact redesign (#1490) halved its margin, shrank its icon to the height of the
// text beside it and gave it an authored width; these pin the sizes it was designed at.
TEST(DialogGeometryTest, AboutMarginIsItsDesignedPixelSize)
{
    EXPECT_FLOAT_EQ(ABOUT_MARGIN_EM * REFERENCE_EM_PX, 16.0F);
}

TEST(DialogGeometryTest, AboutIconIsItsDesignedPixelSize)
{
    EXPECT_FLOAT_EQ(ABOUT_ICON_EM * REFERENCE_EM_PX, 64.0F);
    EXPECT_FLOAT_EQ(ABOUT_HEADER_GAP_EM * REFERENCE_EM_PX, 16.0F);
}

TEST(DialogGeometryTest, AboutWidthIsItsDesignedPixelSize)
{
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX, ABOUT_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 384.0F);
}

TEST(DialogGeometryTest, AboutHeightCapIsBelowTheSharedCap)
{
    EXPECT_GT(ABOUT_MAX_HEIGHT_FRACTION, 0.0F);
    EXPECT_LT(ABOUT_MAX_HEIGHT_FRACTION, UI::DialogMetrics::MAX_VIEWPORT_FRACTION);
}

TEST(DialogGeometryTest, AboutButtonReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX, ABOUT_BUTTON_MIN_EM), 120.0F);
}

TEST(DialogGeometryTest, ElevationNoticeReproducesItsFormerPixelWidth)
{
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 480.0F);
}

TEST(DialogGeometryTest, ElevationNoticeFloorIsItsDesignedPixelWidth)
{
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 240.0F);
}

// #1601: the notice fits its text between the floor and its authored 480px.
TEST(DialogGeometryTest, ElevationNoticeShrinksToShortText)
{
    // The Windows text's widest line is roughly 300px at the reference em.
    EXPECT_FLOAT_EQ(computeFittedDialogWidth(316.0F, REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT),
                    316.0F);
}

TEST(DialogGeometryTest, ElevationNoticeWrapsLongTextAtItsAuthoredWidth)
{
    // The Linux text's paragraphs run to well over 480px unwrapped.
    EXPECT_FLOAT_EQ(computeFittedDialogWidth(900.0F, REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT),
                    480.0F);
}

TEST(DialogGeometryTest, ElevationNoticeNeverFitsBelowItsFloor)
{
    EXPECT_FLOAT_EQ(computeFittedDialogWidth(50.0F, REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT),
                    240.0F);
    // A width that could not be measured counts as none: the floor.
    EXPECT_FLOAT_EQ(
        computeFittedDialogWidth(
            std::numeric_limits<float>::quiet_NaN(), REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT),
        240.0F);
}

TEST(DialogGeometryTest, FittedWidthStillFitsANarrowViewport)
{
    // 90% of a 200px viewport is below both the floor and the content: the viewport wins.
    EXPECT_FLOAT_EQ(computeFittedDialogWidth(316.0F, REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, 200.0F), 180.0F);
    // 90% of 400px caps the long text below its authored 480px.
    EXPECT_FLOAT_EQ(computeFittedDialogWidth(900.0F, REFERENCE_EM_PX, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, 400.0F), 360.0F);
}

TEST(DialogGeometryTest, ElevationButtonReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX, ELEVATION_BUTTON_MIN_EM), 100.0F);
}

TEST(DialogGeometryTest, SettingsButtonReproducesItsFormerPixelSize)
{
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX, SETTINGS_BUTTON_MIN_EM), 100.0F);
}

TEST(DialogGeometryTest, EveryDialogConstantScalesWithTheFont)
{
    // The whole point of the change: double the font, double the geometry. A constant accidentally
    // left as a pixel literal would not move here.
    EXPECT_FLOAT_EQ(ABOUT_MARGIN_EM * (REFERENCE_EM_PX * 2.0F), 32.0F);
    EXPECT_FLOAT_EQ(ABOUT_ICON_EM * (REFERENCE_EM_PX * 2.0F), 128.0F);
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX * 2.0F, ABOUT_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 768.0F);
    EXPECT_FLOAT_EQ(computeDialogWidth(REFERENCE_EM_PX * 2.0F, ELEVATION_WIDTH_EM, UNCONSTRAINED_VIEWPORT), 960.0F);
    EXPECT_FLOAT_EQ(computeActionButtonWidth(NARROW_LABEL_PX, REFERENCE_EM_PX * 2.0F, ABOUT_BUTTON_MIN_EM), 240.0F);
}

} // namespace
} // namespace App
