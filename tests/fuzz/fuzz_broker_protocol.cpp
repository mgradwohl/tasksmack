#include "Platform/BrokerProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

// The privileged-broker decoder (#2306) on arbitrary bytes: the elevated helper decodes what an
// unprivileged process sends, so nothing may read past the frame, allocate without bound, or throw out.
// A frame that decodes re-encodes to the same bytes and decodes to the same message.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace Broker = Platform::Broker;
    const std::span<const std::uint8_t> input(data, size);
    if (const auto request = Broker::decodeRequest(input); request.has_value())
    {
        const std::vector<std::uint8_t> again = Broker::encodeRequest(request->requestId, request->message);
        const auto reread = Broker::decodeRequest(again);
        if (!reread.has_value() || !(reread->message == request->message))
        {
            std::abort();
        }
    }
    if (const auto response = Broker::decodeResponse(input); response.has_value())
    {
        const std::vector<std::uint8_t> again = Broker::encodeResponse(response->requestId, response->message);
        const auto reread = Broker::decodeResponse(again);
        if (!reread.has_value() || !(reread->message == response->message))
        {
            std::abort();
        }
    }
    return 0;
}
