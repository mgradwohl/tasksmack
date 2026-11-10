/// @file test_BrokerProtocol.cpp
/// @brief The privileged-broker wire format (#2306): every message round-trips, and the decoder
/// refuses whatever isn't exactly a frame it knows. The helper reads what an unprivileged process
/// sends, so the refusals matter as much as the round trips.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <variant>
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

// #3063: v8's signals end at User2; the next value is refused, not sent as some other signal.
TEST(BrokerProtocolTest, ASignalPastTheLastIsRefused)
{
    std::vector<std::uint8_t> bytes = encodeRequest(1, Request{SignalRequest{.target = TARGET, .signal = Signal::User2}});
    ASSERT_TRUE(decodeRequest(bytes).has_value());
    bytes.back() = static_cast<std::uint8_t>(static_cast<std::uint8_t>(Signal::User2) + 1); // the signal is the payload's last byte
    EXPECT_FALSE(decodeRequest(bytes).has_value());
}

// #3071 (v9): Efficiency mode on and off round-trip; its flag is one byte of 0 or 1, and anything else is
// refused rather than read as on.
TEST(BrokerProtocolTest, EfficiencyModeRoundTripsAndRefusesAStrayFlag)
{
    for (const bool on : {true, false})
    {
        const Request request = EfficiencyModeRequest{.target = TARGET, .on = on};
        EXPECT_EQ(roundTrip(request), request) << on;
    }
    std::vector<std::uint8_t> bytes = encodeRequest(1, Request{EfficiencyModeRequest{.target = TARGET, .on = true}});
    EXPECT_EQ(bytes[6], static_cast<std::uint8_t>(MessageKind::SetEfficiencyMode));
    // The target's PID and start time, the flag, then the class an off restores (v21).
    ASSERT_EQ(bytes.size(), 4U + HEADER_BYTES + 4U + 8U + 1U + 4U);
    const std::size_t flagAt = 4U + HEADER_BYTES + 4U + 8U;
    bytes[flagAt] = 2;
    EXPECT_FALSE(decodeRequest(bytes).has_value());
    bytes[flagAt] = 1;
    bytes.pop_back(); // and a frame cut short of the class is short
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());
}

// #3327 (v22): an off carries the Windows priority class TaskSmack remembers the on replaced, all 32 bits, and an
// ActionResult the class an on replaced, so either side's memory survives the other's.
TEST(BrokerProtocolTest, EfficiencyModeCarriesTheClassToRestoreAndTheReplyTheClassReplaced)
{
    const Request off = EfficiencyModeRequest{.target = TARGET, .on = false, .restorePriorityClass = 0x89ABCDEFU};
    EXPECT_EQ(roundTrip(off), off);
    const Response replaced = ActionResult{.success = true, .errorMessage = {}, .successNote = {}, .priorityClassBefore = 0x80000000U};
    EXPECT_EQ(roundTrip(replaced), replaced);
    std::vector<std::uint8_t> bytes = encodeResponse(1, replaced);
    bytes.pop_back(); // a reply cut short of the class is refused
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeResponse(bytes).has_value());

    // ActionResult's class reaches ProcessActionResult and back.
    ProcessActionResult result = ProcessActionResult::ok();
    result.priorityClassBefore = 0x4000;
    EXPECT_EQ(ActionResult::from(result).priorityClassBefore, 0x4000U);
    EXPECT_EQ(ActionResult::from(result).toProcessActionResult().priorityClassBefore, 0x4000U);
}

// #3066: CloseConnection carries its target and the socket's inode, the full 64 bits.
TEST(BrokerProtocolTest, CloseConnectionRoundTrips)
{
    const Request request{CloseConnectionRequest{.target = TARGET, .socketInode = 0xFEDCBA9876543210ULL}};
    EXPECT_EQ(roundTrip(request), request);
    const std::vector<std::uint8_t> bytes = encodeRequest(3, request);
    EXPECT_EQ(Detail::readFrame(bytes).value_or(Detail::Frame{}).kind, MessageKind::CloseConnection);
}

// #3358 (v20): on Windows, CloseConnection names the IPv4 TCP connection by its endpoints, every byte and both ports.
TEST(BrokerProtocolTest, CloseConnectionCarriesTheTcpEndpoints)
{
    const Request request{CloseConnectionRequest{
        .target = TARGET,
        .socketInode = 0,
        .tcpConnection = {.localAddress = {10, 0, 0, 5}, .localPort = 0xFFFE, .remoteAddress = {142, 250, 80, 255}, .remotePort = 443},
    }};
    EXPECT_EQ(roundTrip(request), request);
    std::vector<std::uint8_t> bytes = encodeRequest(4, request);
    // The target, the inode, then 4 address bytes and a 2-byte port for each end.
    ASSERT_EQ(bytes.size(), 4U + HEADER_BYTES + 4U + 8U + 8U + 12U);
    bytes.pop_back(); // a frame cut short of the remote port is refused
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());
}

// #3069 (v14): one thread's Suspend, Resume or End carries its target, the TID and the action, byte for byte.
TEST(BrokerProtocolTest, ThreadActionRoundTripsAndRefusesWhatIsntOne)
{
    for (const ThreadAction action : {ThreadAction::Suspend, ThreadAction::Resume, ThreadAction::End})
    {
        const Request request{
            ThreadActionRequest{.target = TARGET, .tid = 0x7FFF'FFFC, .threadCreateTime = 0x01DC'0123'4567'89ABULL, .action = action}};
        EXPECT_EQ(roundTrip(request), request);
    }
    const std::vector<std::uint8_t> bytes = encodeRequest(
        7,
        Request{ThreadActionRequest{
            .target = {.pid = 0x0102, .startTimeTicks = 3}, .tid = 0x0405, .threadCreateTime = 0x0706, .action = ThreadAction::End}});
    // The header, the target (PID, then start time), the TID, its creation time and the action.
    const std::vector<std::uint8_t> expected{33, 0,  0, 0, static_cast<std::uint8_t>(PROTOCOL_VERSION),
                                             0,  25, 0, 7, 0,
                                             0,  0,  2, 1, 0,
                                             0,  3,  0, 0, 0,
                                             0,  0,  0, 0, 5,
                                             4,  0,  0, 6, 7,
                                             0,  0,  0, 0, 0,
                                             0,  2};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(Detail::readFrame(bytes).value_or(Detail::Frame{}).kind, MessageKind::ThreadAction);

    constexpr std::size_t ACTION = 4U + HEADER_BYTES + 4U + 8U + 4U + 8U;
    std::vector<std::uint8_t> bad = bytes;
    bad[ACTION] = 3; // past End
    EXPECT_FALSE(decodeRequest(bad).has_value());
    for (const std::int32_t tid : {0, -1})
    {
        EXPECT_FALSE(
            decodeRequest(encodeRequest(1, Request{ThreadActionRequest{.target = TARGET, .tid = tid, .action = ThreadAction::Suspend}}))
                .has_value())
            << tid;
    }
    bad = bytes;
    bad.pop_back(); // short
    bad[0] = static_cast<std::uint8_t>(bad.size() - 4);
    EXPECT_FALSE(decodeRequest(bad).has_value());
    bad = bytes;
    bad.push_back(0); // left over
    bad[0] = static_cast<std::uint8_t>(bad.size() - 4);
    EXPECT_FALSE(decodeRequest(bad).has_value());
}

