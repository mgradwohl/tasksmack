#pragma once

#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace Platform
{

/// The live Sensors section's per-tick read on Linux (#1522): discover() lists the hwmon and thermal-zone
/// sensors as LinuxSensors.h does for the System Information page and opens each one's input file once;
/// readValues() then re-reads every open file from offset 0 (one pread() each, which makes sysfs
/// regenerate the value), into a stack buffer: no enumeration and no allocation per tick. A chip whose
/// device has a runtime PM status is read only while that says it is awake, so a sleeping discrete GPU
/// isn't woken every tick; its readings are NaN meanwhile.
class LinuxSensorProbe final : public ISensorProbe
{
  public:
    /// @param root Where /sys is read from; tests point it at a fixture tree.
    explicit LinuxSensorProbe(std::filesystem::path root = "/") : m_Root(std::move(root))
    {}
    ~LinuxSensorProbe() override;

    LinuxSensorProbe(const LinuxSensorProbe&) = delete;
    LinuxSensorProbe& operator=(const LinuxSensorProbe&) = delete;
    LinuxSensorProbe(LinuxSensorProbe&&) = delete;
    LinuxSensorProbe& operator=(LinuxSensorProbe&&) = delete;

    [[nodiscard]] SensorsInfo discover() override;
    void readValues(std::span<double> values) override;

  private:
    /// One reading's open input file.
    struct Input
    {
        int fd = -1;               ///< -1 when it couldn't be opened: always NaN
        double divisor = 1.0;      ///< From the file's unit to the kind's
        std::size_t status = NONE; ///< Index into m_StatusFds of its chip's runtime PM status, or NONE
    };
    static constexpr std::size_t NONE = static_cast<std::size_t>(-1);

    void closeAll() noexcept;

    std::filesystem::path m_Root;
    std::vector<Input> m_Inputs;     ///< discover()'s readings, in order
    std::vector<int> m_StatusFds;    ///< One per chip with a runtime PM status (-1 when it couldn't be opened)
    std::vector<char> m_StatusAwake; ///< This tick's answer per status file, sized with it: no allocation per tick
};

} // namespace Platform
