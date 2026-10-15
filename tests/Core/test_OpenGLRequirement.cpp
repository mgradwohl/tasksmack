/// @file test_OpenGLRequirement.cpp
/// @brief Tests for Core/OpenGLRequirement.h: which OpenGL context versions Core::Window accepts, and the
/// error it stops with for an older one, e.g. GDI Generic's OpenGL 1.1 on a PC with no GPU driver (#1398).

#include "Core/OpenGLRequirement.h"

#include <gtest/gtest.h>

#include <string>

namespace Core
{
namespace
{

TEST(OpenGLRequirementTest, RequiresOpenGL33)
{
    EXPECT_EQ(REQUIRED_GL_MAJOR, 3);
    EXPECT_EQ(REQUIRED_GL_MINOR, 3);
}

TEST(OpenGLRequirementTest, AcceptsTheRequiredVersionAndNewer)
{
    EXPECT_TRUE(meetsRequiredGLVersion(3, 3));
    EXPECT_TRUE(meetsRequiredGLVersion(3, 4));
    EXPECT_TRUE(meetsRequiredGLVersion(4, 0));
    EXPECT_TRUE(meetsRequiredGLVersion(4, 6));
}

TEST(OpenGLRequirementTest, RejectsOlderVersions)
{
    EXPECT_FALSE(meetsRequiredGLVersion(1, 1)); // GDI Generic
    EXPECT_FALSE(meetsRequiredGLVersion(2, 1));
    EXPECT_FALSE(meetsRequiredGLVersion(3, 0));
    EXPECT_FALSE(meetsRequiredGLVersion(3, 2));
    EXPECT_FALSE(meetsRequiredGLVersion(0, 0)); // GLAD found no version at all
}

TEST(OpenGLRequirementTest, MessageNamesTheRequiredAndActualVersionsAndTheDriver)
{
    const std::string message = unsupportedGLVersionMessage("1.1.0", "GDI Generic", "Microsoft Corporation");
    EXPECT_NE(message.find("OpenGL 3.3 or newer"), std::string::npos) << message;
    EXPECT_NE(message.find("only OpenGL 1.1.0 (GDI Generic, Microsoft Corporation)"), std::string::npos) << message;
}

} // namespace
} // namespace Core
