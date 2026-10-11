#include "LinuxSensorProbe.h"

#include "LinuxSensors.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// A sysfs value fits easily: "-273150\n", "unsupported\n".
constexpr std::size_t READ_BUFFER_BYTES = 32;

[[nodiscard]] int openReadOnly(const std::filesystem::path& path) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) - POSIX open() is variadic
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}

/// The file's text from offset 0, into @p buffer; empty when it can't be read.
[[nodiscard]] std::string_view readAt0(int fd, std::array<char, READ_BUFFER_BYTES>& buffer) noexcept
{
    if (fd < 0)
    {
        return {};
    }
    const auto bytes = ::pread(fd, buffer.data(), buffer.size(), 0);
    return (bytes > 0) ? std::string_view(buffer.data(), static_cast<std::size_t>(bytes)) : std::string_view{};
}

} // namespace

LinuxSensorProbe::~LinuxSensorProbe()
{
    closeAll();
}

void LinuxSensorProbe::closeAll() noexcept
{
    for (const Input& input : m_Inputs)
    {
        if (input.fd >= 0)
        {
            ::close(input.fd);
        }
    }
    for (const int fd : m_StatusFds)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
    m_Inputs.clear();
    m_StatusFds.clear();
    m_StatusAwake.clear();
}

SensorsInfo LinuxSensorProbe::discover()
{
    closeAll();
    SensorsInfo info;
    std::vector<LinuxSensors::SensorSource> sources;
    LinuxSensors::readSensorFacts(m_Root, info, &sources);

    std::map<std::filesystem::path, std::size_t> statusIndex;
    // Reserved up front, so no push_back below can throw with a just-opened descriptor in hand.
    m_Inputs.reserve(sources.size());
    m_StatusFds.reserve(sources.size());
    for (const LinuxSensors::SensorSource& source : sources)
    {
        Input input{.fd = openReadOnly(source.input), .divisor = source.divisor, .status = NONE};
        if (!source.runtimeStatus.empty())
        {
            const auto [it, added] = statusIndex.try_emplace(source.runtimeStatus, m_StatusFds.size());
            if (added)
            {
                m_StatusFds.push_back(openReadOnly(source.runtimeStatus));
            }
            input.status = it->second;
        }
        m_Inputs.push_back(input);
    }
    m_StatusAwake.assign(m_StatusFds.size(), 0);
    return info;
}

void LinuxSensorProbe::readValues(std::span<double> values)
{
    std::array<char, READ_BUFFER_BYTES> buffer{};
    std::size_t chip = 0;
    while (chip < m_StatusFds.size())
    {
        m_StatusAwake[chip] = LinuxSensors::runtimeStatusAllowsRead(readAt0(m_StatusFds[chip], buffer)) ? 1 : 0;
        ++chip;
    }

    const std::size_t count = std::min(values.size(), m_Inputs.size());
    std::size_t index = 0;
    while (index < count)
    {
        const Input& input = m_Inputs[index];
        double value = std::numeric_limits<double>::quiet_NaN();
        if (input.status == NONE || m_StatusAwake[input.status] != 0)
        {
            if (const std::optional<std::int64_t> raw = LinuxSensors::parseSysfsInteger(readAt0(input.fd, buffer)); raw.has_value())
            {
                value = static_cast<double>(*raw) / input.divisor;
            }
        }
        values[index] = value;
        ++index;
    }
}

} // namespace Platform