// #3072 (v11): Replace Task Manager carries its change, the path and the confirmation flag; a path is
// carried exactly, quotes and all (the helper, not the decoder, refuses what isn't TaskSmack.exe).
TEST(BrokerProtocolTest, TaskManagerReplacementRoundTrips)
{
    for (const Request& request : {
             Request{TaskManagerReplacementRequest{
                 .change = TaskManagerChange::Set, .path = R"(C:\Program Files\TaskSmack\TaskSmack.exe)", .replaceOther = false}},
             Request{
                 TaskManagerReplacementRequest{.change = TaskManagerChange::Set, .path = R"(C:\Zoë\TaskSmack.exe)", .replaceOther = true}},
             Request{TaskManagerReplacementRequest{.change = TaskManagerChange::Set, .path = R"(x" & calc.exe ")", .replaceOther = false}},
             Request{TaskManagerReplacementRequest{.change = TaskManagerChange::Set, .path = {}, .replaceOther = false}},
             Request{TaskManagerReplacementRequest{.change = TaskManagerChange::Clear, .path = {}, .replaceOther = false}},
         })
    {
        EXPECT_EQ(roundTrip(request), request);
    }
    const std::vector<std::uint8_t> bytes =
        encodeRequest(5, Request{TaskManagerReplacementRequest{.change = TaskManagerChange::Set, .path = "ab", .replaceOther = true}});
    // The header, then the change, the path's length and bytes, and the flag.
    const std::vector<std::uint8_t> expected{16, 0,   0,   0, static_cast<std::uint8_t>(PROTOCOL_VERSION), 0, 23, 0, 5, 0, 0, 0, 0, 2, 0, 0,
                                             0,  'a', 'b', 1};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(Detail::readFrame(bytes).value_or(Detail::Frame{}).kind, MessageKind::TaskManagerReplacement);
}

// #3076 (v13): a session's Disconnect or Sign out carries the action, the session's WTS ID (the full 32 bits)
// and the sign-in the user confirmed (user, domain, sign-in time), since Windows reuses session IDs; a Lock
// (Linux's, never the helper's), an action past Disconnect, a short frame and a byte left over are refused.
TEST(BrokerProtocolTest, SessionActionRoundTripsAndRefusesALock)
{
    for (const Request& request : {
             Request{SessionActionRequest{
                 .action = SessionAction::SignOut, .sessionId = 2, .user = "alice", .domain = "PC", .logonUsec = 1'700'000'000'000'000ULL}},
             Request{SessionActionRequest{
                 .action = SessionAction::Disconnect, .sessionId = 0xFFFFFFFFU, .user = "Zoë", .domain = "CORP", .logonUsec = ~0ULL}},
             // The helper, not the decoder, refuses session 0 and a request that names no sign-in.
             Request{SessionActionRequest{.action = SessionAction::SignOut, .sessionId = 0, .user = {}, .domain = {}, .logonUsec = 0}},
         })
    {
        EXPECT_EQ(roundTrip(request), request);
    }
    std::vector<std::uint8_t> bytes = encodeRequest(5,
                                                    Request{SessionActionRequest{.action = SessionAction::Disconnect,
                                                                                 .sessionId = 0x01020304U,
                                                                                 .user = "ab",
                                                                                 .domain = {},
                                                                                 .logonUsec = 0x0102030405060708ULL}});
    // The header, then the action, the session's ID, the user, the domain and the sign-in time.
    const std::vector<std::uint8_t> expected{31, 0,   0,   0, static_cast<std::uint8_t>(PROTOCOL_VERSION),
                                             0,  24,  0,   5, 0,
                                             0,  0,   2,   4, 3,
                                             2,  1,   2,   0, 0,
                                             0,  'a', 'b', 0, 0,
                                             0,  0,   8,   7, 6,
                                             5,  4,   3,   2, 1};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(Detail::readFrame(bytes).value_or(Detail::Frame{}).kind, MessageKind::SessionAction);

    constexpr std::size_t ACTION = 4U + HEADER_BYTES;
    bytes[ACTION] = static_cast<std::uint8_t>(SessionAction::Lock);
    EXPECT_FALSE(decodeRequest(bytes).has_value()) << "a Lock";
    bytes[ACTION] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(SessionAction::Disconnect) + 1);
    EXPECT_FALSE(decodeRequest(bytes).has_value()) << "past Disconnect";
    bytes[ACTION] = static_cast<std::uint8_t>(SessionAction::SignOut);
    ASSERT_TRUE(decodeRequest(bytes).has_value());
    bytes.push_back(0); // a byte left over
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());
    bytes.resize(bytes.size() - 2); // the sign-in time short by a byte
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());
    EXPECT_FALSE(decodeResponse(encodeRequest(1, Request{SessionActionRequest{}})).has_value()) << "not a response";
}

