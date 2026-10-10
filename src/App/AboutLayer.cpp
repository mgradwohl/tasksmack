#include "App/AboutLayer.h"

#include "App/AboutDialog.h"
#include "App/PlatformOpen.h"
#include "App/SelectOverride.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "UI/AssetPath.h"
#include "UI/IconLoader.h"

#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <string_view>

namespace App
{

AboutLayer::AboutLayer() : Core::Layer("AboutLayer")
{}

AboutLayer::~AboutLayer() = default;

void AboutLayer::onAttach()
{
    loadIcon();
    // TASKSMACK_OPEN=about (#172): open at startup, for an unattended capture.
    if (SelectOverride::startupDialog() == SelectOverride::StartupDialog::About)
    {
        requestOpen();
    }
}

void AboutLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void AboutLayer::onRender()
{
    if (const std::string_view url = AboutDialog::render(m_OpenRequested, m_Icon); !url.empty())
    {
        (void) PlatformOpen::openWithSystemHandler(url);
    }
}

void AboutLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    // Listen for About requests: the title bar's "i" (#1600), Help's "About TaskSmack..." link and
    // Settings (#172)
    dispatcher.dispatch<Core::OpenAboutEvent>(
        [this](Core::OpenAboutEvent&)
        {
            requestOpen();
            return false; // Don't consume
        });
}

void AboutLayer::requestOpen()
{
    m_OpenRequested = true;
}

void AboutLayer::loadIcon()
{
    const auto iconsDir = UI::findAssetsDir() / "icons";

    constexpr std::array<const char*, 2> sizes = {"tasksmack-256.png", "tasksmack-128.png"};

    for (const auto* const file : sizes)
    {
        const auto iconPath = iconsDir / file;

        if (!std::filesystem::exists(iconPath))
        {
            continue;
        }

        m_Icon = UI::loadTexture(iconPath);
        if (m_Icon.valid())
        {
            spdlog::info("Loaded About dialog icon: {} ({}x{})",
                         Core::pathToUtf8(iconPath),
                         static_cast<int>(m_Icon.size().x),
                         static_cast<int>(m_Icon.size().y));
            return;
        }
    }

    spdlog::warn("About dialog icon not found; continuing without image");
}

} // namespace App
