#include "SystemInfoPanel.h"

#include "App/Panel.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Window.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/Factory.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <system_error>

namespace App
{

namespace
{
constexpr const char* TAB_EVENT_NAME = "SystemInfo";
} // namespace

SystemInfoPanel::SystemInfoPanel() : Panel("System Information")
{}

// A read still in flight is waited for by m_Pending's destructor (a std::async future); it holds its
// own reference to the model.
SystemInfoPanel::~SystemInfoPanel() = default;

void SystemInfoPanel::onAttach()
{
    // The composition root's one probe creation for this panel; the first read waits for the tab.
    m_Model = std::make_shared<Domain::SystemInfoModel>(Platform::makeSystemInfoProbe());
}

void SystemInfoPanel::onDetach()
{
    finishRead(true); // the one place a read is waited for
    m_Snapshot.reset();
    m_Model.reset();
}

void SystemInfoPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            // Static facts: read the first time the tab shows, then only on Refresh.
            if (e.tabName() == TAB_EVENT_NAME && m_Model && m_Model->version() == 0)
            {
                startRead();
            }
            return false;
        });
}

void SystemInfoPanel::startRead()
{
    if (m_Pending.valid() || !m_Model || !m_Model->capabilities().hasOs)
    {
        return;
    }
    try
    {
        m_Pending = std::async(std::launch::async,
                               [model = m_Model]
                               {
                                   // Best effort, as for the Connections read: a pooled thread may keep the name.
                                   static_cast<void>(Platform::setCurrentThreadName(Platform::SYSTEM_INFO_READ_THREAD_NAME));
                                   model->read();
                               });
    }
    catch (const std::system_error& e)
    {
        spdlog::warn("System Information: couldn't start a read: {}", e.what());
    }
}

void SystemInfoPanel::finishRead(bool wait)
{
    if (!m_Pending.valid() || (!wait && m_Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
    {
        return;
    }
    try
    {
        m_Pending.get();
    }
    catch (const std::exception& e)
    {
        spdlog::warn("System Information: read failed: {}", e.what());
    }
}

void SystemInfoPanel::renderContent()
{
    if (!m_Model)
    {
        return;
    }
    finishRead(false);
    if (!m_Snapshot || m_Snapshot->version != m_Model->version())
    {
        m_Snapshot = m_Model->snapshot();
        if (m_Snapshot->version != 0)
        {
            // Core's OpenGL and SDL display facts, on the UI thread that owns them, once per read (#1519).
            m_ViewState.host = Core::Application::get().getWindow().queryGraphicsHostInfo();
        }
    }
    const SystemInfoViewResult result = renderSystemInfoView(m_Snapshot.get(), m_Model->capabilities(), m_Pending.valid(), m_ViewState);
    if (result.refreshRequested)
    {
        startRead();
    }
}

} // namespace App
