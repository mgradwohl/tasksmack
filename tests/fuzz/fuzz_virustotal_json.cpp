#include "Domain/VirusTotal.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Look up on VirusTotal's reply reader (#3094) on arbitrary bytes: the file report comes from the network over
// HTTPS. Nothing may read past the input, recurse, or throw, whatever the status.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace VirusTotal = Domain::VirusTotal;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the fuzzer's bytes as text
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    static_cast<void>(VirusTotal::readFileReport(text));
    for (const int status : {200, 404, 429, 500})
    {
        static_cast<void>(VirusTotal::summary("x", VirusTotal::interpret(status, text)));
    }
    static_cast<void>(VirusTotal::isSha256Hex(text));
    static_cast<void>(VirusTotal::isApiKey(text));
    return 0;
}
