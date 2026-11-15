// LinuxProcessProbe's package power (RAPL, #1093): the powercap zones it reads, split from LinuxProcessProbe.cpp (#3772).

// Linux only, as LinuxProcessProbe.cpp is (parseable elsewhere, e.g. Windows clangd).
#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<pwd.h>) && __has_include(<unistd.h>)

#include "LinuxProcessProbe.h"
#include "Platform/IProcessProbe.h"
#include "ProcParsing.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/syscall.h> // NOLINT(misc-include-cleaner): SYS_ioprio_get, which include-cleaner has no mapping for
#include <unistd.h>

namespace Platform
{

using ProcParsing::parseNum;
using ProcParsing::readProcFile;

bool LinuxProcessProbe::detectPowerCap()
{
    // energy_uj has been root-only (0400) since the Platypus fix (CVE-2020-8694), so a file that
    // exists is not enough: detection used to accept one by existence and then report 0 W for
    // every process for a normal user (#1103). Only a file we can open counts.
    const auto isReadable = [](const std::filesystem::path& path)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1)
        {
            return false;
        }
        ::close(fd);
        return true;
    };

    std::vector<std::filesystem::path> candidates = {
        m_PowercapRoot / "intel-rapl" / "intel-rapl:0" / "energy_uj",
        m_PowercapRoot / "intel-rapl:0" / "energy_uj",
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_PowercapRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().filename().string().starts_with("intel-rapl"))
        {
            candidates.push_back(it->path() / "energy_uj");
            candidates.push_back(it->path() / "intel-rapl:0" / "energy_uj");
        }
    }

    const auto found = std::ranges::find_if(candidates, isReadable);
    if (found == candidates.end())
    {
        return false;
    }
    const std::filesystem::path& path = *found;
    m_PowerCapPath = path.string();

    // Where the counter wraps back to 0, so a wrap reads as the energy used, not a drop.
    m_PowerCapMaxRangeUj = readEnergyFile((path.parent_path() / "max_energy_range_uj").string()).value_or(0);
    if (path.parent_path().filename() == "intel-rapl:0")
    {
        detectOtherPackages(path.parent_path());
    }
    return true;
}

void LinuxProcessProbe::detectOtherPackages(const std::filesystem::path& firstZone)
{
    // Only "intel-rapl:N" itself: "intel-rapl:N:M" are a package's subzones (cores, uncore, DRAM),
    // already in its energy. AMD's RAPL zones are named the same.
    std::vector<std::pair<unsigned, OtherPackageCounter>> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(firstZone.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        constexpr std::string_view PREFIX = "intel-rapl:";
        unsigned index = 0;
        const char* const first = name.data() + std::min(PREFIX.size(), name.size());
        const char* const last = name.data() + name.size();
        if (!name.starts_with(PREFIX) || first == last || std::from_chars(first, last, index, 10).ptr != last || index == 0)
        {
            continue;
        }
        std::array<char, 64> zoneName{};
        const std::string namePath = (it->path() / "name").string();
        const std::size_t len = readProcFile(namePath.c_str(), zoneName.data(), zoneName.size());
        const std::string energyPath = (it->path() / "energy_uj").string();
        if (!std::string_view(zoneName.data(), len).starts_with("package-") || !readEnergyFile(energyPath).has_value())
        {
            continue;
        }
        found.emplace_back(index,
                           OtherPackageCounter{
                               .path = energyPath,
                               .maxRangeUj = readEnergyFile((it->path() / "max_energy_range_uj").string()).value_or(0),
                           });
    }
    std::ranges::sort(found, {}, &std::pair<unsigned, OtherPackageCounter>::first);
    for (auto& [index, counter] : found)
    {
        m_OtherPackages.push_back(std::move(counter));
    }
}

std::optional<uint64_t> LinuxProcessProbe::readEnergyFile(const std::string& path)
{
    std::array<char, 32> buf{};
    const std::size_t len = readProcFile(path.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return std::nullopt;
    }
    const char* p = buf.data();
    uint64_t value = 0;
    if (!parseNum(p, buf.data() + len, value))
    {
        return std::nullopt;
    }
    return value;
}

std::optional<uint64_t> LinuxProcessProbe::readSystemEnergy() const
{
    if (m_PowerCapPath.empty())
    {
        return std::nullopt;
    }
    return readEnergyFile(m_PowerCapPath); // Already in microjoules
}

std::optional<PackageEnergyReading> LinuxProcessProbe::readPackageEnergy() const
{
    if (!m_HasPowerCap)
    {
        return std::nullopt;
    }
    // Raw read only: ProcessModel shares it out between processes per interval (#1093).
    const auto cpuTimes = readCpuTimes();
    PackageEnergyReading reading{
        .energyUj = readSystemEnergy(),
        .maxRangeUj = m_PowerCapMaxRangeUj,
        .busyCpuTicks = cpuTimes.has_value() ? std::optional{cpuTimes->busy} : std::nullopt,
        .otherPackages = {},
    };
    // Every package's energy, as the busy ticks are every package's CPUs' (#2444). One that can't be read
    // makes the sample unread, rather than one package's energy shared out over all of them.
    for (const OtherPackageCounter& package : m_OtherPackages)
    {
        const std::optional<uint64_t> energyUj = reading.energyUj.has_value() ? readEnergyFile(package.path) : std::nullopt;
        if (!energyUj.has_value())
        {
            reading.energyUj = std::nullopt;
            reading.otherPackages.clear();
            break;
        }
        reading.otherPackages.push_back({.energyUj = *energyUj, .maxRangeUj = package.maxRangeUj});
    }
    return reading;
}

} // namespace Platform

#endif
