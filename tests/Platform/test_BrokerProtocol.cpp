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
#include "Platform/IProcessProbe.h"

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

TEST(BrokerProtocolTest, EveryRequestRoundTrips)
{
    EXPECT_EQ(roundTrip(Request{Hello{}}), Request{Hello{}});
    EXPECT_EQ(roundTrip(Request{Hello{.forSession = true}}), Request{Hello{.forSession = true}});
    const Request counters = ProcessCountersRequest{.targets = {TARGET, ProcessTarget{.pid = 7, .startTimeTicks = 9}}};
    EXPECT_EQ(roundTrip(counters), counters);
    const Request withSockets = ProcessCountersRequest{.targets = {TARGET}, .withSockets = true};
    EXPECT_EQ(roundTrip(withSockets), withSockets);
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
    ASSERT_EQ(bytes.size(), 4U + HEADER_BYTES + 3U); // a Hello's payload: the version, then forSession
    EXPECT_EQ(bytes[0], HEADER_BYTES + 3U);          // length, low byte first
    EXPECT_EQ(bytes[4], PROTOCOL_VERSION & 0xFFU);
    EXPECT_EQ(bytes[6], static_cast<std::uint8_t>(MessageKind::Hello));
    EXPECT_EQ(bytes[8], 0x04U); // request id, little-endian
    EXPECT_EQ(bytes[11], 0x01U);
}

TEST(BrokerProtocolTest, ProcessCountersRoundTripAndRefuseTooManyOrAStrayFlag)
{
    const Response reply = ProcessCountersReply{
        .entries = {
            ProcessCountersEntry{
                .target = TARGET, .handleCountRead = true, .handleCount = 57, .ioRead = true, .readBytes = 123456, .writeBytes = 7890},
            ProcessCountersEntry{.target = {.pid = 7, .startTimeTicks = 9}, .handleCountRead = true, .handleCount = 3},
            ProcessCountersEntry{.target = {.pid = 8, .startTimeTicks = 9}, .socketsRead = true, .socketInodes = {11, 12}},
            ProcessCountersEntry{.target = {.pid = 9, .startTimeTicks = 9}, .socketsRead = true, .socketInodes = {}},
        }};
    EXPECT_EQ(roundTrip(reply), reply);
    EXPECT_EQ(roundTrip(Response{ProcessCountersReply{}}), Response{ProcessCountersReply{}});

    // A count past MAX_COUNTER_TARGETS is refused before any target is read.
    std::vector<std::uint8_t> request = encodeRequest(1, Request{ProcessCountersRequest{.targets = {TARGET}}});
    const std::uint32_t tooMany = static_cast<std::uint32_t>(MAX_COUNTER_TARGETS) + 1;
    std::memcpy(request.data() + 4 + HEADER_BYTES + 1, &tooMany, sizeof(tooMany)); // after withSockets
    EXPECT_FALSE(decodeRequest(request).has_value());
    // withSockets is one byte of zero or one.
    request = encodeRequest(1, Request{ProcessCountersRequest{.targets = {TARGET}}});
    request.at(4 + HEADER_BYTES) = 2;
    EXPECT_FALSE(decodeRequest(request).has_value());

    // An entry's flags byte has three bits; any other is refused.
    std::vector<std::uint8_t> bytes = encodeResponse(1, reply);
    const std::size_t firstFlags = 4 + HEADER_BYTES + 4 + 4 + 8;
    ASSERT_EQ(bytes.at(firstFlags), 3U);
    bytes.at(firstFlags) = 8;
    EXPECT_FALSE(decodeResponse(bytes).has_value());
    // Socket inodes without the sockets bit are refused: the first entry, given one.
    bytes = encodeResponse(1, reply);
    const std::size_t firstInodeCount = firstFlags + 1 + 4 + 8 + 8;
    ASSERT_EQ(bytes.at(firstInodeCount), 0U);
    bytes.at(firstInodeCount) = 1;
    EXPECT_FALSE(decodeResponse(bytes).has_value());
}

