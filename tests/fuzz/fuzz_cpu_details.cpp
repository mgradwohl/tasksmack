#include "Platform/Linux/LinuxCpuDetails.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// /proc/cpuinfo and the /sys/devices/system/cpu values the CPU tab reads (#4616): each processor block, the online
// set from them, the model name, the virtualization flags, and the cache, hex and cpuN fields. Nothing may read past
// the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Cpu = Platform::LinuxCpuDetails;

    const std::vector<Cpu::CpuInfoProcessor> processors = Cpu::parseCpuInfoProcessors(text);
    static_cast<void>(Cpu::onlineCpus(processors));
    static_cast<void>(Cpu::cpuModelFromCpuInfo(text));
    static_cast<void>(Cpu::parseCpuVirtualizationFlags(text));
    static_cast<void>(Cpu::Detail::parseUnsigned(text));
    static_cast<void>(Cpu::parseCacheSize(text));
    static_cast<void>(Cpu::Detail::parseHex(text));
    static_cast<void>(Cpu::cpuDirectoryIndex(text));
    return 0;
}
