#include "SystemInfoPanel.h"

#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/SystemInfoSections.h"
#include "App/Panels/SystemInfoView.h"
#include "App/SyntheticScenario.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/GraphicsHostInfo.h"
#include "Core/SaveFileDialog.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ThreadName.h"
#include "UI/Format.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{

namespace
{
constexpr const char* TAB_EVENT_NAME = "SystemInfo";
// How many numbered names ("report (2).txt", ...) are tried before giving up on a free one (#2144).
constexpr int MAX_NUMBERED_REPORT_NAMES = 999;

[[nodiscard]] bool pathExists(const std::string& utf8)
{
    std::error_code error;
    return std::filesystem::exists(Core::utf8ToPath(utf8), error) || error;
}

/// Where to write the report the dialog answered @p chosen for, as @p path (with its extension added):
/// @p path itself, unless adding the extension named a file that already exists. The dialog asked
/// about replacing @p chosen, not that one, so a free numbered name is used instead; empty when there is none.
[[nodiscard]] std::string freeReportPath(const std::string& chosen, const std::string& path)
{
    if (path == chosen || !pathExists(path))
    {
        return path;
    }
    for (int number = 2; number <= MAX_NUMBERED_REPORT_NAMES; ++number)
    {
        std::string numbered = SystemInfo::numberedReportPath(path, number);
        if (!pathExists(numbered))
        {
            return numbered;
        }
    }
    return {};
}

/// Writes @p contents to the file @p path, replacing it: nullopt once written, else why it couldn't be.
[[nodiscard]] std::optional<std::string> writeReportFile(const std::string& path, const std::string& contents)
{
    std::ofstream file(Core::utf8ToPath(path), std::ios::binary | std::ios::trunc);
    if (file)
    {
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.close();
    }
    if (!file)
    {
        const int error = errno; // this thread's: the worker's own open, write or close
        return error != 0 ? std::generic_category().message(error) : std::string("write failed");
    }
    return std::nullopt;
}

/// The report-save worker's job (#2986): where to write the report the dialog answered @p chosen for
/// (freeReportPath()), the write through @p write, and the note that says how it went.
[[nodiscard]] std::string saveReport(const std::string& chosen,
                                     const std::string& withExtension,
                                     const std::string& contents,
                                     const SystemInfoPanel::ReportWriter& write)
{
    const std::string path = freeReportPath(chosen, withExtension);
    if (path.empty())
    {
        return std::format("Couldn't save: {} already exists", withExtension);
    }
    if (const std::optional<std::string> failed = write(path, contents))
    {
        std::string note = std::format("Couldn't save {}: {}", path, *failed);
        spdlog::warn("System Information: {}", note);
        return note;
    }
    return path == withExtension ? "Saved to " + path : std::format("Saved to {} ({} already exists)", path, withExtension);
}
} // namespace

SystemInfoPanel::SystemInfoPanel() : Panel("System Information")
{}

SystemInfoPanel::SystemInfoPanel(std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeProbe,
                                 std::function<Core::GraphicsHostInfo()> queryHost)
    : Panel("System Information"), m_MakeProbe(std::move(makeProbe)), m_QueryHost(std::move(queryHost))
{}

// A read still in flight is waited for by m_Pending's destructor (a BackgroundTask); it holds its
// own reference to the model. Likewise a report still being written, by m_SaveWrite's: it owns copies
// of everything it uses.
SystemInfoPanel::~SystemInfoPanel() = default;

void SystemInfoPanel::onAttach()
{
    // The composition root's one probe creation for this panel; the first read waits for the tab.
    // An injected factory (tests, #1395) replaces the platform's.
    m_Model = std::make_shared<Domain::SystemInfoModel>(m_MakeProbe ? m_MakeProbe() : makeSystemInfoPanelProbe(Synthetic::activeScenario()),
                                                        m_CrashHistory);

    if (!m_Model->capabilities().hasOs)
    {
        return; // the page shows only that it's unsupported: no sensors to draw
    }
    // The sensors' own probe, built by the sampler's first pass on its own thread (#2731, #2737).
    std::function<std::unique_ptr<Platform::ISystemInfoProbe>()> makeSensorProbe = m_MakeProbe;
    if (!makeSensorProbe)
    {
        makeSensorProbe = [scenario = Synthetic::activeScenario()]
        {
            return App::makeSensorProbe(scenario);
        };
    }
    m_Sensors = std::make_shared<Domain::SensorModel>(
        [makeSensorProbe = std::move(makeSensorProbe)]() -> Domain::SensorModel::Reader
        {
            std::shared_ptr<Platform::ISystemInfoProbe> probe = makeSensorProbe();
            if (!probe)
            {
                return {};
            }
            return [probe = std::move(probe)]
            {
                return probe->readSensors();
            };
        },
        m_RefreshInterval);
    m_Sensors->setHistorySeconds(m_HistorySeconds);
    m_SensorSampler = std::make_unique<Domain::BackgroundSampler>(Domain::SamplerConfig{
        .interval = m_RefreshInterval, .firstSampleAfterInterval = false, .threadName = std::string(Platform::SENSOR_SAMPLER_THREAD_NAME)});
    m_SensorSampler->addSamplable(m_Sensors, "sensors");
    m_SensorSampler->start();
}