// #3072: what the decoder refuses of a TaskManagerReplacement: a change past Clear, a flag that isn't 0 or 1,
// a Clear that carries a path or the flag (it has neither), a short frame and bytes left over.
TEST(BrokerProtocolTest, TaskManagerReplacementRefusesWhatIsntExactlyOne)
{
    const auto set = [](std::string path, bool replaceOther)
    {
        return encodeRequest(1,
                             Request{TaskManagerReplacementRequest{
                                 .change = TaskManagerChange::Set, .path = std::move(path), .replaceOther = replaceOther}});
    };
    std::vector<std::uint8_t> bytes = set("ab", false);
    ASSERT_TRUE(decodeRequest(bytes).has_value());
    constexpr std::size_t CHANGE = 4U + HEADER_BYTES;
    bytes[CHANGE] = 2; // past Clear
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("ab", false);
    bytes.back() = 2; // the flag
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("ab", false);
    bytes[CHANGE] = static_cast<std::uint8_t>(TaskManagerChange::Clear); // a Clear with a path
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("", true);
    bytes[CHANGE] = static_cast<std::uint8_t>(TaskManagerChange::Clear); // a Clear with the flag
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("ab", false);
    bytes.pop_back(); // no flag
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("ab", false);
    bytes.push_back(0); // a byte left over
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 4U);
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    bytes = set("ab", false);
    bytes[CHANGE + 1] = 0xFF; // a path longer than the frame
    EXPECT_FALSE(decodeRequest(bytes).has_value());

    // A path past the string cap is cut by the encoder (so it no longer ends in TaskSmack.exe, which the helper
    // refuses), and a frame that claims a longer one is refused by the decoder.
    const auto capped = decodeRequest(set(std::string(MAX_STRING_BYTES + 1, 'a'), false));
    ASSERT_TRUE(capped.has_value());
    const auto* cut = std::get_if<TaskManagerReplacementRequest>(&capped->message);
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(cut->path.size(), MAX_STRING_BYTES);
    std::vector<std::uint8_t> claims(4U + HEADER_BYTES + 1U + 4U + MAX_STRING_BYTES + 1U + 1U, 'a');
    const auto put32 = [&claims](std::size_t at, std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            claims[at + i] = static_cast<std::uint8_t>(value >> (8U * i));
        }
    };
    put32(0, static_cast<std::uint32_t>(claims.size() - 4U));
    claims[4] = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    claims[5] = 0;
    claims[6] = static_cast<std::uint8_t>(MessageKind::TaskManagerReplacement);
    claims[7] = 0;
    put32(8, 1);
    claims[CHANGE] = 0;
    put32(CHANGE + 1, static_cast<std::uint32_t>(MAX_STRING_BYTES + 1));
    claims.back() = 0;
    EXPECT_FALSE(decodeRequest(claims).has_value()) << "a path past the string cap";
    EXPECT_FALSE(decodeResponse(set("ab", false)).has_value()) << "not a response";
}

TEST(BrokerProtocolTest, EveryRequestRoundTrips)
{
    EXPECT_EQ(roundTrip(Request{Hello{}}), Request{Hello{}});
    for (const Signal signal : {Signal::Terminate,
                                Signal::Kill,
                                Signal::Suspend,
                                Signal::Resume,
                                Signal::Hangup,
                                Signal::Interrupt,
                                Signal::Quit,
                                Signal::User1,
                                Signal::User2}) // the last five from v8 (#3063)
    {
        const Request request = SignalRequest{.target = TARGET, .signal = signal};
        EXPECT_EQ(roundTrip(request), request);
    }
    const Request priority = PriorityRequest{.target = TARGET, .nice = -20};
    EXPECT_EQ(roundTrip(priority), priority);
    const Request io = IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 0};
    EXPECT_EQ(roundTrip(io), io);
    const Request hint = IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::WindowsHint, .level = 3}; // v18 (#3362)
    EXPECT_EQ(roundTrip(hint), hint);

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
    ASSERT_EQ(bytes.size(), 4U + HEADER_BYTES + 2U); // a Hello's payload: the version (v23, #3580: no forSession)
    EXPECT_EQ(bytes[0], HEADER_BYTES + 2U);          // length, low byte first
    EXPECT_EQ(bytes[4], PROTOCOL_VERSION & 0xFFU);
    EXPECT_EQ(bytes[6], static_cast<std::uint8_t>(MessageKind::Hello));
    EXPECT_EQ(bytes[8], 0x04U); // request id, little-endian
    EXPECT_EQ(bytes[11], 0x01U);
}

// #3076: a session action's result, a ServiceActionReply (#2728).
TEST(BrokerProtocolTest, ServiceActionRepliesRoundTripAndRefuseAStrayFlag)
{
    ServiceActionResult refused = ServiceActionResult::failed("Requires administrator");
    refused.needsAuthorization = true;
    const Response reply = ServiceActionReply{.result = refused};
    EXPECT_EQ(roundTrip(reply), reply);
    EXPECT_EQ(roundTrip(Response{ServiceActionReply{.result = ServiceActionResult::succeeded()}}),
              Response{ServiceActionReply{.result = ServiceActionResult::succeeded()}});

    // The reply's flags have four bits; any other is refused.
    std::vector<std::uint8_t> answer = encodeResponse(1, reply);
    answer.at(4 + HEADER_BYTES) = 16;
    EXPECT_FALSE(decodeResponse(answer).has_value());
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
    // An I/O class past WindowsHint (#3362): its byte follows the 12-byte target.
    std::vector<std::uint8_t> io =
        encodeRequest(1, Request{IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 0}});
    io[4 + HEADER_BYTES + 12] = 7;
    EXPECT_FALSE(decodeRequest(io).has_value());
    io[4 + HEADER_BYTES + 12] = static_cast<std::uint8_t>(IoPriorityClass::WindowsHint) + 1;
    EXPECT_FALSE(decodeRequest(io).has_value());
    io[4 + HEADER_BYTES + 12] = static_cast<std::uint8_t>(IoPriorityClass::WindowsHint);
    EXPECT_TRUE(decodeRequest(io).has_value());

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
    count[4 + HEADER_BYTES + 1 + 1 + 3] = 0x7F; // claims far more variables than the protocol allows, and than follow
    EXPECT_FALSE(decodeResponse(count).has_value());

    std::vector<std::uint8_t> truncated =
        encodeResponse(1, Response{EnvironmentReply{.result = {.status = EnvironmentReadStatus::Ok, .variables = {}}}});
    truncated[4 + HEADER_BYTES + 1] = 2; // a flag is 0 or 1
    EXPECT_FALSE(decodeResponse(truncated).has_value());
}

