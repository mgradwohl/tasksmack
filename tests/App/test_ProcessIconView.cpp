/// @file test_ProcessIconView.cpp
/// @brief The icon beside a process's name (#1800), drawn headless: the generic glyph while an icon
/// loads and for a program without one, the program's texture (made once, through a fake backend)
/// once loaded, nothing at all while the setting is off, and every texture deleted on detach.

#include "App/Panels/ProcessIconView.h"
#include "Domain/ProcessIconCache.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessIconSource.h"
#include "UI/IconTextureCache.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

/// Every process of "/bin/withicon" has a solid icon; any other has none.
class ViewFakeIconSource final : public Platform::IProcessIconSource
{
  public:
    [[nodiscard]] std::string executablePath(std::int32_t pid) override
    {
        return pid == 1 ? "/bin/withicon" : "/bin/without";
    }

    [[nodiscard]] std::optional<Platform::IconPixels>
    loadIcon(std::string_view executablePath, std::string_view /*processName*/, int sizePx) override
    {
        if (executablePath != "/bin/withicon")
        {
            return std::nullopt;
        }
        const auto side = static_cast<std::size_t>(sizePx);
        return Platform::IconPixels{.width = sizePx, .height = sizePx, .rgba = std::vector<std::uint8_t>(side * side * 4U, 255U)};
    }

    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return false;
    }
};

struct BackendLog
{
    int created = 0;
    int destroyed = 0;
};

constexpr std::uint32_t FIRST_TEXTURE = 77;

[[nodiscard]] UI::IconTextureBackend fakeBackend(const std::shared_ptr<BackendLog>& log)
{
    return UI::IconTextureBackend{
        .create = [log](int /*width*/, int /*height*/, std::span<const std::uint8_t> /*rgba*/) -> std::uint32_t
        { return FIRST_TEXTURE + static_cast<std::uint32_t>(log->created++); },
        .destroy = [log](std::uint32_t /*texture*/) { ++log->destroyed; },
    };
}

[[nodiscard]] Domain::ProcessSnapshot process(std::int32_t pid)
{
    Domain::ProcessSnapshot proc;
    proc.pid = pid;
    proc.uniqueKey = static_cast<std::uint64_t>(pid) * 1000U;
    proc.name = pid == 1 ? "withicon" : "without";
    return proc;
}

class ProcessIconViewTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        m_View.reset();
        ImGui::DestroyContext(m_Context);
    }

    void makeView(bool withSource)
    {
        m_View = std::make_unique<ProcessIconView>(fakeBackend(m_Log), Domain::ProcessIconCacheConfig{.startWorker = false});
        m_View->attach(withSource ? std::make_unique<ViewFakeIconSource>() : nullptr);
    }

    /// One frame drawing @p proc's icon and name; returns how far the name was pushed right.
    float frame(const Domain::ProcessSnapshot& proc)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(400.0F, 300.0F));
        ImGui::Begin("Icons", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        m_View->beginFrame();
        m_LineHeight = ImGui::GetTextLineHeight();
        const float startX = ImGui::GetCursorScreenPos().x;
        m_View->drawInline(proc);
        const float nameX = ImGui::GetCursorScreenPos().x;
        ImGui::TextUnformatted(proc.name.c_str());
        ImGui::End();
        ImGui::Render();
        return nameX - startX;
    }

    /// One frame drawing the icon of the file at @p path and its name, as the Startup and Services
    /// tabs do (#1800); returns how far the name was pushed right.
    float frameFile(std::string_view path, const char* glyph = ProcessIconView::GENERIC_GLYPH)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(400.0F, 300.0F));
        ImGui::Begin("Icons", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        m_View->beginFrame();
        const float startX = ImGui::GetCursorScreenPos().x;
        m_View->drawFileInline(Domain::iconFileKey(path), path, "entry", glyph);
        const float nameX = ImGui::GetCursorScreenPos().x;
        ImGui::TextUnformatted("entry");
        ImGui::End();
        ImGui::Render();
        return nameX - startX;
    }

    /// Whether this frame's draw data drew texture @p texture.
    [[nodiscard]] static bool drew(std::uint32_t texture)
    {
        const ImDrawData* data = ImGui::GetDrawData();
        for (const ImDrawList* list : data->CmdLists)
        {
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.TexRef._TexData == nullptr && cmd.TexRef._TexID == static_cast<ImTextureID>(texture))
                {
                    return true;
                }
            }
        }
        return false;
    }

    std::shared_ptr<BackendLog> m_Log = std::make_shared<BackendLog>();
    std::unique_ptr<ProcessIconView> m_View;
    float m_LineHeight = 0.0F;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessIconViewTest, TheGenericIconHoldsTheSlotUntilTheProgramsIconLoads)
{
    makeView(true);
    const Domain::ProcessSnapshot proc = process(1);
    const float offset = frame(proc); // Queued: the generic glyph
    EXPECT_GT(offset, m_View->iconSize());
    EXPECT_EQ(m_Log->created, 0);

    ASSERT_NE(m_View->cache(), nullptr);
    EXPECT_EQ(m_View->cache()->loadPending(), 1U);
    EXPECT_FLOAT_EQ(frame(proc), offset); // Names stay where they were
    EXPECT_EQ(m_Log->created, 1);
    EXPECT_TRUE(drew(FIRST_TEXTURE));

    static_cast<void>(frame(proc));
    EXPECT_EQ(m_Log->created, 1); // Made once
    EXPECT_TRUE(drew(FIRST_TEXTURE));
}

