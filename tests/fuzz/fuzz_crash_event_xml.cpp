#include "Domain/Utf8Sanitize.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsCrashEvents.h"
#include "Platform/Windows/WindowsStartupImpact.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The two hand-written XML readers on Windows, on arbitrary text (#4358): an Application Error or Application Hang
// event as EvtRender renders it (WindowsCrashEvents::parseEventXml, #1524), whose data strings the crashing program
// chose, and a WDI StartupInfo file (WindowsStartupImpact::parseStartupInfoXml, #3073), whose command lines any
// program started at sign-in chose. The input is read as the event's UTF-8, and as a StartupInfo file's bytes both
// decoded (UTF-16 when it looks like it, as Windows writes the file) and as they are. Nothing may read past the text;
// a UTF-16 file always decodes to valid UTF-8; a reading's processes each have a path and a start time, in start order.
namespace
{

void checkReading(const std::optional<Platform::StartupImpactReading>& reading)
{
    if (!reading.has_value())
    {
        return;
    }
    std::uint64_t previous = 0;
    for (const Platform::StartupImpactProcess& process : reading->processes)
    {
        if (process.path.empty() || process.startTicks == 0 || process.startTicks < previous)
        {
            __builtin_trap();
        }
        previous = process.startTicks;
    }
    constexpr std::uint64_t TICKS_PER_SECOND = 10'000'000;
    if (!reading->processes.empty() && reading->signInUnixSeconds != reading->processes.front().startTicks / TICKS_PER_SECOND)
    {
        __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace Crash = Platform::WindowsCrashEvents;
    namespace Startup = Platform::WindowsStartupImpact;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    static_cast<void>(Crash::parseEventXml(text));
    static_cast<void>(Crash::parseSystemTime(text));
    static_cast<void>(Startup::parseTime(text));

    const std::span<const std::uint8_t> bytes(data, size);
    const std::string decoded = Startup::decodeText(bytes);
    // decodeText()'s UTF-16 test: a byte order mark, or a zero second byte (neither can start a UTF-8 one).
    const bool utf16 = size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || data[1] == 0);
    if (utf16 && !Domain::isValidUtf8(decoded))
    {
        __builtin_trap();
    }
    checkReading(Startup::parseStartupInfoXml(decoded));
    checkReading(Startup::parseStartupInfoXml(text));
    return 0;
}
