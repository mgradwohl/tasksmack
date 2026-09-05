/// @file test_IconLoader.cpp
/// @brief Tests for UI::Texture RAII/move semantics and UI::loadTexture()'s early-return
/// error paths (#771), plus its real-GL success path.
///
/// UI::IconLoader.cpp only needs imgui.h for type declarations (ImTextureID/ImVec2) - it
/// makes no ImGui:: calls - and its GL calls resolve against glad_gl_core_33, which is
/// already linked into TaskSmackTests, so the real file is compiled and linked here (unlike
/// TitleBarLayer.cpp/ProcessesPanel.cpp, which need real ImGui/ImPlot library symbols this
/// binary doesn't have).
///
/// Most cases below return before any GL call is made, so no context is required. The
/// loadTexture() success path (a real image + real glGenTextures/glTexImage2D) needs a live
/// OpenGL context; IconLoaderGLTest below constructs a real Core::Window (mirroring
/// test_Window.cpp's WindowTest fixture) to provide one when a display is available, skipping
/// otherwise (this is always the case on headless CI Windows runners, and on Linux CI unless the
/// offscreen SDL driver happens to support a GL 3.3 core context, which it does not).

#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Window.h"
#include "UI/IconLoader.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace UI
{
namespace
{

TEST(IconLoaderTest, DefaultConstructedTextureIsInvalid)
{
    const Texture tex;
    EXPECT_FALSE(tex.valid());
    EXPECT_EQ(tex.textureId(), ImTextureID{});
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

namespace
{
// Best-effort RAII cleanup for the temp file below: removes it on scope exit regardless of
// whether an ASSERT_* macro returns early or an exception unwinds the stack, instead of only
// on the test's normal fall-through path.
struct ScopedTempFile
{
    std::filesystem::path path;

    ~ScopedTempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec); // best-effort; a failed cleanup isn't a test failure
    }
};
} // namespace

TEST(IconLoaderTest, LoadTextureWithNonImageFileReturnsInvalidTexture)
{
    // A file that exists but isn't a valid image also fails inside stbi_load(), still before
    // any GL call. Write a small guaranteed-non-image temp file rather than using __FILE__:
    // __FILE__ may be a relative path (compiler-flag dependent) and isn't guaranteed to still
    // be an on-disk path at test-run time, while temp_directory_path() keeps this hermetic
    // across toolchains and working directories. The name is suffixed with a timestamp plus a
    // per-process monotonic counter (mirroring ScopedTempDir's approach, without a Linux-only
    // getpid() dependency): collisions are only practically ruled out within this process, not
    // guaranteed impossible against a different process landing on the exact same tick+counter
    // pair, but that's astronomically unlikely for a single-shot test file.
    static std::atomic<std::uint64_t> counter{0};
    const auto uniqueSuffix =
        std::format("{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), counter.fetch_add(1, std::memory_order_relaxed));
    const ScopedTempFile notAnImage{.path = std::filesystem::temp_directory_path() /
                                            std::format("tasksmack_iconloader_test_not_an_image_{}.tmp", uniqueSuffix)};
    {
        std::ofstream out(notAnImage.path, std::ios::binary);
        out << "not an image";
    }
    ASSERT_TRUE(std::filesystem::exists(notAnImage.path));

    const Texture tex = loadTexture(notAnImage.path);
    EXPECT_FALSE(tex.valid());
}

// ==========================================================================
// loadTexture() success path: needs a live OpenGL context.
// ==========================================================================

// Builds a minimal valid 1x1, 24-bit-per-pixel BMP (uncompressed, so no codec dependency
// beyond stb_image's built-in BMP decoder): a 14-byte BITMAPFILEHEADER, a 40-byte
// BITMAPINFOHEADER, and one BGR pixel padded to a 4-byte row boundary. Chosen over PNG/JPEG
// because the uncompressed format can be hand-built byte-for-byte without a codec dependency.
std::vector<unsigned char> makeMinimalRedBmp()
{
    constexpr std::uint32_t fileHeaderSize = 14;
    constexpr std::uint32_t dibHeaderSize = 40;
    constexpr std::uint32_t pixelDataOffset = fileHeaderSize + dibHeaderSize;
    constexpr std::uint32_t rowBytes = 4; // 1 pixel * 3 bytes (BGR) padded to a 4-byte boundary
    constexpr std::uint32_t fileSize = pixelDataOffset + rowBytes;

    const auto putU16 = [](std::vector<unsigned char>& buf, std::uint16_t v)
    {
        buf.push_back(static_cast<unsigned char>(v & 0xFFU));
        buf.push_back(static_cast<unsigned char>((v >> 8U) & 0xFFU));
    };
    const auto putU32 = [](std::vector<unsigned char>& buf, std::uint32_t v)
    {
        buf.push_back(static_cast<unsigned char>(v & 0xFFU));
        buf.push_back(static_cast<unsigned char>((v >> 8U) & 0xFFU));
        buf.push_back(static_cast<unsigned char>((v >> 16U) & 0xFFU));
        buf.push_back(static_cast<unsigned char>((v >> 24U) & 0xFFU));
    };

    std::vector<unsigned char> bmp;
    bmp.reserve(fileSize);

    // BITMAPFILEHEADER
    bmp.push_back('B');
    bmp.push_back('M');
    putU32(bmp, fileSize);
    putU32(bmp, 0); // reserved
    putU32(bmp, pixelDataOffset);

    // BITMAPINFOHEADER
    putU32(bmp, dibHeaderSize);
    putU32(bmp, 1);        // width
    putU32(bmp, 1);        // height (positive = bottom-up)
    putU16(bmp, 1);        // planes
    putU16(bmp, 24);       // bits per pixel
    putU32(bmp, 0);        // compression = BI_RGB (none)
    putU32(bmp, rowBytes); // image size
    putU32(bmp, 0);        // x pixels per meter
    putU32(bmp, 0);        // y pixels per meter
    putU32(bmp, 0);        // colors used
    putU32(bmp, 0);        // colors important

    // Pixel data: one BGR pixel (pure red), padded to 4 bytes.
    bmp.push_back(0x00); // B
    bmp.push_back(0x00); // G
    bmp.push_back(0xFF); // R
    bmp.push_back(0x00); // padding

    return bmp;
}

} // namespace

// Mirrors test_Window.cpp's hasDisplay()/WindowTest pattern: on Windows, any non-CI local run
// is assumed to have a display; on Linux, a real DISPLAY/WAYLAND_DISPLAY with a working GL 3.3
// core context, or (failing that) the offscreen SDL driver - which cannot create a GL context,
// so tests relying on it must still skip.
namespace
{
bool hasDisplay()
{
#ifdef _WIN32
    char* ciEnv = nullptr;
    std::size_t len = 0;
    _dupenv_s(&ciEnv, &len, "CI");
    const bool isCI = (ciEnv != nullptr && std::string_view(ciEnv) == "true");
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory, cppcoreguidelines-no-malloc) - _dupenv_s allocates with malloc; must free with free()
    free(ciEnv);
    return !isCI;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    if ((display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0'))
    {
        if (TestSupport::probeGLCapability())
        {
            return true;
        }
    }
    return false; // offscreen driver cannot create a GL 3.3 core context; no point trying
#endif
}

bool isOffscreenVideoDriver()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* videoDriver = std::getenv("SDL_VIDEODRIVER");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return videoDriver != nullptr && std::string_view(videoDriver) == "offscreen";
#endif
}

// Fixture providing a real OpenGL context via a real Core::Window, so loadTexture()'s
// glGenTextures/glTexImage2D success path can actually run.
class IconLoaderGLTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!hasDisplay())
        {
            GTEST_SKIP() << "No display available (headless environment)";
        }
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            GTEST_SKIP() << "SDL_Init(SDL_INIT_VIDEO) failed: " << SDL_GetError();
        }
        m_SdlInitialized = true;
    }

    void TearDown() override
    {
        if (m_SdlInitialized)
        {
            SDL_Quit();
            m_SdlInitialized = false;
        }
    }

    bool m_SdlInitialized = false;
};

} // namespace

