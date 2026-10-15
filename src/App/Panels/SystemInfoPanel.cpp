#include "SystemInfoPanel.h"

#include "App/Panel.h"
#include "App/Panels/SystemInfoSections.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/GraphicsHostInfo.h"
#include "Core/SaveFileDialog.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/Factory.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ThreadName.h"
#include "UI/Format.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{

namespace
{
constexpr const char* TAB_EVENT_NAME = "SystemInfo";
// How often the sensors are read again while the tab is drawn, so their ranges and histories (#1522)
// follow them.
constexpr std::chrono::milliseconds SENSOR_REFRESH{Domain::Sampling::SYSTEM_INFO_SENSOR_REFRESH_MS};
} // namespace

SystemInfoPanel::SystemInfoPanel() : Panel("System Information")
{}

SystemInfoPanel::SystemInfoPanel(std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeProbe,
                                 std::function<Core::GraphicsHostInfo()> queryHost)
    : Panel("System Information"), m_MakeProbe(std::move(makeProbe)), m_QueryHost(std::move(queryHost))
{}

// A read still in flight is waited for by m_Pending's destructor (a std::async future); it holds its
// own reference to the model.
SystemInfoPanel::~SystemInfoPanel() = default;

void SystemInfoPanel::onAttach()
{
    // The composition root's one probe creation for this panel; the first read waits for the tab.
    // An injected factory (tests, #1395) replaces the platform's.
    m_Model = std::make_shared<Domain::SystemInfoModel>(m_MakeProbe ? m_MakeProbe() : Platform::makeSystemInfoProbe(), m_CrashHistory);
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

void SystemInfoPanel::startRead(bool sensorsOnly)
{
    if (m_Pending.valid() || !m_Model || !m_Model->capabilities().hasOs)
    {
        return;
    }
    try
    {
        m_Pending = std::async(std::launch::async,
                               [model = m_Model, sensorsOnly]
                               {
                                   // Best effort, as for the Connections read: a pooled thread may keep the name.
                                   static_cast<void>(Platform::setCurrentThreadName(Platform::SYSTEM_INFO_READ_THREAD_NAME));
                                   // A failed read comes back as its message, copied here on the worker: no
                                   // exception object crosses to the UI thread (#1685, #1706).
                                   return sensorsOnly ? model->tryReadSensors() : model->tryRead();
                               });
        m_PendingSensorsOnly = sensorsOnly;
        m_LastReadStart = std::chrono::steady_clock::now();
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

void SystemInfoPanel::startSaveReport()
{
    if (m_ViewState.saveDialogOpen || !m_Snapshot || m_Snapshot->version == 0)
    {
        return;
    }
    m_SaveShowIdentifiers = m_ViewState.showIdentifiers;
    const std::string date = UI::Format::formatEpochDateTime(m_Snapshot->readAtUnixSeconds).substr(0, 10);
    const std::string name =
        SystemInfo::reportFileName(m_Snapshot->os.computerName, m_SaveShowIdentifiers, date, SystemInfo::ReportFormat::Text);
    std::vector<Core::FileFilter> filters{{.name = "Text", .pattern = "txt"}, {.name = "JSON", .pattern = "json"}};
    // The callback holds the slot, not the panel: it may run on SDL's thread, or after the panel is gone.
    Core::SaveFileCallback deliver = [slot = std::weak_ptr<SaveSlot>(m_SaveSlot)](Core::SaveFileResult answer)
    {
        if (const std::shared_ptr<SaveSlot> target = slot.lock())
        {
            const std::scoped_lock lock(target->mutex);
            target->result = std::move(answer);
        }
    };
    m_ViewState.saveDialogOpen = true;
    m_ViewState.saveNote.clear();
    if (m_SaveFileDialog)
    {
        m_SaveFileDialog(std::move(filters), name, std::move(deliver));
        return;
    }
    if (!Core::Application::get().hasWindow())
    {
        m_ViewState.saveDialogOpen = false;
        m_ViewState.saveNote = "Couldn't save: there is no window to show a save dialog";
        return;
    }
    Core::showSaveFileDialog(
        Core::Application::get().getWindow().getHandle(), std::move(filters), Core::defaultSaveFolder() + name, std::move(deliver));
}

void SystemInfoPanel::finishSaveReport()
{
    std::optional<Core::SaveFileResult> answer;
    {
        const std::scoped_lock lock(m_SaveSlot->mutex);
        answer.swap(m_SaveSlot->result);
    }
    if (!answer.has_value())
    {
        return;
    }
    m_ViewState.saveDialogOpen = false;
    if (answer->path.empty())
    {
        if (!answer->error.empty())
        {
            m_ViewState.saveNote = "Couldn't show the save dialog: " + answer->error;
        }
        return; // cancelled
    }
    const SystemInfo::ReportFormat format = SystemInfo::reportFormatFor(answer->path, answer->filter);
    const std::string path = SystemInfo::withReportExtension(answer->path, format);
    // The page as it shows now: the newest read's sections, identifiers as they were at the click.
    const std::string contents = format == SystemInfo::ReportFormat::Json
                                   ? SystemInfo::allSectionsJson(m_ViewState.sections, m_SaveShowIdentifiers, m_ViewState.readAtText)
                                   : SystemInfo::allSectionsText(m_ViewState.sections, m_SaveShowIdentifiers);
    std::ofstream file(Core::utf8ToPath(path), std::ios::binary | std::ios::trunc);
    if (file)
    {
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.close();
    }
    if (!file)
    {
        const int error = errno;
        m_ViewState.saveNote =
            std::format("Couldn't save {}: {}", path, error != 0 ? std::generic_category().message(error) : "write failed");
        spdlog::warn("System Information: {}", m_ViewState.saveNote);
        return;
    }
    m_ViewState.saveNote = "Saved to " + path;
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
            // A headless Application (the PGO training driver) has no window to ask: no facts (#2135).
            if (m_QueryHost)
            {
                m_ViewState.host = m_QueryHost();
            }
            else
            {
                const Core::Application& app = Core::Application::get();
                m_ViewState.host = app.hasWindow() ? app.getWindow().queryGraphicsHostInfo() : Core::GraphicsHostInfo{};
            }
        }
    }
    // While the tab is drawn, the sensors are read again every SENSOR_REFRESH so their ranges follow them
    // (#1522); not where there are none to read, and never over another read.
    if (!m_Pending.valid() && m_Snapshot->version != 0 && !m_Snapshot->sensors.devices.empty() &&
        std::chrono::steady_clock::now() - m_LastReadStart >= SENSOR_REFRESH)
    {
        startRead(/*sensorsOnly=*/true);
    }
    // A sensors-only read is quiet: the page doesn't show "reading" for it.
    const bool reading = m_Pending.valid() && !m_PendingSensorsOnly;
    finishSaveReport(); // before drawing, so its note shows this frame
    const SystemInfoViewResult result = renderSystemInfoView(m_Snapshot.get(), m_Model->capabilities(), reading, m_ViewState);
    if (result.refreshRequested)
    {
        startRead();
    }
    if (result.saveReportRequested)
    {
        startSaveReport();
    }
    if (result.wifiDetailsRequested)
    {
        // Every read from now on asks for the Wi-Fi details, which can raise a Windows location prompt
        // (#1782); this one starts as soon as no other read is in flight (a quiet sensors read may be).
        m_Model->includeWifiDetails();
        m_OptInReadDue = true;
    }
    if (result.publicIpRequested)
    {
        // Likewise for the public addresses, which ask a resolver off the machine (#1518).
        m_Model->includePublicIp();
        m_OptInReadDue = true;
    }
    if (m_OptInReadDue && !m_Pending.valid())
    {
        startRead(); // if it can't start, Refresh still reads them: the model keeps the choice
        m_OptInReadDue = false;
    }
}

} // namespace App