void SystemInfoPanel::onDetach()
{
    m_Pending.requestStop(); // a password prompt still up is given up on, not waited out (#2381)
    finishRead(true);        // the one place a read is waited for
    m_SaveWrite.wait();      // and a report being written (#2986); its note is dropped with the page
    static_cast<void>(m_SaveWrite.poll());
    m_Snapshot.reset();
    m_Model.reset();
    if (m_SensorSampler)
    {
        m_SensorSampler->stop();
        m_SensorSampler.reset();
    }
    m_SensorPublication.reset();
    m_Sensors.reset();
    m_ViewState = {}; // a later attach's models count their versions from 1 again
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
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            // The sensors follow the Settings interval, as every metric does (#2737).
            m_RefreshInterval = std::chrono::milliseconds(e.getIntervalMs());
            if (m_SensorSampler)
            {
                m_SensorSampler->setInterval(m_RefreshInterval);
            }
            if (m_Sensors)
            {
                m_Sensors->setInterval(m_RefreshInterval);
            }
            return false;
        });
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            // The sensors' charts span the Settings history duration, as every metric's do (#2796),
            // clamped as the models clamp it.
            m_HistorySeconds = Domain::Sampling::clampHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            if (m_Sensors)
            {
                m_Sensors->setHistorySeconds(m_HistorySeconds);
            }
            return false;
        });
}

void SystemInfoPanel::startRead()
{
    if (m_Pending.busy() || !m_Model || !m_Model->capabilities().hasOs)
    {
        return;
    }
    // A failed read comes back as its message, copied on the worker (SystemInfoModel::tryRead()).
    if (const auto failed = m_Pending.start([model = m_Model](const std::stop_token& stop) { return model->tryRead(stop); }))
    {
        spdlog::warn("System Information: couldn't start a read: {}", failed->value_or(std::string()));
        return;
    }
}

void SystemInfoPanel::finishRead(bool wait)
{
    // Only building a failed read's message (bad_alloc) can fail past tryRead(): BackgroundTask makes
    // that "unknown error", never reading an exception thrown on the worker (#1685, #1706).
    if (const auto finished = m_Pending.poll(wait); finished.has_value() && finished->has_value())
    {
        spdlog::warn("System Information: read failed: {}", **finished);
    }
}

void SystemInfoPanel::startSaveReport()
{
    if (m_ViewState.saveDialogOpen || m_SaveWrite.busy() || !m_Snapshot || m_Snapshot->version == 0)
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
    if (answer.has_value())
    {
        if (answer->path.empty())
        {
            m_ViewState.saveDialogOpen = false;
            if (!answer->error.empty())
            {
                m_ViewState.saveNote = "Couldn't show the save dialog: " + answer->error;
            }
            return; // cancelled
        }
        const SystemInfo::ReportFormat format = SystemInfo::reportFormatFor(answer->path, answer->filter);
        std::string withExtension = SystemInfo::withReportExtension(answer->path, format);
        // The page as it shows now: the newest read's sections, identifiers as they were at the click.
        // Formatted here, from what the page already draws; the file system is the worker's (#2986).
        std::string contents = format == SystemInfo::ReportFormat::Json
                                 ? SystemInfo::allSectionsJson(m_ViewState.sections, m_SaveShowIdentifiers, m_ViewState.readAtText)
                                 : SystemInfo::allSectionsText(m_ViewState.sections, m_SaveShowIdentifiers);
        ReportWriter writer = m_ReportWriter ? m_ReportWriter : ReportWriter(&writeReportFile);
        // Save report... stays disabled (saveDialogOpen) until the worker's note is taken in below.
        if (const auto failed =
                m_SaveWrite.start([chosen = std::move(answer->path),
                                   withExtension = std::move(withExtension),
                                   contents = std::move(contents),
                                   writer = std::move(writer)] { return saveReport(chosen, withExtension, contents, writer); }))
        {
            m_ViewState.saveDialogOpen = false;
            m_ViewState.saveNote = *failed; // no thread to write it on
            spdlog::warn("System Information: {}", m_ViewState.saveNote);
            return;
        }
    }
    // The worker's note, once it has finished: polled, never waited for (a write that finishes at once
    // shows this frame).
    if (std::optional<std::string> note = m_SaveWrite.poll())
    {
        m_ViewState.saveDialogOpen = false;
        m_ViewState.saveNote = std::move(*note);
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
    // The sensor sampler's newest read, adopted by pointer when there's a new one (#2737).
    if (m_Sensors && (!m_SensorPublication || m_SensorPublication->version != m_Sensors->version()))
    {
        m_SensorPublication = m_Sensors->publication();
    }
    const bool reading = m_Pending.busy();
    finishSaveReport(); // before drawing, so a finished save's note shows this frame
    const SystemInfoViewResult result =
        renderSystemInfoView(m_Snapshot.get(), m_Model->capabilities(), reading, m_ViewState, m_SensorPublication.get());
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
        // (#1782); this one starts as soon as no other read is in flight.
        m_Model->includeWifiDetails();
        m_OptInReadDue = true;
    }
    if (result.publicIpRequested)
    {
        // Likewise for the public addresses, which ask a resolver off the machine (#1518).
        m_Model->includePublicIp();
        m_OptInReadDue = true;
    }
    if (result.authenticateRequested)
    {
        // The next read asks for the password first, on its own thread, then reads the root-only
        // facts through the helper (#2310)
        m_Model->requestAuthorization();
        m_OptInReadDue = true;
    }
    if (m_OptInReadDue && !m_Pending.busy())
    {
        startRead(); // if it can't start, Refresh still reads them: the model keeps the choice
        m_OptInReadDue = false;
    }
}

} // namespace App
