#include <array>
#include <cstdint>
#include <cstdio>

namespace
{

using rsmi_device_t = std::uint32_t;
using rsmi_status_t = std::uint32_t;

constexpr rsmi_status_t RSMI_STATUS_SUCCESS = 0;
constexpr rsmi_status_t RSMI_STATUS_NOT_FOUND = 10;

enum rsmi_temperature_type_t : std::uint32_t
{
    RSMI_TEMP_TYPE_EDGE = 0,
    RSMI_TEMP_TYPE_JUNCTION = 1
};

enum rsmi_temperature_metric_t : std::uint32_t
{
    RSMI_TEMP_CURRENT = 0
};

enum rsmi_clk_type_t : std::uint32_t
{
    RSMI_CLK_TYPE_SYS = 0,
    RSMI_CLK_TYPE_MEM = 4
};

enum rsmi_memory_type_t : std::uint32_t
{
    RSMI_MEM_TYPE_VRAM = 0
};

struct rsmi_frequencies_t
{
    std::uint32_t num_supported;
    std::uint32_t current;
    std::uint64_t frequency[32];
};

struct MockRocmDevice
{
    const char* name;
    bool hasName;
    std::uint64_t uniqueId;
    bool hasUniqueId;
    std::uint64_t pciId;
    bool hasPciId;
    std::uint32_t busyPercent;
    std::uint64_t memoryUsedBytes;
    std::uint64_t memoryTotalBytes;
    std::int64_t edgeTempMilliC;
    std::int64_t hotspotTempMilliC;
    bool hasHotspot;
    std::uint64_t powerMicroWatts;
    std::uint64_t powerCapMicroWatts;
    rsmi_frequencies_t gpuFrequency;
    bool hasGpuFrequency;
    rsmi_frequencies_t memoryFrequency;
    bool hasMemoryFrequency;
    std::int64_t fanSpeed;
    bool hasFanSpeed;
};

constexpr rsmi_frequencies_t makeFrequencies(std::uint32_t numSupported, std::uint32_t current, std::uint64_t activeFrequency)
{
    rsmi_frequencies_t result{};
    result.num_supported = numSupported;
    result.current = current;
    result.frequency[current < 32U ? current : 0U] = activeFrequency;
    return result;
}

constexpr std::array<MockRocmDevice, 3> MOCK_DEVICES{{
    {.name = "Mock AMD GPU 0",
     .hasName = true,
     .uniqueId = 4001ULL,
     .hasUniqueId = true,
     .pciId = 0ULL,
     .hasPciId = false,
     .busyPercent = 80U,
     .memoryUsedBytes = 3ULL * 1024ULL * 1024ULL * 1024ULL,
     .memoryTotalBytes = 12ULL * 1024ULL * 1024ULL * 1024ULL,
     .edgeTempMilliC = 65000,
     .hotspotTempMilliC = 72000,
     .hasHotspot = true,
     .powerMicroWatts = 150000000ULL,
     .powerCapMicroWatts = 220000000ULL,
     .gpuFrequency = makeFrequencies(2U, 1U, 1500000000ULL),
     .hasGpuFrequency = true,
     .memoryFrequency = makeFrequencies(2U, 1U, 2000000000ULL),
     .hasMemoryFrequency = true,
     .fanSpeed = 1700,
     .hasFanSpeed = true},
    {.name = "Mock AMD GPU 1",
     .hasName = false,
     .uniqueId = 0ULL,
     .hasUniqueId = false,
     .pciId = 9001ULL,
     .hasPciId = true,
     .busyPercent = 35U,
     .memoryUsedBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL,
     .memoryTotalBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL,
     .edgeTempMilliC = 51000,
     .hotspotTempMilliC = 0,
     .hasHotspot = false,
     .powerMicroWatts = 90000000ULL,
     .powerCapMicroWatts = 180000000ULL,
     .gpuFrequency = makeFrequencies(1U, 3U, 0ULL),
     .hasGpuFrequency = true,
     .memoryFrequency = {},
     .hasMemoryFrequency = false,
     .fanSpeed = 0,
     .hasFanSpeed = false},
    {.name = "Mock AMD GPU 2",
     .hasName = true,
     .uniqueId = 0ULL,
     .hasUniqueId = false,
     .pciId = 0ULL,
     .hasPciId = false,
     .busyPercent = 10U,
     .memoryUsedBytes = 512ULL * 1024ULL * 1024ULL,
     .memoryTotalBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
     .edgeTempMilliC = 43000,
     .hotspotTempMilliC = 47000,
     .hasHotspot = true,
     .powerMicroWatts = 45000000ULL,
     .powerCapMicroWatts = 120000000ULL,
     .gpuFrequency = makeFrequencies(1U, 0U, 1000000000ULL),
     .hasGpuFrequency = true,
     .memoryFrequency = makeFrequencies(1U, 0U, 1500000000ULL),
     .hasMemoryFrequency = true,
     .fanSpeed = 1200,
     .hasFanSpeed = true},
}};

} // namespace

