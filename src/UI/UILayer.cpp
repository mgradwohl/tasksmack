#include "UILayer.h"

#include "Core/Application.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

// clang-format off
// GLFW_INCLUDE_NONE must be defined before including GLFW to prevent GL header conflicts with glad
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
// clang-format on
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <filesystem>

#ifdef __linux__
// No additional includes needed - std::filesystem handles it
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace
{
// Get directory containing the executable
std::filesystem::path getExecutableDir()
{
#ifdef __linux__
    // C++17: read_symlink handles /proc/self/exe directly
    std::error_code errorCode;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", errorCode);
    if (!errorCode)
    {
        return exePath.parent_path();
    }
#elif defined(_WIN32)
    // Use wide string API and let filesystem handle conversion
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size())
    {
        buffer.resize(len);
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

// Convert typographic points to pixels based on display DPI
// Standard: 1 point = 1/72 inch, base DPI assumed 96 (Windows/Linux standard)
float pointsToPixels(float points)
{
    constexpr float BASE_DPI = 96.0F;
    float scaleX = 1.0F;
    float scaleY = 1.0F;

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor != nullptr)
    {
        glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
    }

    // pixels = points * (DPI / 72), where effective DPI = BASE_DPI * scale
    return points * (BASE_DPI * scaleX) / 72.0F;
}
} // namespace

namespace UI
{

UILayer::UILayer() : Layer("UILayer")
{
}

UILayer::~UILayer() = default;

void UILayer::loadAllFonts()
{
    auto& theme = Theme::get();
    ImGuiIO& imguiIO = ImGui::GetIO();

    // Configure FreeType for better hinting at small sizes
    // Note: IMGUI_ENABLE_FREETYPE is defined at compile time, so FreeType is always used
    // LightHinting provides better quality for UI fonts at typical screen sizes
    imguiIO.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;

    // Build font path relative to executable directory
    auto exeDir = getExecutableDir();
    auto fontPath = (exeDir / "assets" / "fonts" / "Inter-Regular.ttf").string();

    spdlog::info("Pre-baking fonts for all {} size presets with FreeType renderer", FONT_SIZE_COUNT);

    // Load fonts for all size presets into a single atlas
    for (const auto size : ALL_FONT_SIZES)
    {
        const auto& fontCfg = theme.fontConfig(size);

        const float fontSizeRegular = pointsToPixels(fontCfg.regularPt);
        const float fontSizeLarge = pointsToPixels(fontCfg.largePt);

        spdlog::debug("Loading {} fonts: {}pt = {:.1f}px, {}pt = {:.1f}px",
                      fontCfg.name,
                      fontCfg.regularPt,
                      fontSizeRegular,
                      fontCfg.largePt,
                      fontSizeLarge);

        ImFont* fontRegular = imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeRegular);
        if (fontRegular == nullptr)
        {
            spdlog::warn("Could not load Inter font from {}, using default", fontPath);
            ImFontConfig defaultFontConfig;
            defaultFontConfig.SizePixels = fontSizeRegular;
            fontRegular = imguiIO.Fonts->AddFontDefault(&defaultFontConfig);
        }

        ImFont* fontLarge = imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeLarge);
        if (fontLarge == nullptr)
        {
            ImFontConfig defaultFontConfig;
            defaultFontConfig.SizePixels = fontSizeLarge;
            fontLarge = imguiIO.Fonts->AddFontDefault(&defaultFontConfig);
        }

        // Register with theme for instant switching
        theme.registerFonts(size, fontRegular, fontLarge);
    }

    spdlog::info("Pre-baked {} fonts into atlas using FreeType", imguiIO.Fonts->Fonts.Size);
}

void UILayer::onAttach()
{
    spdlog::info("Initializing ImGui");

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& imguiIO = ImGui::GetIO();
    imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Multi-viewport (optional)

    // Pre-bake fonts for all size presets
    loadAllFonts();

    // Load themes from TOML files
    auto themesDir = getExecutableDir() / "assets" / "themes";
    Theme::get().loadThemes(themesDir);

    // Style - start with dark base, then apply theme colors
    ImGui::StyleColorsDark();
    Theme::get().applyImGuiStyle();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones
    ImGuiStyle& style = ImGui::GetStyle();
    if ((imguiIO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
    {
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }

    // Setup Platform/Renderer backends
    GLFWwindow* window = Core::Application::get().getWindow().getHandle();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    spdlog::info("ImGui initialized successfully");
}

void UILayer::onDetach()
{
    spdlog::info("Shutting down ImGui");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

void UILayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No font rebuild needed - fonts are pre-baked at all sizes
}

void UILayer::onRender()
{
    beginFrame();

    // Demo windows are now controlled via View menu in ShellLayer
    // UILayer just initializes ImGui frame - actual UI is in other layers
}

void UILayer::onPostRender()
{
    endFrame();
}

void UILayer::beginFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Push the current font size - this applies to all ImGui rendering this frame
    ImFont* font = Theme::get().regularFont();
    if (font != nullptr)
    {
        ImGui::PushFont(font);
    }

    // Clear screen with ImGui's window background color (follows theme)
    const ImVec4& bgColor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    glClearColor(bgColor.x, bgColor.y, bgColor.z, bgColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
}

void UILayer::endFrame()
{
    // Pop the font we pushed in beginFrame()
    ImFont* font = Theme::get().regularFont();
    if (font != nullptr)
    {
        ImGui::PopFont();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Handle multi-viewport
    ImGuiIO& imguiIO = ImGui::GetIO();
    if ((imguiIO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
    {
        GLFWwindow* backupCurrentContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupCurrentContext);
    }
}

} // namespace UI