TEST(BrokerProtocolTest, AnEntryWhoseSocketsWouldPassTheCapGoesWithoutThem)
{
    ProcessCountersEntry many{.target = TARGET, .socketsRead = true};
    many.socketInodes.resize(MAX_COUNTER_SOCKET_INODES - 1, 5);
    ProcessCountersEntry past{.target = {.pid = 7, .startTimeTicks = 9}, .handleCountRead = true, .handleCount = 2, .socketsRead = true};
    past.socketInodes = {1, 2};
    const Response decoded = roundTrip(Response{ProcessCountersReply{.entries = {many, past}}});
    ASSERT_TRUE(std::holds_alternative<ProcessCountersReply>(decoded));
    const auto& entries = std::get<ProcessCountersReply>(decoded).entries;
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].socketInodes.size(), MAX_COUNTER_SOCKET_INODES - 1);
    EXPECT_FALSE(entries[1].socketsRead) << "two more would pass MAX_COUNTER_SOCKET_INODES";
    EXPECT_TRUE(entries[1].socketInodes.empty());
    EXPECT_EQ(entries[1].handleCount, 2) << "the rest of it still goes";
}

TEST(BrokerProtocolTest, AHellosSessionFlagIsOneByteOfZeroOrOne)
{
    std::vector<std::uint8_t> bytes = encodeRequest(1, Request{Hello{.forSession = true}});
    ASSERT_EQ(bytes.back(), 1U);
    bytes.back() = 2;
    EXPECT_FALSE(decodeRequest(bytes).has_value());
}

TEST(BrokerProtocolTest, ATransportWithoutASessionHelperRefusesToHoldOne)
{
    // Windows' Administrator restarts TaskSmack elevated (#2728); only the Linux helper is held.
    Mocks::FakeBroker broker;
    const auto held = broker.holdForSession(std::stop_token{});
    EXPECT_FALSE(held.has_value());
    EXPECT_FALSE(broker.heldForSession());
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

TEST(BrokerProtocolTest, MemoryRollupsRoundTrip)
{
    EXPECT_EQ(roundTrip(Request{MemoryRollupRequest{.target = TARGET}}), Request{MemoryRollupRequest{.target = TARGET}});
    const Response figures = MemoryRollupReply{.result = MemoryRollup{.pssBytes = 2560000, .ussBytes = 2048000, .swapPssBytes = 65536}};
    EXPECT_EQ(roundTrip(figures), figures);
    const Response refused = MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::PermissionDenied)};
    EXPECT_EQ(roundTrip(refused), refused);
    const Response absent = MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
    EXPECT_EQ(roundTrip(absent), absent);

    std::vector<std::uint8_t> bytes = encodeResponse(1, figures);
    bytes[4 + HEADER_BYTES] = 9; // no such failure
    EXPECT_FALSE(decodeResponse(bytes).has_value());
}

TEST(BrokerProtocolTest, FirmwareFilesRoundTrip)
{
    for (const FirmwareFile file : {FirmwareFile::SmbiosEntryPoint,
                                    FirmwareFile::SmbiosTable,
                                    FirmwareFile::ProductSerial,
                                    FirmwareFile::ProductUuid,
                                    FirmwareFile::BoardSerial})
    {
        EXPECT_EQ(roundTrip(Request{FirmwareFileRequest{.file = file}}), Request{FirmwareFileRequest{.file = file}});
    }
    const Response table = FirmwareFileReply{.status = FirmwareFileStatus::Ok, .bytes = {0x11, 0x15, 0x00, 0x00, 0x7F}};
    EXPECT_EQ(roundTrip(table), table);
    const Response missing = FirmwareFileReply{.status = FirmwareFileStatus::NotFound, .bytes = {}};
    EXPECT_EQ(roundTrip(missing), missing);

    std::vector<std::uint8_t> request = encodeRequest(1, FirmwareFileRequest{.file = FirmwareFile::BoardSerial});
    request[4 + HEADER_BYTES] = 5; // no such file: the helper never maps it to a path
    EXPECT_FALSE(decodeRequest(request).has_value());

    std::vector<std::uint8_t> reply = encodeResponse(1, table);
    reply[4 + HEADER_BYTES] = 3; // no such status
    EXPECT_FALSE(decodeResponse(reply).has_value());
}