TEST(BrokerProtocolTest, ModulesAndOpenFilesRoundTrip)
{
    EXPECT_EQ(roundTrip(Request{ModulesRequest{.target = TARGET}}), Request{ModulesRequest{.target = TARGET}});
    EXPECT_EQ(roundTrip(Request{OpenFilesRequest{.target = TARGET}}), Request{OpenFilesRequest{.target = TARGET}});

    ModulesReadResult modules{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    modules.modules.push_back(
        {.path = "/usr/lib/libc.so.6", .baseAddress = 0x7f0000, .sizeBytes = 0x1000, .version = std::nullopt, .deleted = true});
    modules.modules.push_back({.path = "C:\\x.dll",
                               .baseAddress = 1,
                               .sizeBytes = 2,
                               .version = ModuleVersion{.major = 10, .minor = 0, .build = 19041, .revision = 1},
                               .deleted = false});
    const Response modulesReply = ModulesReply{.result = modules};
    EXPECT_EQ(roundTrip(modulesReply), modulesReply);

    OpenFilesReadResult files;
    files.status = OpenFilesReadStatus::Ok;
    files.namesIncomplete = true;
    files.files.push_back({.descriptor = 3, .kind = OpenFileKind::Socket, .path = "socket:[1234]", .flags = 02, .deleted = false});
    files.files.push_back(
        {.descriptor = 4, .kind = OpenFileKind::File, .path = "/tmp/x (deleted)", .flags = std::nullopt, .deleted = true});
    const Response filesReply = OpenFilesReply{.result = files};
    EXPECT_EQ(roundTrip(filesReply), filesReply);
}

// #3069: another user's process's threads, as the helper reads them.
TEST(BrokerProtocolTest, ThreadsRoundTripAndBadRepliesAreRefused)
{
    EXPECT_EQ(roundTrip(Request{ThreadsRequest{.target = TARGET}}), Request{ThreadsRequest{.target = TARGET}});
    ThreadsReadResult threads{.status = ThreadsReadStatus::Ok, .threads = {}, .detail = {}};
    threads.ticksPerSecond = 100;
    threads.bootTimeEpoch = 1'761'900'000;
    threads.truncated = true;
    threads.threads.push_back(
        {.tid = 4242, .name = "server", .state = 'S', .userTicks = 5, .systemTicks = 6, .nice = -5, .startTicks = 4242});
    threads.threads.push_back(
        {.tid = 4250, .name = "pool) worker", .state = 'R', .userTicks = 300, .systemTicks = 40, .nice = 19, .startTicks = 5000});
    const Response reply = ThreadsReply{.result = threads};
    EXPECT_EQ(roundTrip(reply), reply);
    // v14 (#3069): Windows' rows, with their priorities, wait reasons and suspend counts.
    ThreadsReadResult windows = threads;
    windows.windowsPriorities = true;
    windows.threads[0].waitReason = "UserRequest";
    windows.threads[1].state = 'T';
    windows.threads[1].waitReason = "Suspended";
    windows.threads[1].suspendCount = 3;
    windows.threads[1].createTime = 0x01DC'0000'0000'0042ULL;
    const Response windowsReply = ThreadsReply{.result = windows};
    EXPECT_EQ(roundTrip(windowsReply), windowsReply);
    const Response refused = ThreadsReply{.result = {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = "no"}};
    EXPECT_EQ(roundTrip(refused), refused);

    // A status past Failed, or no tick rate, is refused.
    std::vector<std::uint8_t> badStatus = encodeResponse(1, reply);
    badStatus[4 + HEADER_BYTES] = static_cast<std::uint8_t>(ThreadsReadStatus::Failed) + 1;
    EXPECT_FALSE(decodeResponse(badStatus).has_value());
    ThreadsReadResult noTicks = threads;
    noTicks.ticksPerSecond = 0;
    EXPECT_FALSE(decodeResponse(encodeResponse(1, Response{ThreadsReply{.result = noTicks}})).has_value());
    // So is a count past MAX_THREADS (status, empty detail, ticks, boot time, truncated, Windows priorities,
    // then the count).
    std::vector<std::uint8_t> tooMany =
        encodeResponse(1, Response{ThreadsReply{.result = {.status = ThreadsReadStatus::Ok, .threads = {}, .detail = {}}}});
    const std::size_t countAt = 4 + HEADER_BYTES + 1 + 4 + 8 + 8 + 1 + 1;
    const auto huge = static_cast<std::uint32_t>(MAX_THREADS + 1);
    for (std::size_t i = 0; i < 4; ++i)
    {
        tooMany[countAt + i] = static_cast<std::uint8_t>(huge >> (8U * i));
    }
    EXPECT_FALSE(decodeResponse(tooMany).has_value());
}

TEST(BrokerProtocolTest, OpenFilesPastOneFrameAreCutAndSaySo)
{
    OpenFilesReadResult files;
    files.status = OpenFilesReadStatus::Ok;
    const std::string longPath(MAX_STRING_BYTES, 'p'); // 64 KiB each: 8 MiB holds about 127
    for (std::uint64_t fd = 0; fd < 200; ++fd)
    {
        files.files.push_back({.descriptor = fd, .kind = OpenFileKind::File, .path = longPath, .flags = std::nullopt, .deleted = false});
    }
    const std::vector<std::uint8_t> bytes = encodeResponse(1, Response{OpenFilesReply{.result = files}});
    EXPECT_LE(bytes.size(), MAX_MESSAGE_BYTES);
    const auto decoded = decodeResponse(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    const auto& result = std::get<OpenFilesReply>(decoded->message).result;
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(result.files.size(), 200U);
    EXPECT_GT(result.files.size(), 100U);
}

// #2403: a process mapping tens of thousands of distinct files (Elasticsearch, Kafka) or carrying a huge
// environment made a reply past MAX_MESSAGE_BYTES, which the helper couldn't send, so it ended the session.
// The lists stop short of one frame, as Open files' does, and say they were cut.
TEST(BrokerProtocolTest, ModulesPastOneFrameAreCutAndSaySo)
{
    ModulesReadResult modules{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    const std::string path(200, 'm'); // 65536 of them: about 15 MiB on the wire uncut
    modules.modules.assign(MAX_MODULES,
                           {.path = path, .baseAddress = 0x7f0000, .sizeBytes = 0x1000, .version = std::nullopt, .deleted = false});
    const std::vector<std::uint8_t> bytes = encodeResponse(1, Response{ModulesReply{.result = modules}});
    EXPECT_LE(bytes.size(), MAX_MESSAGE_BYTES);
    const auto decoded = decodeResponse(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    const ModulesReadResult& result = std::get<ModulesReply>(decoded->message).result;
    EXPECT_EQ(result.status, ModulesReadStatus::Ok);
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(result.modules.size(), MAX_MODULES);
    EXPECT_GT(result.modules.size(), MAX_MODULES / 2); // cut at the frame, not far short of it
    EXPECT_EQ(result.modules.front().path, path);
    EXPECT_EQ(result.modules.back().path, path);
}

TEST(BrokerProtocolTest, EnvironmentPastOneFrameIsCutAndSaysSo)
{
    EnvironmentReadResult environment{.status = EnvironmentReadStatus::Ok, .variables = {}};
    const std::string text(200, 'e'); // 65536 names and values: about 26 MiB on the wire uncut
    environment.variables.assign(MAX_ENVIRONMENT_VARIABLES, {.name = text, .value = text});
    const std::vector<std::uint8_t> bytes = encodeResponse(1, Response{EnvironmentReply{.result = environment}});
    EXPECT_LE(bytes.size(), MAX_MESSAGE_BYTES);
    const auto decoded = decodeResponse(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    const EnvironmentReadResult& result = std::get<EnvironmentReply>(decoded->message).result;
    EXPECT_EQ(result.status, EnvironmentReadStatus::Ok);
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(result.variables.size(), MAX_ENVIRONMENT_VARIABLES);
    EXPECT_GT(result.variables.size(), MAX_ENVIRONMENT_VARIABLES / 4);
    EXPECT_EQ(result.variables.back().value, text);
}

TEST(BrokerProtocolTest, ListsPastTheirCountCapSayTheyWereCut)
{
    ModulesReadResult modules{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    modules.modules.resize(MAX_MODULES + 1);
    const Response moduleReply = roundTrip(ModulesReply{.result = modules});
    EXPECT_TRUE(std::get<ModulesReply>(moduleReply).result.truncated);
    EXPECT_EQ(std::get<ModulesReply>(moduleReply).result.modules.size(), MAX_MODULES);

    EnvironmentReadResult environment{.status = EnvironmentReadStatus::Ok, .variables = {}};
    environment.variables.resize(MAX_ENVIRONMENT_VARIABLES + 1);
    const Response environmentReply = roundTrip(EnvironmentReply{.result = environment});
    EXPECT_TRUE(std::get<EnvironmentReply>(environmentReply).result.truncated);
    EXPECT_EQ(std::get<EnvironmentReply>(environmentReply).result.variables.size(), MAX_ENVIRONMENT_VARIABLES);
}

TEST(BrokerProtocolTest, AWholeListIsNotCutAndACutReadStaysCut)
{
    ModulesReadResult modules{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    modules.modules.push_back({.path = "/usr/lib/libc.so.6", .baseAddress = 1, .sizeBytes = 2, .version = std::nullopt, .deleted = false});
    EXPECT_FALSE(std::get<ModulesReply>(roundTrip(ModulesReply{.result = modules})).result.truncated);
    modules.truncated = true; // the reader itself kept only the first
    const Response cutModules = ModulesReply{.result = modules};
    EXPECT_EQ(roundTrip(cutModules), cutModules);

    EnvironmentReadResult environment{.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}}};
    EXPECT_FALSE(std::get<EnvironmentReply>(roundTrip(EnvironmentReply{.result = environment})).result.truncated);
    environment.truncated = true;
    const Response cutEnvironment = EnvironmentReply{.result = environment};
    EXPECT_EQ(roundTrip(cutEnvironment), cutEnvironment);
}

TEST(BrokerProtocolTest, AnOpenFileOfNoKnownKindIsRefused)
{
    OpenFilesReadResult files;
    files.status = OpenFilesReadStatus::Ok;
    files.files.push_back({.descriptor = 3, .kind = OpenFileKind::Other, .path = "", .flags = std::nullopt, .deleted = false});
    std::vector<std::uint8_t> bytes = encodeResponse(1, Response{OpenFilesReply{.result = files}});
    // status, detail (4), three flags, count (4), descriptor (8), then the kind
    bytes[4 + HEADER_BYTES + 1 + 4 + 3 + 4 + 8] = 0xEE;
    EXPECT_FALSE(decodeResponse(bytes).has_value());
}

TEST(BrokerProtocolTest, ConnectionsRoundTripAndBadEnumsAreRefused)
{
    EXPECT_EQ(roundTrip(Request{ConnectionsRequest{.target = TARGET}}), Request{ConnectionsRequest{.target = TARGET}});
    ConnectionsReadResult result{.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
    ProcessConnection tcp;
    tcp.protocol = ConnectionProtocol::Tcp;
    tcp.family = ConnectionFamily::IPv6;
    tcp.local.address[15] = 1; // ::1
    tcp.local.port = 8080;
    tcp.state = ConnectionState::Listen;
    result.connections.push_back(tcp);
    ProcessConnection udp;
    udp.protocol = ConnectionProtocol::Udp;
    udp.remote.address = {192, 168, 1, 2};
    udp.remote.port = 53;
    udp.state = ConnectionState::Established;
    result.connections.push_back(udp);
    const Response reply = ConnectionsReply{.result = result};
    EXPECT_EQ(roundTrip(reply), reply);

    std::vector<std::uint8_t> bytes = encodeResponse(1, reply);
    // status, detail (4), count (4), then the first socket's protocol
    bytes[4 + HEADER_BYTES + 1 + 4 + 4] = 7;
    EXPECT_FALSE(decodeResponse(bytes).has_value());
}

// #2417: the decoders' refusals of a well-framed message that is the wrong way round or out of range.
TEST(BrokerProtocolTest, NoResponseDecodesAsARequestAndNoRequestAsAResponse)
{
    const std::vector<Response> responses = {
        HelloReply{.version = PROTOCOL_VERSION, .accepted = true},
        ActionResult{.success = true, .errorMessage = {}, .successNote = {}},
        ErrorReply{.message = "no"},
        EnvironmentReply{.result = {.status = EnvironmentReadStatus::Ok, .variables = {}}},
        ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}},
        OpenFilesReply{},
        ConnectionsReply{.result = {.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}}},
        ThreadsReply{.result = {.status = ThreadsReadStatus::Ok, .threads = {}, .detail = {}}},
        ServiceActionReply{},
    };
    for (const Response& response : responses)
    {
        const auto decoded = decodeRequest(encodeResponse(3, response));
        ASSERT_FALSE(decoded.has_value()) << response.index();
        EXPECT_EQ(decoded.error(), "Not a request this helper does");
    }

    const std::vector<Request> requests = {
        Hello{},
        SignalRequest{.target = TARGET, .signal = Signal::Kill},
        PriorityRequest{.target = TARGET, .nice = 5},
        IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 0},
        AffinityRequest{.target = TARGET, .cpus = CpuAffinity::fromMask(1)},
        EfficiencyModeRequest{.target = TARGET, .on = true},
        EnvironmentRequest{.target = TARGET},
        ModulesRequest{.target = TARGET},
        OpenFilesRequest{.target = TARGET},
        ConnectionsRequest{.target = TARGET},
        ThreadsRequest{.target = TARGET},
        ThreadActionRequest{.target = TARGET, .tid = 9, .action = ThreadAction::Resume},
    };
    for (const Request& request : requests)
    {
        const auto decoded = decodeResponse(encodeRequest(3, request));
        ASSERT_FALSE(decoded.has_value()) << request.index();
        EXPECT_EQ(decoded.error(), "Not a response");
    }
}

TEST(BrokerProtocolTest, AResponseFrameThatIsntOneIsRefusedWithTheFramesReason)
{
    const std::vector<std::uint8_t> shortFrame(3, 0);
    const auto tooShort = decodeResponse(shortFrame);
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error(), "A frame shorter than its header");

    const std::vector<std::uint8_t> huge(MAX_MESSAGE_BYTES + 1, 0);
    const auto tooLong = decodeRequest(huge);
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_EQ(tooLong.error(), "A frame larger than the protocol allows");
}

TEST(BrokerProtocolTest, ReplyListsWithABadStatusOrTooManyEntriesAreRefused)
{
    constexpr std::size_t PAYLOAD = 4 + HEADER_BYTES;
    const auto refused = [](std::vector<std::uint8_t> bytes, std::size_t at, std::uint8_t value)
    {
        bytes[at] = value;
        return !decodeResponse(bytes).has_value();
    };

    // Modules: status, detail (4), truncated, count (4).
    const std::vector<std::uint8_t> modules =
        encodeResponse(1, ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}});
    ASSERT_TRUE(decodeResponse(modules).has_value());
    EXPECT_TRUE(refused(modules, PAYLOAD, 0xEE));
    EXPECT_TRUE(refused(modules, PAYLOAD + 1 + 4, 2)); // a flag is 0 or 1
    EXPECT_TRUE(refused(modules, PAYLOAD + 1 + 4 + 1 + 3, 0x7F));

    // Open files: status, detail (4), three flags, count (4).
    OpenFilesReadResult files;
    files.status = OpenFilesReadStatus::Ok;
    const std::vector<std::uint8_t> openFiles = encodeResponse(1, OpenFilesReply{.result = files});
    ASSERT_TRUE(decodeResponse(openFiles).has_value());
    EXPECT_TRUE(refused(openFiles, PAYLOAD, 0xEE));
    EXPECT_TRUE(refused(openFiles, PAYLOAD + 1 + 4 + 3 + 3, 0x7F));

    // Connections: status, detail (4), count (4), then per socket protocol, family, two endpoints
    // (16-byte address and port each) and the state.
    ConnectionsReadResult sockets{.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
    ProcessConnection tcp;
    tcp.protocol = ConnectionProtocol::Tcp;
    tcp.state = ConnectionState::Listen;
    sockets.connections.push_back(tcp);
    const std::vector<std::uint8_t> connections = encodeResponse(1, ConnectionsReply{.result = sockets});
    ASSERT_TRUE(decodeResponse(connections).has_value());
    EXPECT_TRUE(refused(connections, PAYLOAD, 0xEE));
    EXPECT_TRUE(refused(connections, PAYLOAD + 1 + 4 + 3, 0x7F));
    EXPECT_TRUE(refused(connections, PAYLOAD + 1 + 4 + 4 + 1 + 1 + 18 + 18, 0xEE)); // no such state
}

