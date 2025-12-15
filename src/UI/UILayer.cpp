#include "UILayer.h"

#include "Core/Application.h"

#include <spdlog/spdlog.h>

// clang-format off
// GLFW_INCLUDE_NONE must be defined before including GLFW to prevent GL header conflicts with glad
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
// clang-format on
#include <imgui.h>
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

    // Load Inter font (TTF) at multiple sizes for flexibility
    // Font sizes in typographic points (1pt = 1/72 inch)
    constexpr float FONT_SIZE_REGULAR_PT = 8.0F; // standard body text
    constexpr float FONT_SIZE_LARGE_PT = 10.0F;  // headings

    const float fontSizeRegular = pointsToPixels(FONT_SIZE_REGULAR_PT);
    const float fontSizeLarge = pointsToPixels(FONT_SIZE_LARGE_PT);

    spdlog::info("Font sizes: {}pt = {:.1f}px, {}pt = {:.1f}px", FONT_SIZE_REGULAR_PT, fontSizeRegular, FONT_SIZE_LARGE_PT, fontSizeLarge);

    // Build font path relative to executable directory
    auto exeDir = getExecutableDir();
    auto fontPath = (exeDir / "assets" / "fonts" / "Inter-Regular.ttf").string();
    auto fontBoldPath = (exeDir / "assets" / "fonts" / "Inter-Bold.ttf").string();

    spdlog::info("Loading fonts from: {}", exeDir.string());

    // Try to load Inter font from assets directory
    ImFont* fontRegular = imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeRegular);
    if (fontRegular != nullptr)
    {
        spdlog::info("Loaded Inter-Regular.ttf at {:.1f}px ({}pt)", fontSizeRegular, FONT_SIZE_REGULAR_PT);
        // Also load bold variant
        ImFont* fontBold = imguiIO.Fonts->AddFontFromFileTTF(fontBoldPath.c_str(), fontSizeRegular);
        if (fontBold != nullptr)
        {
            spdlog::info("Loaded Inter-Bold.ttf at {:.1f}px ({}pt)", fontSizeRegular, FONT_SIZE_REGULAR_PT);
        }
        // Load a larger size variant
        imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeLarge);
    }
    else
    {
        spdlog::warn("Could not load Inter font from {}, falling back to default", fontPath);
        ImFontConfig fontConfig;
        fontConfig.SizePixels = fontSizeRegular;
        imguiIO.Fonts->AddFontDefault(&fontConfig);
    }

    // Style
    ImGui::StyleColorsLight();

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
    // UI logic updates can go here
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

    // Clear screen with ImGui's window background color (follows theme)
    const ImVec4& bgColor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    glClearColor(bgColor.x, bgColor.y, bgColor.z, bgColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
}

void UILayer::endFrame()
{
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
