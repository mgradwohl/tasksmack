/// @file test_OpenGLRequirement.cpp
/// @brief Tests for Core/OpenGLRequirement.h: which OpenGL context versions Core::Window accepts, and the
/// error it stops with for an older one, e.g. GDI Generic's OpenGL 1.1 on a PC with no GPU driver (#1398).

#include "Core/OpenGLRequirement.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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

TEST(OpenGLRequirementTest, VersionStringAcceptsTheRequiredVersionAndNewer)
{
    EXPECT_TRUE(glVersionStringMeetsRequirement("3.3 (Core Profile) Mesa 26.2.4")); // CI's llvmpipe
    EXPECT_TRUE(glVersionStringMeetsRequirement("4.6.0 NVIDIA 560.94"));
    EXPECT_TRUE(glVersionStringMeetsRequirement("4.5"));
    EXPECT_TRUE(glVersionStringMeetsRequirement("3.10"));
    EXPECT_TRUE(glVersionStringMeetsRequirement(std::string_view{"10.0"}));
}

TEST(OpenGLRequirementTest, VersionStringRejectsOlderVersions)
{
    EXPECT_FALSE(glVersionStringMeetsRequirement("1.1.0")); // GDI Generic (#2257)
    EXPECT_FALSE(glVersionStringMeetsRequirement("2.1 Mesa 26.2.4"));
    EXPECT_FALSE(glVersionStringMeetsRequirement("3.2.0"));
}

TEST(OpenGLRequirementTest, VersionStringRejectsMissingOrUnparseableStrings)
{
    EXPECT_FALSE(glVersionStringMeetsRequirement(static_cast<const char*>(nullptr)));
    EXPECT_FALSE(glVersionStringMeetsRequirement(""));
    EXPECT_FALSE(glVersionStringMeetsRequirement(std::string_view{}));
    EXPECT_FALSE(glVersionStringMeetsRequirement("4"));
    EXPECT_FALSE(glVersionStringMeetsRequirement("4."));
    EXPECT_FALSE(glVersionStringMeetsRequirement(".3"));
    EXPECT_FALSE(glVersionStringMeetsRequirement("OpenGL 4.6"));
    EXPECT_FALSE(glVersionStringMeetsRequirement("99999999999999999999.0")); // no overflow into a "valid" major
}

TEST(OpenGLRequirementTest, VersionStringCheckIsConstexpr)
{
    static_assert(glVersionStringMeetsRequirement(std::string_view{"3.3"}));
    static_assert(!glVersionStringMeetsRequirement(std::string_view{"1.1.0"}));
}

TEST(OpenGLRequirementTest, MessageNamesTheRequiredAndActualVersionsAndTheDriver)
{
    const std::string message = unsupportedGLVersionMessage("1.1.0", "GDI Generic", "Microsoft Corporation");
    EXPECT_NE(message.find("OpenGL 3.3 or newer"), std::string::npos) << message;
    EXPECT_NE(message.find("only OpenGL 1.1.0 (GDI Generic, Microsoft Corporation)"), std::string::npos) << message;
}

} // namespace
} // namespace Core
