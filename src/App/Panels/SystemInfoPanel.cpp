#include "SystemInfoPanel.h"

#include "App/Panel.h"
#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/GraphicsHostInfo.h"
#include "Core/Window.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/Factory.h"
#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace App
{

namespace
{
constexpr const char* TAB_EVENT_NAME = "SystemInfo";
} // namespace

SystemInfoPanel::SystemInfoPanel() : Panel("System Information")
{}

SystemInfoPanel::SystemInfoPanel(std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeProbe,
                                 std::function<Core::GraphicsHostInfo()> queryHost,
                                 std::function<std::unique_ptr<Platform::ISensorProbe>()> makeSensorProbe)
    : Panel("System Information"),
      m_MakeProbe(std::move(makeProbe)),
      m_QueryHost(std::move(queryHost)),
      m_MakeSensorProbe(std::move(makeSensorProbe)),
      m_TestSeam(true)
{}

// A read still in flight is waited for by m_Pending's destructor (a std::async future); it holds its
// own reference to the model.
SystemInfoPanel::~SystemInfoPanel() = default;

void SystemInfoPanel::onAttach()
{
    // The composition root's one probe creation for this panel; the first read waits for the tab.
    // An injected factory (tests, #1395) replaces the platform's.
    m_Model = std::make_shared<Domain::SystemInfoModel>(m_MakeProbe ? m_MakeProbe() : Platform::makeSystemInfoProbe(), m_CrashHistory);

    // The live sensors (#1522): sampled from now on, so minimum and maximum are since start. The first
    // sample, on the sampler thread, finds the sensors; later ones only read their values.
    std::unique_ptr<Platform::ISensorProbe> sensorProbe;
    if (m_MakeSensorProbe)
    {
        sensorProbe = m_MakeSensorProbe();
    }
    else if (!m_TestSeam)
    {
        sensorProbe = Platform::makeSensorProbe();
    }
    if (sensorProbe && m_Model->capabilities().hasOs)
    {
        m_SensorModel = std::make_shared<Domain::SensorModel>(std::move(sensorProbe));
        m_SensorModel->setMaxHistorySeconds(m_ViewState.historySeconds);
        Domain::SamplerConfig config;
        config.interval = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(m_RefreshInterval, m_IsActiveTab, false);
        config.threadName = Platform::SENSOR_SAMPLER_THREAD_NAME;
        m_SensorSampler = std::make_unique<Domain::BackgroundSampler>(config);
        m_SensorSampler->addSamplable(m_SensorModel, "sensors");
        m_SensorSampler->start();
    }
}

void SystemInfoPanel::onDetach()
{
    m_SensorSampler.reset(); // joins the sampler before its model goes
    m_ViewState.sensors.reset();
    m_SensorModel.reset();
    finishRead(true); // the one place a read is waited for
    m_Snapshot.reset();
    m_Model.reset();
}

void SystemInfoPanel::applySensorInterval()
{
    if (m_SensorSampler)
    {
        m_SensorSampler->setInterval(AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(m_RefreshInterval, m_IsActiveTab, false));
    }
}

void SystemInfoPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            m_IsActiveTab = e.tabName() == TAB_EVENT_NAME;
            applySensorInterval();
            // Static facts: read the first time the tab shows, then only on Refresh.
            if (m_IsActiveTab && m_Model && m_Model->version() == 0)
            {
                startRead();
            }
            return false;
        });
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            m_RefreshInterval = std::chrono::milliseconds(e.getIntervalMs());
            applySensorInterval();
            return false;
        });
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            // Clamped as the model clamps it, so the sparklines span the window it keeps (#1145).
            m_ViewState.historySeconds = Domain::Sampling::clampHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            if (m_SensorModel)
            {
                m_SensorModel->setMaxHistorySeconds(m_ViewState.historySeconds);
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
                                   // A failed read comes back as its message, copied here on the worker: no
                                   // exception object crosses to the UI thread (#1685, #1706).
                                   return model->tryRead();
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
        if (const std::optional<std::string> failure = m_Pending.get())
        {
            spdlog::warn("System Information: read failed: {}", *failure);
        }
    }
    catch (...)
    {
        // The worker turns a throwing read into its message itself (SystemInfoModel::tryRead()), so only
        // building that message can land here (bad_alloc). The exception object was thrown on the worker
        // and is not read here (#1685, #1706).
        spdlog::warn("System Information: read failed: unknown error");
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
            m_ViewState.host = m_QueryHost ? m_QueryHost() : Core::Application::get().getWindow().queryGraphicsHostInfo();
        }
    }
    if (m_SensorModel && (!m_ViewState.sensors || m_ViewState.sensors->version != m_SensorModel->publicationVersion()))
    {
        m_ViewState.sensors = m_SensorModel->publication(); // held, not copied: immutable and shared
    }
    const SystemInfoViewResult result = renderSystemInfoView(m_Snapshot.get(), m_Model->capabilities(), m_Pending.valid(), m_ViewState);
    if (result.refreshRequested)
    {
        startRead();
    }
}

} // namespace App
