#include "UI/Widgets.h"

#include <gtest/gtest.h>

namespace UI::Widgets
{
namespace
{

TEST(WidgetsHeaderTest, MinBarFillHeightConstantIsOnePixel)
{
    EXPECT_FLOAT_EQ(MIN_BAR_FILL_HEIGHT, 1.0F);
}

} // namespace
} // namespace UI::Widgets