extern "C"
{

    rsmi_status_t rsmi_init(std::uint64_t /*flags*/)
    {
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_shut_down()
    {
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_num_monitor_devices(std::uint32_t* count)
    {
        *count = static_cast<std::uint32_t>(MOCK_DEVICES.size());
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_name_get(std::uint32_t deviceIndex, char* name, std::size_t length)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        if (!device.hasName)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        ::snprintf(name, length, "%s", device.name);
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_id_get(std::uint32_t /*deviceIndex*/, std::uint16_t* id)
    {
        *id = 0;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_pci_id_get(std::uint32_t deviceIndex, std::uint64_t* pciId)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        if (!device.hasPciId)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        *pciId = device.pciId;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_unique_id_get(std::uint32_t deviceIndex, std::uint64_t* uniqueId)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        if (!device.hasUniqueId)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        *uniqueId = device.uniqueId;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_gpu_busy_percent_get(std::uint32_t deviceIndex, std::uint32_t* busyPercent)
    {
        *busyPercent = MOCK_DEVICES.at(deviceIndex).busyPercent;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_memory_usage_get(std::uint32_t deviceIndex, rsmi_memory_type_t /*memoryType*/, std::uint64_t* usedBytes)
    {
        *usedBytes = MOCK_DEVICES.at(deviceIndex).memoryUsedBytes;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_memory_total_get(std::uint32_t deviceIndex, rsmi_memory_type_t /*memoryType*/, std::uint64_t* totalBytes)
    {
        *totalBytes = MOCK_DEVICES.at(deviceIndex).memoryTotalBytes;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_temp_metric_get(std::uint32_t deviceIndex,
                                           rsmi_temperature_type_t type,
                                           rsmi_temperature_metric_t /*metric*/,
                                           std::int64_t* temperature)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        if (type == RSMI_TEMP_TYPE_JUNCTION && !device.hasHotspot)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        *temperature = (type == RSMI_TEMP_TYPE_JUNCTION) ? device.hotspotTempMilliC : device.edgeTempMilliC;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_power_ave_get(std::uint32_t deviceIndex, std::uint32_t /*sensor*/, std::uint64_t* power)
    {
        *power = MOCK_DEVICES.at(deviceIndex).powerMicroWatts;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_power_cap_get(std::uint32_t deviceIndex, std::uint32_t /*sensor*/, std::uint64_t* powerCap)
    {
        *powerCap = MOCK_DEVICES.at(deviceIndex).powerCapMicroWatts;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_gpu_clk_freq_get(std::uint32_t deviceIndex, rsmi_clk_type_t type, rsmi_frequencies_t* frequencies)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        const bool isMemory = (type == RSMI_CLK_TYPE_MEM);
        const auto& source = isMemory ? device.memoryFrequency : device.gpuFrequency;
        const bool available = isMemory ? device.hasMemoryFrequency : device.hasGpuFrequency;
        if (!available)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        *frequencies = source;
        return RSMI_STATUS_SUCCESS;
    }

    rsmi_status_t rsmi_dev_fan_speed_get(std::uint32_t deviceIndex, std::uint32_t /*sensorIndex*/, std::int64_t* fanSpeed)
    {
        const auto& device = MOCK_DEVICES.at(deviceIndex);
        if (!device.hasFanSpeed)
        {
            return RSMI_STATUS_NOT_FOUND;
        }

        *fanSpeed = device.fanSpeed;
        return RSMI_STATUS_SUCCESS;
    }

    const char* rsmi_status_string(rsmi_status_t /*status*/)
    {
        return "Mock ROCm status";
    }

} // extern "C"
