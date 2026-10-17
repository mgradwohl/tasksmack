#include "Platform/DnsMessage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The DNS reply parser (#2172), the only parser in the tree fed bytes from the network: a resolver's
// UDP reply to the opt-in public IP lookup, or a spoofed datagram. The input is the reply to fixed A
// and AAAA queries, and is also tried as both reply and query. Nothing may read past either buffer
// or loop on a compression pointer cycle; every input gets an address or an AnswerError.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace Dns = Platform::DnsMessage;
    static const std::vector<std::uint8_t> QUERY_A = Dns::buildQuery(0x1234, "myip.example.com", Dns::RecordType::A);
    static const std::vector<std::uint8_t> QUERY_AAAA = Dns::buildQuery(0x1234, "myip.example.com", Dns::RecordType::AAAA);

    const std::span<const std::uint8_t> input(data, size);
    static_cast<void>(Dns::parseAnswer(input, QUERY_A));
    static_cast<void>(Dns::parseAnswer(input, QUERY_AAAA));
    static_cast<void>(Dns::parseAnswer(input, input));
    return 0;
}
