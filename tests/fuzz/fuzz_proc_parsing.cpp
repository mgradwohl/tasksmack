#include "Platform/Linux/ProcParsing.h"

#include <cstddef>
#include <cstdint>

namespace
{

volatile std::uint64_t g_UnsignedSink = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::int64_t g_SignedSink = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
volatile double g_DoubleSink = 0.0;        // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

template<typename T> void parseInteger(const char* begin, const char* end, volatile T& sink)
{
    const char* cursor = begin;
    T value = 0;
    if (Platform::ProcParsing::parseNum(cursor, end, value))
    {
        sink = value;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    if (size == 0)
    {
        return 0;
    }

    // Any object representation may be inspected through char pointers.
    const auto* begin = reinterpret_cast<const char*>(data);
    const char* const end = begin + size;

    parseInteger<std::uint64_t>(begin, end, g_UnsignedSink);
    parseInteger<std::int64_t>(begin, end, g_SignedSink);

    const char* cursor = begin;
    double value = 0.0;
    if (Platform::ProcParsing::parseDouble(cursor, end, value))
    {
        g_DoubleSink = value;
    }

    return 0;
}