TEST(BrokerProtocolTest, RepliesCompareByEveryFigure)
{
    const EnvironmentReply home{.result = {.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}}}};
    EnvironmentReply otherValue = home;
    otherValue.result.variables[0].value = "/home/u";
    EnvironmentReply otherName = home;
    otherName.result.variables[0].name = "PATH";
    EnvironmentReply otherStatus = home;
    otherStatus.result.status = EnvironmentReadStatus::Failed;
    EnvironmentReply fewer = home;
    fewer.result.variables.clear();
    EnvironmentReply cut = home;
    cut.result.truncated = true;
    EXPECT_FALSE(home == cut);
    const ModulesReply wholeModules;
    ModulesReply cutModules;
    cutModules.result.truncated = true;
    EXPECT_FALSE(wholeModules == cutModules);
    EXPECT_EQ(home, home);
    EXPECT_FALSE(home == otherValue);
    EXPECT_FALSE(home == otherName);
    EXPECT_FALSE(home == otherStatus);
    EXPECT_FALSE(home == fewer);
}

TEST(BrokerProtocolTest, TheWriterCountsWhatItHasWritten)
{
    Detail::Writer writer;
    EXPECT_EQ(writer.size(), 0U);
    writer.u8(1);
    writer.u32(2);
    writer.text("abc");
    EXPECT_EQ(writer.size(), 1U + 4U + 4U + 3U);
    EXPECT_EQ(writer.frame(MessageKind::Error, 1).size(), 4U + HEADER_BYTES + writer.size());
}

