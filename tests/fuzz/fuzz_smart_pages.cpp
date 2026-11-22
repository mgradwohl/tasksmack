#include "Domain/Utf8Sanitize.h"
#include "Platform/Windows/WindowsEnergyMeterMath.h"
#include "Platform/Windows/WindowsStorageSmartMath.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// What a drive's firmware, or a device driver, answers the Windows probes with, on arbitrary bytes (#4358): the ATA
// SMART READ DATA and READ THRESHOLDS pages, IDENTIFY DEVICE's SMART words and the AtaHealth built from them (#2588),
// a STORAGE_DEVICE_DESCRIPTOR with its strings and the NVMe health log page (#1517), and the Energy Meter Interface's
// metadata and measurement blocks (#2587). Every parser reads the whole input; READ THRESHOLDS reads the second 512
// bytes when there are two pages. Nothing may read past the input; the attribute table has at most its 30 slots, none
// empty; the descriptor's strings are valid UTF-8 (#4312); an EMI channel name is ASCII with no NUL.
namespace
{

void checkSmart(std::span<const std::byte> bytes)
{
    namespace Smart = Platform::AtaSmartMath;
    const std::span<const std::byte> thresholdPage = bytes.size() >= 2 * Smart::PAGE_BYTES ? bytes.subspan(Smart::PAGE_BYTES) : bytes;
    const std::optional<Smart::Thresholds> thresholds = Smart::parseThresholds(thresholdPage);
    std::optional<bool> statusFailing;
    if (bytes.size() >= Smart::TASK_FILE_BYTES)
    {
        const std::span<const std::uint8_t> taskFile(reinterpret_cast<const std::uint8_t*>(bytes.data()), Smart::TASK_FILE_BYTES);
        static_cast<void>(Smart::commandFailed(taskFile));
        statusFailing = Smart::returnStatusFailing(taskFile[Smart::TASK_FILE_LBA_MID], taskFile[Smart::TASK_FILE_LBA_HIGH]);
    }
    if (const std::optional<std::vector<Smart::Attribute>> attributes = Smart::parseAttributes(bytes))
    {
        if (attributes->size() > Smart::ATTRIBUTE_COUNT)
        {
            __builtin_trap();
        }
        for (const Smart::Attribute& attribute : *attributes)
        {
            if (attribute.id == 0 || attribute.raw >= (std::uint64_t{1} << (8U * Smart::RAW_VALUE_BYTES)))
            {
                __builtin_trap();
            }
        }
        for (const std::uint8_t id : Smart::failingAttributes(*attributes, thresholds))
        {
            if (Smart::findAttribute(*attributes, id) == nullptr)
            {
                __builtin_trap();
            }
        }
        if (const std::optional<int> celsius = Smart::temperatureCelsius(*attributes);
            celsius.has_value() && (*celsius <= 0 || *celsius > 127))
        {
            __builtin_trap();
        }
        static_cast<void>(Smart::toAtaHealth(*attributes, thresholds, statusFailing));
    }
    static_cast<void>(Smart::parseIdentifySmart(bytes));
}

void checkDescriptor(std::span<const std::byte> bytes)
{
    namespace Storage = Platform::WindowsStorage;
    if (const std::optional<Storage::DeviceDescriptor> descriptor = Storage::parseDeviceDescriptor(bytes))
    {
        for (const std::string& text : {descriptor->model, descriptor->firmware, descriptor->serial})
        {
            if (!Domain::isValidUtf8(text))
            {
                __builtin_trap();
            }
        }
    }
    static_cast<void>(Storage::parseNvmeHealthLog(bytes));
}

void checkEnergyMeter(std::span<const std::byte> bytes)
{
    namespace Emi = Platform::Windows;
    for (const std::uint16_t version : {Emi::EMI_INTERFACE_V1, Emi::EMI_INTERFACE_V2})
    {
        const std::optional<std::vector<Emi::EmiChannel>> channels = Emi::parseEmiMetadata(version, bytes);
        if (!channels.has_value())
        {
            continue;
        }
        if (channels->size() > Emi::EMI_CHANNEL_CAP || (version == Emi::EMI_INTERFACE_V1 && channels->size() != 1))
        {
            __builtin_trap();
        }
        for (std::size_t index = 0; index < channels->size(); ++index)
        {
            for (const char c : (*channels)[index].name)
            {
                if (c == '\0' || static_cast<unsigned char>(c) >= 0x80U)
                {
                    __builtin_trap();
                }
            }
            static_cast<void>(Emi::isPackageChannelName((*channels)[index].name));
            if (const std::optional<std::uint64_t> energy = Emi::emiChannelEnergy(bytes, index))
            {
                static_cast<void>(Emi::picowattHoursToMicrojoules(*energy));
            }
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const std::uint8_t>(data, size));
    checkSmart(bytes);
    checkDescriptor(bytes);
    checkEnergyMeter(bytes);
    return 0;
}