TEST(BrokerProtocolTest, CrashesRoundTrip)
{
    EXPECT_EQ(roundTrip(Request{CrashesRequest{}}), Request{CrashesRequest{}});
    CrashesReply reply;
    reply.info.available = true;
    reply.info.family = OsFamily::Linux;
    reply.info.listed = true;
    reply.info.journalRead = true;
    reply.info.capped = true;
    CrashEvent own;
    own.unixSeconds = 1'760'000'000;
    own.application = "python3";
    own.pid = 4242;
    own.uid = 1000;
    own.coreBytes = 1 << 20;
    own.coreKept = true;
    own.signal = "SIGSEGV";
    own.executable = "/usr/bin/python3.12";
    CrashEvent others;
    others.unixSeconds = 1'759'000'000;
    others.application = "postgres";
    others.signal = "SIGABRT";
    reply.info.events = {own, others};
    EXPECT_EQ(roundTrip(Response{reply}), Response{reply});

    std::vector<std::uint8_t> bytes = encodeResponse(1, reply);
    bytes[4 + HEADER_BYTES + 1] = 9; // no such OS family
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
        MemoryRollupReply{},
        FirmwareFileReply{.status = FirmwareFileStatus::Ok, .bytes = {}},
        CrashesReply{},
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
        EnvironmentRequest{.target = TARGET},
        ModulesRequest{.target = TARGET},
        OpenFilesRequest{.target = TARGET},
        ConnectionsRequest{.target = TARGET},
        MemoryRollupRequest{.target = TARGET},
        FirmwareFileRequest{.file = FirmwareFile::ProductUuid},
        CrashesRequest{},
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

    // Crashes: available, family, listed, accessDenied, reason (4), capped, journalRead,
    // journal reason (4), otherUsersHidden, count (4).
    const std::vector<std::uint8_t> crashes = encodeResponse(1, CrashesReply{});
    ASSERT_TRUE(decodeResponse(crashes).has_value());
    EXPECT_TRUE(refused(crashes, PAYLOAD + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 4 + 1 + 3, 0x7F));
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

    const MemoryRollupReply figures{.result = MemoryRollup{.pssBytes = 1, .ussBytes = 2, .swapPssBytes = 3}};
    const MemoryRollupReply refusedRollup{.result = std::unexpected(MemoryRollupFailure::PermissionDenied)};
    const MemoryRollupReply absent{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
    EXPECT_FALSE(figures == refusedRollup);
    EXPECT_FALSE(refusedRollup == absent);
    EXPECT_FALSE(figures == (MemoryRollupReply{.result = MemoryRollup{.pssBytes = 1, .ussBytes = 2, .swapPssBytes = 4}}));
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

TEST(BrokerProtocolTest, AFirmwareFileIsCappedAtItsLimit)
{
    const std::vector<std::uint8_t> big(MAX_FIRMWARE_FILE_BYTES + 10, 0xAB);
    const auto decoded = decodeResponse(encodeResponse(1, FirmwareFileReply{.status = FirmwareFileStatus::Ok, .bytes = big}));
    ASSERT_TRUE(decoded.has_value());
    const auto* reply = std::get_if<FirmwareFileReply>(&decoded->message);
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->bytes.size(), MAX_FIRMWARE_FILE_BYTES);
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
                                   MessageKind::ReadEnvironment,
                                   MessageKind::ReadModules,
                                   MessageKind::ReadOpenFiles,
                                   MessageKind::ReadConnections,
                                   MessageKind::ReadMemoryRollup,
                                   MessageKind::ReadFirmwareFile,
                                   MessageKind::ReadCrashes,
                                   MessageKind::ReadProcessCounters,
                                   MessageKind::ActionResult,
                                   MessageKind::EnvironmentResult,
                                   MessageKind::ModulesResult,
                                   MessageKind::OpenFilesResult,
                                   MessageKind::ConnectionsResult,
                                   MessageKind::MemoryRollupResult,
                                   MessageKind::FirmwareFileResult,
                                   MessageKind::CrashesResult,
                                   MessageKind::ProcessCountersResult,
                                   MessageKind::Error})
    {
        EXPECT_TRUE(seeded.contains(kind)) << "no seed for kind " << static_cast<int>(kind);
    }
}

} // namespace
} // namespace Platform::Broker
