/// @file test_IconLoader.cpp
/// @brief Tests for UI::Texture RAII/move semantics and UI::loadTexture()'s early-return
/// error paths (#771).
///
/// UI::IconLoader.cpp only needs imgui.h for type declarations (ImTextureID/ImVec2) - it
/// makes no ImGui:: calls - and its GL calls resolve against glad_gl_core_33, which is
/// already linked into TaskSmackTests, so the real file is compiled and linked here (unlike
/// TitleBarLayer.cpp/ProcessesPanel.cpp, which need real ImGui/ImPlot library symbols this
/// binary doesn't have). Its success path (a real image + real glGenTextures/glTexImage2D)
/// is intentionally not covered here: that needs a live OpenGL context, which this test
/// binary doesn't create. Every case below returns before any GL call is made, so no context
/// is required.

#include "UI/IconLoader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <utility>

namespace UI
{
namespace
{

TEST(IconLoaderTest, DefaultConstructedTextureIsInvalid)
{
    const Texture tex;
    EXPECT_FALSE(tex.valid());
    EXPECT_EQ(tex.textureId(), static_cast<ImTextureID>(0));
    EXPECT_FLOAT_EQ(tex.size().x, 0.0F);
    EXPECT_FLOAT_EQ(tex.size().y, 0.0F);
}

TEST(IconLoaderTest, MoveConstructFromDefaultLeavesBothInvalid)
{
    Texture source;
    // NOLINTNEXTLINE(bugprone-use-after-move) - checking the moved-from object's public
    // post-move state (m_Id reset to 0) is exactly the point here.
    const Texture moved(std::move(source));
    EXPECT_FALSE(moved.valid());
    EXPECT_FALSE(source.valid());
}

TEST(IconLoaderTest, MoveAssignFromDefaultLeavesBothInvalid)
{
    Texture source;
    Texture target;
    // NOLINTNEXTLINE(bugprone-use-after-move) - see above.
    target = std::move(source);
    EXPECT_FALSE(target.valid());
    EXPECT_FALSE(source.valid());
}

TEST(IconLoaderTest, SelfMoveAssignIsSafe)
{
    // Texture::operator=(Texture&&) guards against self-assignment (this != &other); this
    // exercises a default-constructed (m_Id == 0) instance so no GL call happens either way.
    Texture tex;
    auto& ref = tex;
    tex = std::move(ref);
    EXPECT_FALSE(tex.valid());
}

TEST(IconLoaderTest, LoadTextureWithEmptyPathReturnsInvalidTexture)
{
    // loadTexture() checks path.empty() before any stbi_load/GL call.
    const Texture tex = loadTexture(std::filesystem::path{});
    EXPECT_FALSE(tex.valid());
}

TEST(IconLoaderTest, LoadTextureWithNonexistentFileReturnsInvalidTexture)
{
    // stbi_load() returns nullptr for a nonexistent file before any GL call is made. Assert
    // the path is actually absent first so this can't silently become the live-GL success
    // path (needing a context this test binary doesn't create) if it ever existed on some
    // machine/CI image.
    const std::filesystem::path nonexistent{"this/path/definitely/does/not/exist_12345.png"};
    ASSERT_FALSE(std::filesystem::exists(nonexistent));

    const Texture tex = loadTexture(nonexistent);
    EXPECT_FALSE(tex.valid());
}

TEST(IconLoaderTest, LoadTextureWithNonImageFileReturnsInvalidTexture)
{
    // A file that exists but isn't a valid image also fails inside stbi_load(), still before
    // any GL call - use this test file itself as guaranteed-not-an-image content.
    const std::filesystem::path notAnImage = __FILE__;
    ASSERT_TRUE(std::filesystem::exists(notAnImage));
    const Texture tex = loadTexture(notAnImage);
    EXPECT_FALSE(tex.valid());
}

} // namespace
} // namespace UI
