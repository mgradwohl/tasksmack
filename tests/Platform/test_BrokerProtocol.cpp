/// @file test_BrokerProtocol.cpp
/// @brief The privileged-broker wire format (#2306): every message round-trips, and the decoder
/// refuses whatever isn't exactly a frame it knows. The helper reads what an unprivileged process
/// sends, so the refusals matter as much as the round trips.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace Platform::Broker
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 0x0123456789ABCDEFULL};

[[nodiscard]] Request roundTrip(const Request& request, std::uint32_t id = 7)
{
    const std::vector<std::uint8_t> bytes = encodeRequest(id, request);
    const auto decoded = decodeRequest(bytes);
    EXPECT_TRUE(decoded.has_value()) << (decoded.has_value() ? "" : decoded.error());
    if (!decoded.has_value())
    {
        return Hello{};
    }
    EXPECT_EQ(decoded->requestId, id);
    return decoded->message;
}

[[nodiscard]] Response roundTrip(const Response& response, std::uint32_t id = 9)
{
    const std::vector<std::uint8_t> bytes = encodeResponse(id, response);
    const auto decoded = decodeResponse(bytes);
    EXPECT_TRUE(decoded.has_value()) << (decoded.has_value() ? "" : decoded.error());
    if (!decoded.has_value())
    {
        return ErrorReply{};
    }
    EXPECT_EQ(decoded->requestId, id);
    return decoded->message;
}

TEST(BrokerProtocolTest, EveryRequestRoundTrips)
{
    EXPECT_EQ(roundTrip(Request{Hello{}}), Request{Hello{}});
    for (const Signal signal : {Signal::Terminate, Signal::Kill, Signal::Suspend, Signal::Resume})
    {
        const Request request = SignalRequest{.target = TARGET, .signal = signal};
        EXPECT_EQ(roundTrip(request), request);
    }
    const Request priority = PriorityRequest{.target = TARGET, .nice = -20};
    EXPECT_EQ(roundTrip(priority), priority);
    const Request io = IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 0};
    EXPECT_EQ(roundTrip(io), io);

    CpuAffinity cpus;
    cpus.set(0);
    cpus.set(3);
    cpus.set(130); // past the first word
    const Request affinity = AffinityRequest{.target = TARGET, .cpus = cpus};
    EXPECT_EQ(roundTrip(affinity), affinity);
}

TEST(BrokerProtocolTest, EveryResponseRoundTrips)
{
    const Response hello = HelloReply{.version = PROTOCOL_VERSION, .accepted = true};
    EXPECT_EQ(roundTrip(hello), hello);
    const Response ok = ActionResult{.success = true, .errorMessage = {}, .successNote = "Changed 3 of 4 threads"};
    EXPECT_EQ(roundTrip(ok), ok);
    const Response refused = ActionResult{.success = false, .errorMessage = "Process not found", .successNote = {}};
    EXPECT_EQ(roundTrip(refused), refused);
    const Response error = ErrorReply{.message = "A malformed message"};
    EXPECT_EQ(roundTrip(error), error);
}

TEST(BrokerProtocolTest, TheFrameIsLittleEndianWithItsLengthVersionKindAndId)
{
    const std::vector<std::uint8_t> bytes = encodeRequest(0x01020304, Request{Hello{}});
    ASSERT_EQ(bytes.size(), 4U + HEADER_BYTES + 2U);
    EXPECT_EQ(bytes[0], HEADER_BYTES + 2U); // length, low byte first
    EXPECT_EQ(bytes[4], PROTOCOL_VERSION & 0xFFU);
    EXPECT_EQ(bytes[6], static_cast<std::uint8_t>(MessageKind::Hello));
    EXPECT_EQ(bytes[8], 0x04U); // request id, little-endian
    EXPECT_EQ(bytes[11], 0x01U);
}

TEST(BrokerProtocolTest, RefusesAnythingButExactlyAKnownFrame)
{
    const std::vector<std::uint8_t> good = encodeRequest(1, Request{SignalRequest{.target = TARGET, .signal = Signal::Kill}});
    ASSERT_TRUE(decodeRequest(good).has_value());

    // Truncated, at every length.
    for (std::size_t size = 0; size < good.size(); ++size)
    {
        EXPECT_FALSE(decodeRequest(std::span(good).first(size)).has_value()) << size;
    }
    // A byte too many.
    std::vector<std::uint8_t> longer = good;
    longer.push_back(0);
    EXPECT_FALSE(decodeRequest(longer).has_value());
    // The length field lying, both ways.
    std::vector<std::uint8_t> lying = good;
    ++lying[0];
    EXPECT_FALSE(decodeRequest(lying).has_value());
    // Another version.
    std::vector<std::uint8_t> version = good;
    version[4] = static_cast<std::uint8_t>(PROTOCOL_VERSION + 1);
    EXPECT_FALSE(decodeRequest(version).has_value());
    // An unknown kind, and a response where a request is expected.
    std::vector<std::uint8_t> kind = good;
    kind[6] = 200;
    EXPECT_FALSE(decodeRequest(kind).has_value());
    EXPECT_FALSE(decodeRequest(encodeResponse(1, Response{ErrorReply{.message = "x"}})).has_value());
    // A signal out of range (the last payload byte).
    std::vector<std::uint8_t> signal = good;
    signal.back() = 9;
    EXPECT_FALSE(decodeRequest(signal).has_value());
}