// #2643: fuzz_broker_protocol's seed corpus has a seed for every message kind, and each one still
// decodes. libFuzzer rarely builds a well-formed nested list reply from nothing, so a kind without a
// seed is barely fuzzed; a protocol change that breaks a seed, or a new kind, fails here instead.
TEST(BrokerProtocolTest, TheFuzzCorpusHasADecodableSeedForEveryMessageKind)
{
    std::set<MessageKind> seeded;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(TASKSMACK_BROKER_FUZZ_CORPUS_DIR))
    {
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto frame = Detail::readFrame(bytes);
        ASSERT_TRUE(frame.has_value()) << entry.path().filename() << ": " << frame.error();
        EXPECT_TRUE(decodeRequest(bytes).has_value() || decodeResponse(bytes).has_value()) << entry.path().filename() << " doesn't decode";
        seeded.insert(frame->kind);
    }
    for (const MessageKind kind : {MessageKind::Hello,
                                   MessageKind::HelloReply,
                                   MessageKind::Signal,
                                   MessageKind::SetPriority,
                                   MessageKind::SetIoPriority,
                                   MessageKind::SetAffinity,
                                   MessageKind::SetEfficiencyMode,
                                   MessageKind::CloseConnection,
                                   MessageKind::TaskManagerReplacement,
                                   MessageKind::SessionAction,
                                   MessageKind::ThreadAction,
                                   MessageKind::ReadEnvironment,
                                   MessageKind::ReadModules,
                                   MessageKind::ReadOpenFiles,
                                   MessageKind::ReadConnections,
                                   MessageKind::ReadThreads,
                                   MessageKind::ActionResult,
                                   MessageKind::EnvironmentResult,
                                   MessageKind::ModulesResult,
                                   MessageKind::OpenFilesResult,
                                   MessageKind::ConnectionsResult,
                                   MessageKind::ServiceActionResult,
                                   MessageKind::ThreadsResult,
                                   MessageKind::Error})
    {
        EXPECT_TRUE(seeded.contains(kind)) << "no seed for kind " << static_cast<int>(kind);
    }
}

