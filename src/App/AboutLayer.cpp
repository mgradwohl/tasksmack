#include "App/AboutLayer.h"

#include "App/AboutDialog.h"
#include "App/BackgroundOpen.h"
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
    // A finished open frees the links; a failure is in the log, as the opener writes it.
    static_cast<void>(m_Open.poll());
    if (const std::string_view url = AboutDialog::render(m_OpenRequested, m_CloseRequested, m_Icon, m_Open.busy()); !url.empty())
    {
        // ShellExecuteW or xdg-open, on the worker: never on the UI thread (#3155).
        static_cast<void>(m_Open.start(BackgroundOpen::openUtf8(std::string(url))));
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
    // TASKSMACK_DEMO's close=dialog (#2827): closes the dialog as OK does, on the next render.
    dispatcher.dispatch<Core::CloseDialogEvent>(
        [this](Core::CloseDialogEvent&)
        {
            m_CloseRequested = true;
            return false; // The Help window closes on it too
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
