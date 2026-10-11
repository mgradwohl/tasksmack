#pragma once

#include "Domain/CrashHistory.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Domain
{

/// One immutable read of the System Information page's static facts (#1399). Each section's raw
/// facts are a member; a new section adds one.
/// The lowest and highest one sensor has read since the page's first read (#1522).
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

/// A sensor's key in SystemInfoSnapshot::sensorRanges: "device: label", as its row is labelled.
[[nodiscard]] inline std::string sensorRangeKey(std::string_view device, std::string_view label)
{
    return std::format("{}: {}", device, label);
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
    /// Each sensor's range over the reads so far (sensorRangeKey()); full reads and readSensors() add to it.
    std::map<std::string, SensorRange> sensorRanges;
    /// Each sensor's recent readings, keyed as sensorRanges; appended to by the same reads (#1522).
    std::map<std::string, SensorHistory> sensorHistories;
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
    /// UI thread.
    void read();

    /// read(), for a worker thread: a read that throws is caught here, on the thread that threw it, and
    /// only its message is returned (a copy), so no exception object reaches another thread through a
    /// future (#1685, #1706). @return nullopt when the read succeeded; else the exception's what(), or
    /// "unknown error" for a throw that isn't a std::exception. Only building that message (bad_alloc)
    /// can still escape.
    [[nodiscard]] std::optional<std::string> tryRead();
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

    /// What the probe can read, fixed at construction.
    [[nodiscard]] const Platform::SystemInfoCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

  private:
    std::unique_ptr<Platform::ISystemInfoProbe> m_Probe; // used under m_ReadMutex only
    Platform::SystemInfoCapabilities m_Capabilities;
    std::shared_ptr<CrashHistory> m_CrashHistory; // may be null
    std::mutex m_ReadMutex;
    std::uint64_t m_LastVersion = 0;                        // guarded by m_ReadMutex
    std::map<std::string, SensorRange> m_SensorRanges;      // guarded by m_ReadMutex
    std::map<std::string, SensorHistory> m_SensorHistories; // guarded by m_ReadMutex

    /// Adds @p sensors' readings to m_SensorRanges and m_SensorHistories. Under m_ReadMutex.
    void addToSensorRanges(const Platform::SensorsInfo& sensors);
    PublicationSlot<SystemInfoSnapshot> m_Slot;
};

} // namespace Domain