/// @p base equals itself, and each of @p change, which alters one field of a copy, makes the copy
/// unequal: so == compares every field, and each comparison in it is seen both equal and not (#3221).
template<typename Message, typename... Change> void expectEveryFieldCompared(const Message& base, Change... change)
{
    EXPECT_TRUE(base == base);
    std::size_t field = 0;
    const auto differs = [&base, &field](const auto& alter)
    {
        Message other = base;
        alter(other);
        EXPECT_FALSE(base == other) << "field " << field;
        ++field;
    };
    (differs(change), ...);
}

// The replies whose == is written out, not defaulted (a defaulted one compares its members in order).
TEST(BrokerProtocolTest, ResultRepliesCompareByEveryField)
{
    expectEveryFieldCompared(
        ServiceActionReply{
            .result = {.ok = true, .message = "m", .cancelled = false, .needsAuthorization = false, .authorizationUnavailable = false}},
        [](ServiceActionReply& m) { m.result.ok = false; },
        [](ServiceActionReply& m) { m.result.message = "n"; },
        [](ServiceActionReply& m) { m.result.cancelled = true; },
        [](ServiceActionReply& m) { m.result.needsAuthorization = true; },
        [](ServiceActionReply& m) { m.result.authorizationUnavailable = true; });
}

TEST(BrokerProtocolTest, ListRepliesCompareByEveryFieldOfEveryEntry)
{
    ModulesReply modules;
    modules.result = {
        .status = ModulesReadStatus::Ok,
        .modules = {ProcessModule{.path = "a.dll", .baseAddress = 1, .sizeBytes = 2, .version = ModuleVersion{}, .deleted = false}},
        .detail = "d",
        .truncated = false};
    expectEveryFieldCompared(
        modules,
        [](ModulesReply& m) { m.result.status = ModulesReadStatus::Failed; },
        [](ModulesReply& m) { m.result.detail = "e"; },
        [](ModulesReply& m) { m.result.truncated = true; },
        [](ModulesReply& m) { m.result.modules[0].path = "b.dll"; },
        [](ModulesReply& m) { m.result.modules[0].baseAddress = 9; },
        [](ModulesReply& m) { m.result.modules[0].sizeBytes = 9; },
        [](ModulesReply& m) { m.result.modules[0].version.reset(); },
        [](ModulesReply& m) { m.result.modules[0].deleted = true; });

    OpenFilesReply files;
    files.result.status = OpenFilesReadStatus::Ok;
    files.result.files = {OpenFile{.descriptor = 3, .kind = OpenFileKind::File, .path = "/f", .flags = 2U, .deleted = false}};
    files.result.detail = "d";
    expectEveryFieldCompared(
        files,
        [](OpenFilesReply& m) { m.result.status = OpenFilesReadStatus::Failed; },
        [](OpenFilesReply& m) { m.result.detail = "e"; },
        [](OpenFilesReply& m) { m.result.truncated = true; },
        [](OpenFilesReply& m) { m.result.namesIncomplete = true; },
        [](OpenFilesReply& m) { m.result.hexDescriptors = true; },
        [](OpenFilesReply& m) { m.result.files[0].descriptor = 4; },
        [](OpenFilesReply& m) { m.result.files[0].kind = OpenFileKind::Pipe; },
        [](OpenFilesReply& m) { m.result.files[0].path = "/g"; },
        [](OpenFilesReply& m) { m.result.files[0].flags.reset(); },
        [](OpenFilesReply& m) { m.result.files[0].deleted = true; });

    ThreadsReply threads;
    threads.result.status = ThreadsReadStatus::Ok;
    threads.result.threads = {
        ProcessThread{.tid = 1, .name = "a", .state = 'S', .userTicks = 1, .systemTicks = 1, .nice = 0, .startTicks = 1}};
    threads.result.detail = "d";
    expectEveryFieldCompared(
        threads,
        [](ThreadsReply& m) { m.result.status = ThreadsReadStatus::Failed; },
        [](ThreadsReply& m) { m.result.detail = "e"; },
        [](ThreadsReply& m) { m.result.ticksPerSecond = 250; },
        [](ThreadsReply& m) { m.result.truncated = true; },
        [](ThreadsReply& m) { m.result.bootTimeEpoch = 9; },
        [](ThreadsReply& m) { m.result.threads[0].tid = 2; },
        [](ThreadsReply& m) { m.result.threads[0].name = "b"; },
        [](ThreadsReply& m) { m.result.threads[0].state = 'R'; },
        [](ThreadsReply& m) { m.result.threads[0].userTicks = 2; },
        [](ThreadsReply& m) { m.result.threads[0].systemTicks = 2; },
        [](ThreadsReply& m) { m.result.threads[0].nice = 5; },
        [](ThreadsReply& m) { m.result.threads[0].startTicks = 2; },
        [](ThreadsReply& m) { m.result.windowsPriorities = true; },
        [](ThreadsReply& m) { m.result.threads[0].waitReason = "Suspended"; },
        [](ThreadsReply& m) { m.result.threads[0].suspendCount = 1; },
        [](ThreadsReply& m) { m.result.threads[0].createTime = 7; });

    ProcessConnection connection;
    connection.local.port = 80;
    connection.remote.port = 5000;
    connection.state = ConnectionState::Established;
    expectEveryFieldCompared(
        ConnectionsReply{.result = {.status = ConnectionsReadStatus::Ok, .connections = {connection}, .detail = "d"}},
        [](ConnectionsReply& m) { m.result.status = ConnectionsReadStatus::Failed; },
        [](ConnectionsReply& m) { m.result.detail = "e"; },
        [](ConnectionsReply& m) { m.result.connections[0].protocol = ConnectionProtocol::Udp; },
        [](ConnectionsReply& m) { m.result.connections[0].family = ConnectionFamily::IPv6; },
        [](ConnectionsReply& m) { m.result.connections[0].local.address[0] = 1; },
        [](ConnectionsReply& m) { m.result.connections[0].local.port = 81; },
        [](ConnectionsReply& m) { m.result.connections[0].remote.address[0] = 1; },
        [](ConnectionsReply& m) { m.result.connections[0].remote.port = 5001; },
        [](ConnectionsReply& m) { m.result.connections[0].state = ConnectionState::Unknown; });
}