TEST_F(ProcessIconViewTest, AProgramWithoutAnIconKeepsTheGenericOne)
{
    makeView(true);
    const Domain::ProcessSnapshot proc = process(2);
    static_cast<void>(frame(proc));
    static_cast<void>(m_View->cache()->loadPending());
    EXPECT_GT(frame(proc), 0.0F);
    EXPECT_EQ(m_Log->created, 0);
    EXPECT_EQ(m_View->textureFor(proc), ImTextureID_Invalid);
}

TEST_F(ProcessIconViewTest, WithoutASourceEveryProcessShowsTheGenericIcon)
{
    makeView(false);
    EXPECT_EQ(m_View->cache(), nullptr);
    EXPECT_GT(frame(process(1)), 0.0F);
    EXPECT_EQ(m_Log->created, 0);
}

TEST_F(ProcessIconViewTest, OffItDrawsAndLoadsNothing)
{
    makeView(true);
    m_View->setEnabled(false);
    const Domain::ProcessSnapshot proc = process(1);
    EXPECT_FLOAT_EQ(frame(proc), 0.0F);
    EXPECT_EQ(m_View->textureFor(proc), ImTextureID_Invalid);
    EXPECT_EQ(m_View->cache()->stats().queued, 0U);
}

TEST_F(ProcessIconViewTest, DetachDeletesEveryTexture)
{
    makeView(true);
    const Domain::ProcessSnapshot proc = process(1);
    static_cast<void>(frame(proc));
    static_cast<void>(m_View->cache()->loadPending());
    static_cast<void>(frame(proc));
    ASSERT_EQ(m_Log->created, 1);
    m_View->detach();
    EXPECT_EQ(m_Log->destroyed, 1);
    EXPECT_EQ(m_View->cache(), nullptr);
}

// #1800: the Startup and Services tabs' icons, by file path.

TEST_F(ProcessIconViewTest, AFilesIconLoadsByPathBehindTheGenericGlyph)
{
    makeView(true);
    const float offset = frameFile("/bin/withicon", ProcessIconView::SERVICE_GLYPH); // Queued: the glyph
    EXPECT_GT(offset, m_View->iconSize());
    EXPECT_EQ(m_Log->created, 0);

    EXPECT_EQ(m_View->cache()->loadPending(), 1U);
    EXPECT_FLOAT_EQ(frameFile("/bin/withicon", ProcessIconView::SERVICE_GLYPH), offset);
    EXPECT_EQ(m_Log->created, 1);
    EXPECT_TRUE(drew(FIRST_TEXTURE));
    EXPECT_EQ(m_View->cache()->stats().files, 1U);
}

TEST_F(ProcessIconViewTest, AFileAndAProcessOfOneProgramShareATexture)
{
    makeView(true);
    static_cast<void>(frame(process(1)));          // Runs /bin/withicon
    static_cast<void>(frameFile("/bin/withicon")); // The same program, by path
    static_cast<void>(m_View->cache()->loadPending());
    static_cast<void>(frame(process(1)));
    static_cast<void>(frameFile("/bin/withicon"));
    EXPECT_EQ(m_Log->created, 1) << "one icon, one texture";
    EXPECT_TRUE(drew(FIRST_TEXTURE));
}

TEST_F(ProcessIconViewTest, AFileWithoutAnIconOrAPathKeepsTheGenericGlyph)
{
    makeView(true);
    static_cast<void>(frameFile("/bin/without"));
    static_cast<void>(m_View->cache()->loadPending());
    EXPECT_GT(frameFile("/bin/without"), m_View->iconSize());
    EXPECT_EQ(m_Log->created, 0);

    EXPECT_GT(frameFile("", ProcessIconView::SERVICE_GLYPH), m_View->iconSize()); // svchost's, or unresolved
    EXPECT_EQ(m_View->cache()->stats().queued, 0U) << "an empty path is never looked up";
    EXPECT_EQ(m_View->textureForFile(Domain::iconFileKey(""), "", "entry"), ImTextureID_Invalid);
}

TEST_F(ProcessIconViewTest, OffAFileDrawsAndLoadsNothing)
{
    makeView(true);
    m_View->setEnabled(false);
    EXPECT_FLOAT_EQ(frameFile("/bin/withicon"), 0.0F);
    EXPECT_EQ(m_View->cache()->stats().queued, 0U);
}

TEST_F(ProcessIconViewTest, TheIconIsTheTextsLineHeight)
{
    makeView(true);
    static_cast<void>(frame(process(1)));
    EXPECT_FLOAT_EQ(m_View->iconSize(), std::round(m_LineHeight));
}

} // namespace
} // namespace App