namespace UI
{
namespace
{

TEST_F(IconLoaderGLTest, LoadTextureWithRealImageAndRealGLContextSucceeds)
{
    try
    {
        const Core::Window window(
            Core::WindowSpecification{.Title = "IconLoaderGLTest", .Width = 640, .Height = 480, .VSync = false, .Borderless = true});

        static std::atomic<std::uint64_t> counter{0};
        const auto uniqueSuffix = std::format(
            "{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), counter.fetch_add(1, std::memory_order_relaxed));
        const ScopedTempFile bmpFile{.path = std::filesystem::temp_directory_path() /
                                             std::format("tasksmack_iconloader_test_red_pixel_{}.bmp", uniqueSuffix)};
        {
            const auto bytes = makeMinimalRedBmp();
            std::ofstream out(bmpFile.path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }
        ASSERT_TRUE(std::filesystem::exists(bmpFile.path));

        Texture tex = loadTexture(bmpFile.path);

        EXPECT_TRUE(tex.valid());
        EXPECT_NE(tex.textureId(), ImTextureID{});
        EXPECT_FLOAT_EQ(tex.size().x, 1.0F);
        EXPECT_FLOAT_EQ(tex.size().y, 1.0F);

        // Texture's destructor calls glDeleteTextures(); it must run while window's GL
        // context is still current, so destroy it explicitly before window goes out of scope.
        tex = Texture{};
    }
    catch (const std::exception& e)
    {
        if (isOffscreenVideoDriver())
        {
            GTEST_SKIP() << "Window creation failed on offscreen driver (no GL): " << e.what();
        }
        FAIL() << "Window creation failed unexpectedly: " << e.what();
    }
}

} // namespace
} // namespace UI
} // namespace UI
