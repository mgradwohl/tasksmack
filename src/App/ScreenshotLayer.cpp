#include "App/ScreenshotLayer.h"

#include "App/ScreenshotOverride.h"
#include "App/ShellLayer.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/FramebufferCapture.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace App
{

namespace
{

/// @p path made absolute for a log line, or as given when that fails.
[[nodiscard]] std::filesystem::path absoluteForLog(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

} // namespace

ScreenshotLayer::ScreenshotLayer(ScreenshotOverride::Request request, const ShellLayer* shell)
    : Core::Layer("ScreenshotLayer"), m_Request(std::move(request)), m_Shell(shell)
{}

void ScreenshotLayer::onAttach()
{
    m_StartTime = Core::Application::getTime();
}

void ScreenshotLayer::onRender()
{
    // A sequence needs frames at its rate: idling, the loop renders at 20 fps (Application.cpp), so
    // from the first capture on it is asked for as a chart asks for its motion. One capture takes
    // whatever frame comes.
    if (m_Outcome == Outcome::Pending && m_Settled && m_Request.frames > 1)
    {
        Core::AnimationRequest::request(static_cast<double>(m_Request.fps));
    }
}

void ScreenshotLayer::onPostRender()
{
    if (m_Outcome != Outcome::Pending)
    {
        return;
    }
    const double now = Core::Application::getTime();
    if (!m_Settled)
    {
        if (now - m_StartTime < static_cast<double>(m_Request.afterSeconds))
        {
            return;
        }
        if (m_Shell != nullptr && m_Shell->startupSelectionPending())
        {
            return;
        }
        m_Settled = true;
    }
    // This frame drew with every startup selection in place; SETTLE_FRAMES more let ImGui show them.
    if (m_SettledFrames < SETTLE_FRAMES)
    {
        ++m_SettledFrames;
        return;
    }
    // A sequence's captures are scheduled from the first one, every 1/fps s on the app clock: a frame
    // that renders late is captured as it is, and the next capture keeps its scheduled time, so the
    // sequence never drifts; a loop slower than fps captures every frame it renders.
    if (m_FramesWritten == 0)
    {
        m_FirstCaptureTime = now;
    }
    else if (now < m_FirstCaptureTime + (static_cast<double>(m_FramesWritten) / static_cast<double>(m_Request.fps)))
    {
        return;
    }
    if (!captureFrame())
    {
        finish(Outcome::Failed);
        return;
    }
    if (m_FramesWritten < m_Request.frames)
    {
        return;
    }
    const auto [width, height] = m_CapturedSize;
    if (m_Request.frames == 1)
    {
        spdlog::info("Screenshot written: {} ({}x{})", Core::pathToUtf8(absoluteForLog(m_Request.path)), width, height);
    }
    else
    {
        spdlog::info("Screenshots written: {} frames at {} fps -> {} ({}x{})",
                     m_Request.frames,
                     m_Request.fps,
                     ScreenshotOverride::sequencePattern(absoluteForLog(m_Request.path)),
                     width,
                     height);
    }
    finish(Outcome::Written);
}

void ScreenshotLayer::onDetach()
{
    // The app closed before the capture (the user closed the window, or a shutdown): the hook's run
    // produced nothing, which main.cpp reports in the exit code.
    if (m_Outcome == Outcome::Pending)
    {
        m_Outcome = Outcome::Failed;
        spdlog::error("{}: no screenshot was written; TaskSmack closed first", ScreenshotOverride::ENV_VAR);
    }
}

bool ScreenshotLayer::captureFrame()
{
    const std::filesystem::path file =
        m_Request.frames == 1 ? m_Request.path : ScreenshotOverride::frameFileName(m_Request.path, m_FramesWritten + 1);
    const auto [width, height] = Core::Application::get().getWindow().getSizeInPixels();
    if (const auto read = Core::FramebufferCapture::readDefaultFramebuffer(width, height, m_Pixels); !read.has_value())
    {
        spdlog::error("{}: {} not written: {}", ScreenshotOverride::ENV_VAR, Core::pathToUtf8(absoluteForLog(file)), read.error());
        return false;
    }
    if (const auto written = Core::FramebufferCapture::writePng(file, width, height, m_Pixels); !written.has_value())
    {
        spdlog::error("{}: {}", ScreenshotOverride::ENV_VAR, written.error());
        return false;
    }
    m_CapturedSize = {width, height};
    ++m_FramesWritten;
    return true;
}

void ScreenshotLayer::finish(const Outcome outcome)
{
    m_Outcome = outcome;
    // The window's close request (#1077): Application::run() raises WindowCloseEvent after the next
    // event drain and stops, and every layer's onDetach() runs, ShellLayer's saving config.toml -- as
    // the title bar's Close button does.
    Core::Application::get().getWindow().requestClose();
}

} // namespace App