// #3221: a reader that has already failed reads no text, and a text longer than any may be fails it.
TEST(BrokerProtocolTest, AFailedReaderReadsNoTextAndATextPastItsCapFails)
{
    const std::vector<std::uint8_t> twoBytes{1, 2};
    Detail::Reader text(twoBytes);
    static_cast<void>(text.u32()); // past the end: the reader fails
    EXPECT_FALSE(text.ok());
    EXPECT_TRUE(text.text().empty());
    EXPECT_FALSE(text.ok());

    // A text longer than any may be.
    Detail::Writer longText;
    longText.u32(static_cast<std::uint32_t>(MAX_STRING_BYTES + 1));
    const std::vector<std::uint8_t> declared = longText.frame(MessageKind::Error, 1);
    Detail::Reader tooLong(std::span(declared).subspan(4 + HEADER_BYTES));
    EXPECT_TRUE(tooLong.text().empty());
    EXPECT_FALSE(tooLong.ok());
}

// #3221: a message whose payload ends before its fields do is refused as malformed, as a request and as
// a response, and a list request whose entries run out is too.
TEST(BrokerProtocolTest, APayloadThatEndsEarlyIsAMalformedMessage)
{
    Detail::Writer shortSignal;
    shortSignal.i32(TARGET.pid); // the start time and the signal are missing
    const auto request = decodeRequest(shortSignal.frame(MessageKind::Signal, 1));
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), "A malformed message");

    Detail::Writer shortReply;
    shortReply.u8(1); // HelloReply's version is two bytes
    const auto response = decodeResponse(shortReply.frame(MessageKind::HelloReply, 1));
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "A malformed message");

    Detail::Writer fewerWords;
    fewerWords.target(TARGET);
    fewerWords.u32(3);
    fewerWords.u64(1); // one affinity word of the three it says
    EXPECT_FALSE(decodeRequest(fewerWords.frame(MessageKind::SetAffinity, 1)).has_value());

    // And bytes left over after the last field are refused the same way.
    Detail::Writer trailing;
    trailing.u16(PROTOCOL_VERSION);
    trailing.u8(0); // one byte more than a Hello has (v23, #3580: its version only)
    const auto extra = decodeRequest(trailing.frame(MessageKind::Hello, 1));
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error(), "A malformed message");
}

// #3221: the flags each encoder writes as set read back set, so neither value of any of them is lost.
TEST(BrokerProtocolTest, EveryFlagAnEncoderWritesRoundTripsSet)
{
    OpenFilesReply files;
    files.result.status = OpenFilesReadStatus::Ok;
    files.result.truncated = true;
    files.result.hexDescriptors = true;
    const Response openFiles = roundTrip(Response{files});
    ASSERT_TRUE(std::holds_alternative<OpenFilesReply>(openFiles));
    EXPECT_TRUE(std::get<OpenFilesReply>(openFiles).result.truncated);
    EXPECT_TRUE(std::get<OpenFilesReply>(openFiles).result.hexDescriptors);

    for (const ServiceActionResult& outcome :
         {ServiceActionResult{
              .ok = false, .message = "m", .cancelled = false, .needsAuthorization = true, .authorizationUnavailable = false},
          ServiceActionResult{
              .ok = false, .message = "m", .cancelled = false, .needsAuthorization = false, .authorizationUnavailable = true},
          ServiceActionResult{
              .ok = false, .message = "m", .cancelled = true, .needsAuthorization = false, .authorizationUnavailable = false}})
    {
        const Response service = roundTrip(Response{ServiceActionReply{.result = outcome}});
        ASSERT_TRUE(std::holds_alternative<ServiceActionReply>(service));
        EXPECT_EQ(std::get<ServiceActionReply>(service), ServiceActionReply{.result = outcome});
    }
}

// #3221: the reply lists' caps and enums, refused when the count or a value is past them.
TEST(BrokerProtocolTest, ReplyListsPastTheirCapsOrWithAStrayFamilyAreRefused)
{
    // Modules: status, an empty detail, truncated, then a count past MAX_MODULES.
    Detail::Writer modules;
    modules.u8(0);
    modules.text({});
    modules.u8(0);
    modules.u32(static_cast<std::uint32_t>(MAX_MODULES + 1));
    EXPECT_FALSE(decodeResponse(modules.frame(MessageKind::ModulesResult, 1)).has_value());

    // Status, an empty detail and a count, then the connection's protocol and family.
    constexpr std::size_t FAMILY = 4 + HEADER_BYTES + 1 + 4 + 4 + 1;
    std::vector<std::uint8_t> connections = encodeResponse(
        1, ConnectionsReply{.result = {.status = ConnectionsReadStatus::Ok, .connections = {ProcessConnection{}}, .detail = {}}});
    ASSERT_TRUE(decodeResponse(connections).has_value());
    connections[FAMILY] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(ConnectionFamily::IPv6) + 1);
    EXPECT_FALSE(decodeResponse(connections).has_value());
}

// #3221: the requests the newest kinds name aren't responses, and a kind neither side knows is neither.
TEST(BrokerProtocolTest, TheNewerRequestKindsAndAnUnknownKindAreNotResponses)
{
    for (const Request& request :
         {Request{CloseConnectionRequest{.target = TARGET, .socketInode = 5}},
          Request{TaskManagerReplacementRequest{.change = TaskManagerChange::Clear, .path = {}, .replaceOther = false}},
          Request{SessionActionRequest{.action = SessionAction::SignOut, .sessionId = 2, .user = "u", .domain = "d", .logonUsec = 1}},
          Request{ThreadActionRequest{.target = TARGET, .tid = 9, .action = ThreadAction::End}}})
    {
        const auto decoded = decodeResponse(encodeRequest(3, request));
        ASSERT_FALSE(decoded.has_value()) << request.index();
        EXPECT_EQ(decoded.error(), "Not a response");
    }
    Detail::Writer empty;
    const std::vector<std::uint8_t> unknown = empty.frame(static_cast<MessageKind>(999), 1);
    EXPECT_FALSE(decodeResponse(unknown).has_value());
    EXPECT_FALSE(decodeRequest(unknown).has_value());
}

// #3221: a list's room is a frame's whole, less its length, header and the reply's fixed fields.
TEST(BrokerProtocolTest, AListsRoomIsTheFrameLessItsFixedBytes)
{
    for (const std::size_t fixed : {std::size_t{0}, std::size_t{1} + 4})
    {
        EXPECT_EQ(Detail::listRoom(fixed), MAX_MESSAGE_BYTES - (4 + HEADER_BYTES) - fixed);
    }
}

} // namespace
} // namespace Platform::Broker