TEST(BrokerProtocolTest, RefusesOutOfRangeEnumsCountsAndStrings)
{
    // An I/O class past Idle: its byte follows the 12-byte target.
    std::vector<std::uint8_t> io =
        encodeRequest(1, Request{IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 0}});
    io[4 + HEADER_BYTES + 12] = 7;
    EXPECT_FALSE(decodeRequest(io).has_value());

    // An affinity claiming more words than any CPU count allows.
    std::vector<std::uint8_t> affinity = encodeRequest(1, Request{AffinityRequest{.target = TARGET, .cpus = CpuAffinity::fromMask(1)}});
    const std::size_t countAt = 4 + HEADER_BYTES + 12;
    affinity[countAt + 3] = 0x7F; // a huge count, with only one word after it
    EXPECT_FALSE(decodeRequest(affinity).has_value());

    // A string longer than it says, and one longer than the cap.
    std::vector<std::uint8_t> text = encodeResponse(1, Response{ErrorReply{.message = "abc"}});
    text[4 + HEADER_BYTES] = 200;
    EXPECT_FALSE(decodeResponse(text).has_value());
    const std::string huge(MAX_STRING_BYTES + 10, 'x');
    const auto capped = decodeResponse(encodeResponse(1, Response{ErrorReply{.message = huge}}));
    ASSERT_TRUE(capped.has_value()) << "the writer caps what it sends to what the reader takes";
    EXPECT_EQ(std::get<ErrorReply>(capped->message).message.size(), MAX_STRING_BYTES);

    // A flag that isn't 0 or 1.
    std::vector<std::uint8_t> flag = encodeResponse(1, Response{ActionResult{.success = true, .errorMessage = {}, .successNote = {}}});
    flag[4 + HEADER_BYTES] = 2;
    EXPECT_FALSE(decodeResponse(flag).has_value());
}

TEST(BrokerProtocolTest, ActionResultsOfACallReadInTheUsersTerms)
{
    using Result = std::expected<Response, std::string>;
    EXPECT_TRUE(actionResultOf(Result{ActionResult{.success = true, .errorMessage = {}, .successNote = "note"}}).success);
    EXPECT_EQ(actionResultOf(Result{ActionResult{.success = true, .errorMessage = {}, .successNote = "note"}}).successNote, "note");
    EXPECT_EQ(actionResultOf(Result{ActionResult{.success = false, .errorMessage = "Process not found", .successNote = {}}}).errorMessage,
              "Process not found");
    EXPECT_EQ(actionResultOf(Result{ErrorReply{.message = "A malformed message"}}).errorMessage,
              "The administrator helper refused it: A malformed message");
    EXPECT_EQ(actionResultOf(Result{std::unexpect, "timed out"}).errorMessage, "The administrator helper couldn't be asked: timed out");
    EXPECT_FALSE(actionResultOf(Result{HelloReply{}}).success);
}

TEST(BrokerProtocolTest, TheFakeBrokerRecordsRequestsAndAnswersFromItsScript)
{
    Mocks::FakeBroker broker;
    const std::stop_source stop;
    const Request kill = SignalRequest{.target = TARGET, .signal = Signal::Kill};
    EXPECT_TRUE(actionResultOf(broker.call(kill, stop.get_token())).success);
    broker.setAnswer([](const Request&) { return Response{ActionResult{.success = false, .errorMessage = "No", .successNote = {}}}; });
    EXPECT_EQ(actionResultOf(broker.call(kill, stop.get_token())).errorMessage, "No");
    broker.setConnected(false);
    EXPECT_FALSE(actionResultOf(broker.call(kill, stop.get_token())).success);
    ASSERT_EQ(broker.requests().size(), 3U);
    EXPECT_EQ(broker.requests()[0], kill);
}

// #2309: Process Details' environment, read by the helper.
TEST(BrokerProtocolTest, EnvironmentReadsRoundTrip)
{
    const Request request = EnvironmentRequest{.target = TARGET};
    EXPECT_EQ(roundTrip(request), request);

    const Response ok = EnvironmentReply{
        .result = {.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}, {.name = "EMPTY", .value = ""}}}};
    EXPECT_EQ(roundTrip(ok), ok);
    const Response gone = EnvironmentReply{.result = {.status = EnvironmentReadStatus::ProcessExited, .variables = {}}};
    EXPECT_EQ(roundTrip(gone), gone);
}

TEST(BrokerProtocolTest, AnEnvironmentReplyOutOfRangeIsRefused)
{
    std::vector<std::uint8_t> status =
        encodeResponse(1, Response{EnvironmentReply{.result = {.status = EnvironmentReadStatus::Ok, .variables = {}}}});
    status[4 + HEADER_BYTES] = 0xFF; // no such status
    EXPECT_FALSE(decodeResponse(status).has_value());

    std::vector<std::uint8_t> count =
        encodeResponse(1, Response{EnvironmentReply{.result = {.status = EnvironmentReadStatus::Ok, .variables = {}}}});
    count[4 + HEADER_BYTES + 4] = 0x7F; // claims far more variables than the protocol allows, and than follow
    EXPECT_FALSE(decodeResponse(count).has_value());
}

} // namespace
} // namespace Platform::Broker
