#pragma once

#include "Domain/CrashHistory.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemInfoProbe.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Domain
{

/// One immutable read of the System Information page's static facts (#1399). Each section's raw
/// facts are a member; a new section adds one.
/// The lowest and highest one sensor has read since the page's first read (#1522), or since its device
/// was plugged in (#2495).
struct SensorRange
{
    double min = 0.0;
    double max = 0.0;
    std::uint32_t samples = 0; ///< How many reads it was in.

    [[nodiscard]] bool operator==(const SensorRange&) const = default;
};

/// One sensor's recent readings, for its row's chart (#1522), oldest first, `values` aligned with
/// `times`: at most Sampling::SENSOR_HISTORY_CAPACITY of them, none more than
/// Sampling::HISTORY_SECONDS_DEFAULT older than the newest. A NaN value marks a break where the reads
/// paused (the tab was hidden).
struct SensorHistory
{
    std::vector<float> values;
    std::vector<double> times; ///< When each was read, steady_clock seconds (UI::Widgets::historyFrameNowSeconds()'s clock)

    [[nodiscard]] bool operator==(const SensorHistory&) const = default;
};

/// Appends @p value read at @p nowSeconds to @p history: a NaN break first when the reads paused for
/// longer than SENSOR_HISTORY_GAP_INTERVALS refreshes, then the oldest dropped past the capacity or the
/// window. A time not after the newest is ignored, so the history stays in order.
void appendSensorHistory(SensorHistory& history, double value, double nowSeconds);

/// A pause longer than this many sensor refreshes breaks a sensor's history line.
inline constexpr double SENSOR_HISTORY_GAP_INTERVALS = 3.0;

/// The device part of sensorKey(): SensorDevice::id, or its name from a source without ids.
[[nodiscard]] inline std::string_view sensorDeviceKey(const Platform::SensorDevice& device)
{
    return device.id.empty() ? std::string_view(device.name) : std::string_view(device.id);
}

/// A sensor's key in SystemInfoSnapshot::sensorRanges and sensorHistories: its device's and its own
/// identity (SensorDevice::id, SensorReading::id), never its label, so two channels that share a label
/// (two "acpitz" zones, a voltage and a current both "VDD_IN") keep their own range and history (#2340).
/// From a source without ids, the device's name and the reading's kind and label stand in.
[[nodiscard]] inline std::string sensorKey(const Platform::SensorDevice& device, const Platform::SensorReading& reading)
{
    const std::string_view deviceId = sensorDeviceKey(device);
    if (!reading.id.empty())
    {
        return std::format("{}\x1F{}", deviceId, reading.id);
    }
    return std::format("{}\x1F{}\x1F{}", deviceId, std::to_underlying(reading.kind), reading.label);
}

struct SystemInfoSnapshot
{
    std::uint64_t version = 0;           ///< 0 until the first read.
    std::uint64_t readAtUnixSeconds = 0; ///< When the read started (#1773).
    Platform::OsInfo os;
    Platform::FirmwareInfo firmware;
    Platform::MemoryModulesInfo memory;
    Platform::CommitPagingInfo paging;
    Platform::StorageInfo storage;
    Platform::GraphicsInfo graphics;
    Platform::PlatformSecurityInfo security;
    Platform::SensorsInfo sensors;
    Platform::DevicesInfo devices;
    Platform::DriversInfo drivers;
    Platform::CrashesInfo crashes;
    Platform::NetworkAdaptersInfo adapters;
    Platform::BootPerformanceInfo boot;
    /// Each sensor's range over the reads so far (sensorKey()); full reads and readSensors() add to it.
    std::map<std::string, SensorRange> sensorRanges;
    /// Each sensor's recent readings, keyed as sensorRanges; appended to by the same reads (#1522).
    std::map<std::string, SensorHistory> sensorHistories;
    /// This read asked the probe for the Wi-Fi details that can raise a location prompt (#1782).
    bool wifiDetailsIncluded = false;
    /// This read looked the public addresses up (#1518): the user asked, and the probe can.
    bool publicIpIncluded = false;
    /// The public addresses, when publicIpIncluded.
    Platform::PublicIpInfo publicIp;
};

/// Reads the static facts on demand (once when the page first shows, and on Refresh), never per tick,
/// and publishes each read as an immutable SystemInfoSnapshot the UI adopts by pointer.
class SystemInfoModel
{
  public:
    /// @param crashHistory The shared crash cache Process Details shows (#1675), handed each read's
    ///        crashes so it need not read them again; null for none.
    explicit SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe> probe, std::shared_ptr<CrashHistory> crashHistory = nullptr);

    /// Reads every section and publishes the result. Thread-safe; reads are serialised. Called off the
    /// UI thread. @p stopToken cancels a password prompt requestAuthorization() asked for (#2381).
    void read(const std::stop_token& stopToken = {});

    /// read(), for a worker thread: a read that throws is caught here, on the thread that threw it, and
    /// only its message is returned (a copy), so no exception object reaches another thread through a
    /// future (#1685, #1706). @return nullopt when the read succeeded; else the exception's what(), or
    /// "unknown error" for a throw that isn't a std::exception. Only building that message (bad_alloc)
    /// can still escape.
    [[nodiscard]] std::optional<std::string> tryRead(const std::stop_token& stopToken = {});
    /// Reads the sensors again and publishes them with every other fact of the newest read unchanged,
    /// "read at" included, adding to each sensor's range (#1522). Does nothing before the first read() or
    /// when the probe reads nothing. Thread-safe; serialised with read(). Called off the UI thread.
    void readSensors();
    /// readSensors(), for a worker thread, as tryRead() is for read().
    [[nodiscard]] std::optional<std::string> tryReadSensors();

    /// The newest read (an empty version-0 one before the first).
    [[nodiscard]] std::shared_ptr<const SystemInfoSnapshot> snapshot() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// From now on, every read() asks the probe for the Wi-Fi details too, which can raise a Windows
    /// location prompt (#1782): the user clicked "Show Wi-Fi details". The choice holds for the model's
    /// life (the session). Thread-safe; takes effect from the next read() that starts.
    void includeWifiDetails() noexcept
    {
        m_IncludeWifiDetails.store(true, std::memory_order_relaxed);
    }

    /// Whether includeWifiDetails() was called.
    [[nodiscard]] bool wifiDetailsIncluded() const noexcept
    {
        return m_IncludeWifiDetails.load(std::memory_order_relaxed);
    }

    /// From now on, every read() also looks the public addresses up (#1518), which sends a query to a
    /// DNS resolver off the machine: the user clicked "Show public IP". The choice holds for the model's
    /// life (the session). Thread-safe; takes effect from the next read() that starts.
    void includePublicIp() noexcept
    {
        m_IncludePublicIp.store(true, std::memory_order_relaxed);
    }

    /// Whether includePublicIp() was called.
    [[nodiscard]] bool publicIpIncluded() const noexcept
    {
        return m_IncludePublicIp.load(std::memory_order_relaxed);
    }

    /// The next read() first asks for an administrator's password, so the facts readable by root only
    /// are read through the privileged helper (#2310): the user clicked "Authenticate to read". Once,
    /// not for the session: the helper stays authorized for a few minutes by itself. Thread-safe.
    void requestAuthorization() noexcept
    {
        m_AuthorizationRequested.store(true, std::memory_order_relaxed);
    }

    /// What the probe can read, fixed at construction, but for canReadAuthorized, which the helper can
    /// take back during a read (#2769): that is as of the last read().
    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const
    {
        Platform::SystemInfoCapabilities capabilities = m_Capabilities;
        capabilities.canReadAuthorized = m_CanReadAuthorized.load(std::memory_order_relaxed);
        return capabilities;
    }

  private:
    std::unique_ptr<Platform::ISystemInfoProbe> m_Probe; // used under m_ReadMutex only
    Platform::SystemInfoCapabilities m_Capabilities;     // fixed at construction; canReadAuthorized's live value is below
    std::atomic<bool> m_CanReadAuthorized{false};        // refreshed by each read(), read by the UI thread (#2769)
    std::shared_ptr<CrashHistory> m_CrashHistory;        // may be null
    std::atomic<bool> m_IncludeWifiDetails{false};       // set from the UI thread, read by read() (#1782)
    std::atomic<bool> m_IncludePublicIp{false};          // set from the UI thread, read by read() (#1518)
    std::atomic<bool> m_AuthorizationRequested{false};   // set from the UI thread, taken by read() (#2310)
    std::mutex m_ReadMutex;
    std::uint64_t m_LastVersion = 0;                        // guarded by m_ReadMutex
    std::map<std::string, SensorRange> m_SensorRanges;      // guarded by m_ReadMutex
    std::map<std::string, SensorHistory> m_SensorHistories; // guarded by m_ReadMutex
    /// Each device's driver name (without the " #2" that makes it unique on the page) by its
    /// sensorDeviceKey(), as last read: a new name under the same key is a new device (#2495).
    std::map<std::string, std::string, std::less<>> m_SensorDeviceNames; // guarded by m_ReadMutex

    /// Adds @p sensors' readings to m_SensorRanges and m_SensorHistories, first dropping what they
    /// kept for a device that is gone or that another has taken the key of (#2495). Under m_ReadMutex.
    void addToSensorRanges(const Platform::SensorsInfo& sensors);
    /// Drops m_SensorRanges' and m_SensorHistories' entries for the device keyed @p deviceKey.
    void forgetSensorDevice(std::string_view deviceKey);
    PublicationSlot<SystemInfoSnapshot> m_Slot;
};

} // namespace Domain
