// Tests for the Windows transport of the privileged broker (#2311): the named pipe's security (its
// DACL, label, single instance and the PID checks each side makes), BrokerProtocol frames over it, the
// helper's serve loop, and PipeBroker, the client, against a helper served in this process over a real
// pipe with a fresh random name per test. Every wait is bounded.

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ThreadName.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aclapi.h>
#include <sddl.h>

namespace Platform::Windows
{
namespace
{

using namespace std::chrono_literals;

/// The bound on anything that should happen at once: a test that waits this long has failed.
constexpr std::chrono::milliseconds WAIT{5'000};
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 99};

[[nodiscard]] std::wstring freshName()
{
    auto name = BrokerPipe::makePipeName();
    EXPECT_TRUE(name.has_value());
    return name.value_or(L"");
}

[[nodiscard]] BrokerPipe::Sid mySid()
{
    UniqueHandle token;
    EXPECT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()), FALSE);
    auto sid = BrokerPipe::userSidOfToken(token.get());
    EXPECT_TRUE(sid.has_value());
    return sid.value_or(BrokerPipe::Sid{});
}

/// Why the broker won't serve this test's own user, or nothing when it will. It serves only user
/// accounts (BrokerPipe::checkUserSid). Windows Sandbox's only user, the built-in WDAGUtilityAccount
/// (RID 504), isn't one, and the sandbox's folder rights aren't a standard install's either, so the
/// tests that need a pipe for this user, or those rights, skip there instead of failing (#2618).
[[nodiscard]] std::optional<std::string> brokerRefusesThisUser()
{
    UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::nullopt;
    }
    const auto sid = BrokerPipe::userSidOfToken(token.get());
    if (!sid.has_value())
    {
        return std::nullopt;
    }
    const auto checked = BrokerPipe::checkUserSid(*sid);
    if (checked.has_value())
    {
        return std::nullopt;
    }
    return checked.error();
}

/// The SID @p text names, or nothing when it doesn't name one.
[[nodiscard]] BrokerPipe::Sid sidFromText(const std::wstring& text)
{
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(text.c_str(), &sid) == FALSE)
    {
        return {};
    }
    const auto* const first = static_cast<const std::byte*>(sid);
    BrokerPipe::Sid bytes(first,
                          first + GetLengthSid(sid)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the SID's own length
    LocalFree(sid);
    return bytes;
}

[[nodiscard]] UniqueHandle ownProcess()
{
    return UniqueHandle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()));
}

/// This process, as the helper holds TaskSmack's: a handle acceptClient() checks the client against
/// (#2517). Open for the whole run.
[[nodiscard]] HANDLE self()
{
    static const UniqueHandle process = ownProcess();
    return process.get();
}

/// A PID that isn't this process's.
[[nodiscard]] DWORD notMyPid()
{
    return GetCurrentProcessId() + 4;
}

[[nodiscard]] std::wstring sidText(PSID sid)
{
    if (sid == nullptr)
    {
        return {};
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid, &text) == FALSE)
    {
        return {};
    }
    std::wstring result(text);
    LocalFree(text);
    return result;
}

[[nodiscard]] std::wstring sidText(BrokerPipe::Sid sid)
{
    return sidText(sid.empty() ? nullptr : static_cast<PSID>(sid.data()));
}

[[nodiscard]] std::chrono::milliseconds since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

/// A server and a client end, connected and each checked against this process.
struct PipePair
{
    UniqueHandle server;
    UniqueHandle client;
};

[[nodiscard]] PipePair connectedPair()
{
    const std::wstring name = freshName();
    auto server = BrokerPipe::createServerPipe(name, mySid());
    if (!server.has_value())
    {
        ADD_FAILURE() << server.error();
        return {};
    }
    auto accepted = std::async(std::launch::async, [pipe = server->get()] { return BrokerPipe::acceptClient(pipe, self(), WAIT, {}); });
    const UniqueHandle process = ownProcess();
    auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), process.get(), WAIT, {});
    const auto acceptedClient = accepted.get();
    EXPECT_TRUE(acceptedClient.has_value()) << acceptedClient.error_or("");
    if (!client.has_value())
    {
        ADD_FAILURE() << client.error();
        return {};
    }
    return {.server = std::move(*server), .client = std::move(*client)};
}

[[nodiscard]] bool send(HANDLE pipe, std::span<const std::uint8_t> bytes)
{
    std::string error;
    const bool sent = BrokerPipe::sendFrame(pipe, bytes, WAIT, {}, error);
    EXPECT_TRUE(sent) << error;
    return sent;
}

/// Four bytes declaring a frame of @p length, little-endian.
[[nodiscard]] std::vector<std::uint8_t> lengthPrefix(std::uint32_t length)
{
    return {static_cast<std::uint8_t>(length),
            static_cast<std::uint8_t>(length >> 8U),
            static_cast<std::uint8_t>(length >> 16U),
            static_cast<std::uint8_t>(length >> 24U)};
}

[[nodiscard]] std::expected<Broker::Decoded<Broker::Response>, std::string> receiveResponse(HANDLE pipe)
{
    const auto frame = BrokerPipe::receiveFrame(pipe, WAIT, {});
    if (!frame.has_value())
    {
        return std::unexpected(std::string("no frame"));
    }
    return Broker::decodeResponse(*frame);
}

// --- The pipe's name and security ---------------------------------------------------------------------

TEST(BrokerPipeTest, PipeNamesAreRandomAndLocal)
{
    const std::wstring first = freshName();
    const std::wstring second = freshName();
    EXPECT_NE(first, second);
    EXPECT_TRUE(first.starts_with(L"\\\\.\\pipe\\TaskSmack.Broker."));
    EXPECT_EQ(first.size(), std::wstring_view(L"\\\\.\\pipe\\TaskSmack.Broker.").size() + 32);
}

TEST(BrokerPipeTest, TheSecurityDescriptorLetsInOnlyTheUserWithAMediumLabel)
{
    const auto sddl = BrokerPipe::pipeSecurityDescriptor(sidFromText(L"S-1-5-21-1-2-3-1001"));
    ASSERT_TRUE(sddl.has_value()) << sddl.error();
    EXPECT_EQ(*sddl, L"D:P(A;;0x00100083;;;S-1-5-21-1-2-3-1001)(A;;RC;;;OW)S:(ML;;NWNRNX;;;ME)");
    EXPECT_EQ(BrokerPipe::CLIENT_ACCESS & FILE_CREATE_PIPE_INSTANCE, 0U);
}

TEST(BrokerPipeTest, UserAccountSidsAreLetIn)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    for (const std::wstring text : {L"S-1-5-21-1-2-3-500", L"S-1-5-21-1-2-3-1001", L"S-1-12-1-1-2-3-4"})
    {
        EXPECT_TRUE(BrokerPipe::checkUserSid(sidFromText(text)).has_value()) << std::string(text.begin(), text.end());
    }
    EXPECT_TRUE(BrokerPipe::checkUserSid(mySid()).has_value()) << "this test's own user";
}

TEST(BrokerPipeTest, GroupsWellKnownSidsAndJunkAreNotLetIn)
{
    // Everyone, Authenticated Users, INTERACTIVE, Users, Administrators, LocalSystem, Domain Users, a
    // low integrity level, and SIDs cut short or malformed.
    for (const std::wstring text : {L"S-1-1-0",
                                    L"S-1-5-11",
                                    L"S-1-5-4",
                                    L"S-1-5-32-545",
                                    L"S-1-5-32-544",
                                    L"S-1-5-18",
                                    L"S-1-5-21-1-2-3-513",
                                    L"S-1-16-4096",
                                    L"S-1-5-21-1-2-1001",
                                    L"S-1-5-",
                                    L"S-1-",
                                    L"WD"})
    {
        const BrokerPipe::Sid sid = sidFromText(text);
        EXPECT_FALSE(BrokerPipe::checkUserSid(sid).has_value()) << std::string(text.begin(), text.end());
        EXPECT_FALSE(BrokerPipe::pipeSecurityDescriptor(sid).has_value()) << std::string(text.begin(), text.end());
    }
    BrokerPipe::Sid truncated = sidFromText(L"S-1-5-21-1-2-3-1001");
    truncated.pop_back();
    EXPECT_FALSE(BrokerPipe::checkUserSid(truncated).has_value());
    EXPECT_FALSE(BrokerPipe::checkUserSid({}).has_value());
}

TEST(BrokerPipeTest, ThePipeLetsInOnlyTheUserTakesTheOwnersRightsAndHasAMediumLabel)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const auto server = BrokerPipe::createServerPipe(freshName(), mySid());
    ASSERT_TRUE(server.has_value()) << server.error();

    PACL dacl = nullptr;
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_EQ(GetSecurityInfo(server->get(),
                              SE_KERNEL_OBJECT,
                              DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                              nullptr,
                              nullptr,
                              &dacl,
                              &sacl,
                              &descriptor),
              static_cast<DWORD>(ERROR_SUCCESS));
    const std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, &LocalFree);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ASSERT_NE(GetSecurityDescriptorControl(descriptor, &control, &revision), FALSE);
    EXPECT_NE(control & SE_DACL_PROTECTED, 0) << "nothing inherited into the DACL";

    // Two allow ACEs: the user's client rights, and READ_CONTROL alone for OWNER RIGHTS, which takes the
    // owner's implicit WRITE_DAC away.
    ASSERT_NE(dacl, nullptr);
    ASSERT_EQ(dacl->AceCount, 2);
    void* ace = nullptr;
    ASSERT_NE(GetAce(dacl, 0, &ace), FALSE);
    auto* const allow = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    EXPECT_EQ(allow->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_EQ(allow->Mask, BrokerPipe::CLIENT_ACCESS);
    EXPECT_EQ(sidText(&allow->SidStart), sidText(mySid()));
    void* ownerAce = nullptr;
    ASSERT_NE(GetAce(dacl, 1, &ownerAce), FALSE);
    auto* const owner = static_cast<ACCESS_ALLOWED_ACE*>(ownerAce);
    EXPECT_EQ(owner->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_EQ(owner->Mask, static_cast<ACCESS_MASK>(READ_CONTROL));
    EXPECT_EQ(sidText(&owner->SidStart), L"S-1-3-4"); // OWNER RIGHTS

    ASSERT_NE(sacl, nullptr);
    ASSERT_EQ(sacl->AceCount, 1);
    void* labelAce = nullptr;
    ASSERT_NE(GetAce(sacl, 0, &labelAce), FALSE);
    auto* const label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(labelAce);
    EXPECT_EQ(label->Header.AceType, SYSTEM_MANDATORY_LABEL_ACE_TYPE);
    EXPECT_EQ(label->Mask,
              static_cast<ACCESS_MASK>(SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP |
                                       SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP));
    EXPECT_EQ(sidText(&label->SidStart), L"S-1-16-8192"); // medium integrity
}

TEST(BrokerPipeTest, APipeForAnotherUserCantBeOpened)
{
    const std::wstring name = freshName();
    const auto server = BrokerPipe::createServerPipe(name, sidFromText(L"S-1-5-21-1-2-3-1001")); // nobody's account
    ASSERT_TRUE(server.has_value()) << server.error();
    const UniqueHandle process = ownProcess();
    const auto start = std::chrono::steady_clock::now();
    const auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), process.get(), WAIT, {});
    EXPECT_FALSE(client.has_value());
    EXPECT_LT(since(start), WAIT) << "refused at once, not waited out";
}

TEST(BrokerPipeTest, ALowIntegrityClientIsRefused)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const std::wstring name = freshName();
    const auto server = BrokerPipe::createServerPipe(name, mySid());
    ASSERT_TRUE(server.has_value()) << server.error();

    // This user, at low integrity: a copy of this process's token, lowered and impersonated on a thread of
    // its own. Only the label can refuse it, since the DACL lets this user in. The error is the reason to
    // skip; the value is what the connect said.
    auto outcome = std::async(
        std::launch::async,
        [&name]() -> std::expected<std::string, std::string>
        {
            UniqueHandle token;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, token.put()) == FALSE)
            {
                return std::unexpected(std::string("can't open this process's token"));
            }
            UniqueHandle low;
            if (DuplicateTokenEx(token.get(),
                                 TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT,
                                 nullptr,
                                 SecurityImpersonation,
                                 TokenImpersonation,
                                 low.put()) == FALSE)
            {
                return std::unexpected(std::string("can't copy this process's token"));
            }
            BrokerPipe::Sid lowSid = sidFromText(L"S-1-16-4096");
            TOKEN_MANDATORY_LABEL label{};
            label.Label.Attributes = SE_GROUP_INTEGRITY;
            label.Label.Sid = lowSid.data();
            if (lowSid.empty() ||
                SetTokenInformation(low.get(), TokenIntegrityLevel, &label, static_cast<DWORD>(sizeof(label) + lowSid.size())) == FALSE)
            {
                return std::unexpected(std::string("can't lower the token's integrity level"));
            }
            const UniqueHandle process = ownProcess();
            if (SetThreadToken(nullptr, low.get()) == FALSE)
            {
                return std::unexpected(std::string("can't impersonate the low-integrity token"));
            }
            const auto client = BrokerPipe::connectToServer(name, GetCurrentProcessId(), process.get(), WAIT, {});
            RevertToSelf();
            return client.has_value() ? std::string("opened") : client.error();
        });
    const auto connected = outcome.get();
    if (!connected.has_value())
    {
        GTEST_SKIP() << "no low-integrity token here: " << connected.error();
    }
    EXPECT_NE(*connected, "opened");
    EXPECT_TRUE(connected->contains(std::system_category().message(ERROR_ACCESS_DENIED))) << *connected;
}

TEST(BrokerPipeTest, TheServerCanIdentifyTheClientButNotActAsIt)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // A pipe's server may impersonate its client once it has read from it.
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(BrokerPipe::receiveFrame(pair.server.get(), WAIT, {}).has_value());

    // On a thread of its own, so a failure can't leave the test's thread impersonating.
    auto level =
        std::async(std::launch::async,
                   [pipe = pair.server.get()]() -> std::expected<SECURITY_IMPERSONATION_LEVEL, std::string>
                   {
                       if (ImpersonateNamedPipeClient(pipe) == FALSE)
                       {
                           return std::unexpected(std::format("ImpersonateNamedPipeClient failed: {}", GetLastError()));
                       }
                       UniqueHandle token;
                       SECURITY_IMPERSONATION_LEVEL impersonation = SecurityAnonymous;
                       DWORD size = 0;
                       const bool read =
                           OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, token.put()) != FALSE &&
                           GetTokenInformation(token.get(), TokenImpersonationLevel, &impersonation, sizeof(impersonation), &size) != FALSE;
                       const DWORD error = GetLastError();
                       RevertToSelf();
                       if (!read)
                       {
                           return std::unexpected(std::format("reading the client's token failed: {}", error));
                       }
                       return impersonation;
                   });
    const auto impersonation = level.get();
    ASSERT_TRUE(impersonation.has_value()) << impersonation.error();
    EXPECT_EQ(*impersonation, SecurityIdentification) << "the client opens the pipe at SECURITY_IDENTIFICATION";
}
TEST(BrokerPipeTest, ASecondPipeOfTheSameNameIsRefused)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const std::wstring name = freshName();
    const auto first = BrokerPipe::createServerPipe(name, mySid());
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_FALSE(BrokerPipe::createServerPipe(name, mySid()).has_value()) << "a squatter's pipe is never shared";
}

TEST(BrokerPipeTest, ASecondClientIsRefused)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const std::wstring name = freshName();
    const auto server = BrokerPipe::createServerPipe(name, mySid());
    ASSERT_TRUE(server.has_value()) << server.error();
    auto accepted = std::async(std::launch::async, [pipe = server->get()] { return BrokerPipe::acceptClient(pipe, self(), WAIT, {}); });
    const UniqueHandle process = ownProcess();
    const auto first = BrokerPipe::connectToServer(name, GetCurrentProcessId(), process.get(), WAIT, {});
    ASSERT_TRUE(first.has_value()) << first.error();
    ASSERT_TRUE(accepted.get().has_value());

    const auto second = BrokerPipe::connectToServer(name, GetCurrentProcessId(), process.get(), WAIT, {});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), "another process opened the helper's pipe first");
}

TEST(BrokerPipeTest, EachSideChecksTheOthersProcess)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    EXPECT_TRUE(BrokerPipe::checkServerProcess(pair.client.get(), GetCurrentProcessId()).has_value());
    EXPECT_TRUE(BrokerPipe::checkClientProcess(pair.server.get(), GetCurrentProcessId()).has_value());
    EXPECT_FALSE(BrokerPipe::checkServerProcess(pair.client.get(), notMyPid()).has_value());
    EXPECT_FALSE(BrokerPipe::checkClientProcess(pair.server.get(), notMyPid()).has_value());
}

TEST(BrokerPipeTest, APipeServedByAnotherProcessIsRefused)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const std::wstring name = freshName();
    const auto server = BrokerPipe::createServerPipe(name, mySid());
    ASSERT_TRUE(server.has_value()) << server.error();
    auto accepted = std::async(std::launch::async, [pipe = server->get()] { return BrokerPipe::acceptClient(pipe, self(), 1s, {}); });
    const UniqueHandle process = ownProcess();
    const auto client = BrokerPipe::connectToServer(name, notMyPid(), process.get(), WAIT, {});
    ASSERT_FALSE(client.has_value());
    EXPECT_TRUE(client.error().contains("not the helper")) << client.error();
    [[maybe_unused]] const auto served = accepted.get(); // whatever the server made of a client that gave up on it
}

// A client that isn't the expected process: test_BrokerHelper.cpp's, which has another process to expect.

TEST(BrokerPipeTest, NoClientWithinTheTimeoutIsGivenUpOn)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const auto server = BrokerPipe::createServerPipe(freshName(), mySid());
    ASSERT_TRUE(server.has_value()) << server.error();
    const auto start = std::chrono::steady_clock::now();
    const auto accepted = BrokerPipe::acceptClient(server->get(), self(), 100ms, {});
    ASSERT_FALSE(accepted.has_value());
    EXPECT_EQ(accepted.error(), "TaskSmack didn't open the pipe in time");
    EXPECT_LT(since(start), WAIT);
}

TEST(BrokerPipeTest, AHelperThatNeverMakesItsPipeIsGivenUpOn)
{
    const UniqueHandle process = ownProcess();
    const auto start = std::chrono::steady_clock::now();
    const auto client = BrokerPipe::connectToServer(freshName(), GetCurrentProcessId(), process.get(), 100ms, {});
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(client.error(), "the helper didn't open its pipe in time");
    EXPECT_LT(since(start), WAIT);
}

TEST(BrokerPipeTest, AHelperThatExitsBeforeMakingItsPipeIsNotWaitedFor)
{
    // A signalled event stands in for the helper's process handle once it has exited.
    const UniqueHandle exited(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    ASSERT_TRUE(exited);
    const auto start = std::chrono::steady_clock::now();
    const auto client = BrokerPipe::connectToServer(freshName(), GetCurrentProcessId(), exited.get(), WAIT, {});
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(client.error(), "the helper exited before it opened its pipe");
    EXPECT_LT(since(start), WAIT);
}

// --- Frames ------------------------------------------------------------------------------------------

TEST(BrokerPipeTest, FramesGoBothWays)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const std::vector<std::uint8_t> request = Broker::encodeRequest(7, Broker::Hello{});
    ASSERT_TRUE(send(pair.client.get(), request));
    const auto received = BrokerPipe::receiveFrame(pair.server.get(), WAIT, {});
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, request);

    const std::vector<std::uint8_t> response = Broker::encodeResponse(7, Broker::HelloReply{.accepted = true});
    ASSERT_TRUE(send(pair.server.get(), response));
    const auto answered = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    ASSERT_TRUE(answered.has_value());
    EXPECT_EQ(*answered, response);
}

TEST(BrokerPipeTest, AFrameLargerThanThePipesBufferArrivesWhole)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    constexpr std::size_t BODY = std::size_t{1} << 20U;
    std::vector<std::uint8_t> frame = lengthPrefix(static_cast<std::uint32_t>(BODY));
    for (std::size_t i = 0; i < BODY; ++i)
    {
        frame.push_back(static_cast<std::uint8_t>(i * 31U));
    }
    auto sent = std::async(std::launch::async,
                           [&]
                           {
                               std::string error;
                               return BrokerPipe::sendFrame(pair.server.get(), frame, WAIT, {}, error);
                           });
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    EXPECT_TRUE(sent.get());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, frame);
}

TEST(BrokerPipeTest, AFrameDeclaredTooLargeIsRefusedBeforeItIsRead)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    ASSERT_TRUE(send(pair.client.get(), lengthPrefix(static_cast<std::uint32_t>(Broker::MAX_MESSAGE_BYTES))));
    const auto start = std::chrono::steady_clock::now();
    const auto received = BrokerPipe::receiveFrame(pair.server.get(), WAIT, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed);
    EXPECT_LT(since(start), WAIT) << "refused on its length, not waited for";
}

TEST(BrokerPipeTest, AFrameDeclaredOneByteOverTheCapIsRefusedBeforeItIsRead)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // The cap is on the whole frame, prefix included, so the largest body is MAX_MESSAGE_BYTES - 4.
    ASSERT_TRUE(send(pair.client.get(), lengthPrefix(static_cast<std::uint32_t>(Broker::MAX_MESSAGE_BYTES - 3))));
    const auto start = std::chrono::steady_clock::now();
    const auto received = BrokerPipe::receiveFrame(pair.server.get(), WAIT, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed);
    EXPECT_LT(since(start), WAIT) << "refused on its length, not waited for";
}

TEST(BrokerPipeTest, AFrameDeclaredExactlyAtTheCapArrivesWhole)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    constexpr std::size_t BODY = Broker::MAX_MESSAGE_BYTES - 4;
    std::vector<std::uint8_t> frame = lengthPrefix(static_cast<std::uint32_t>(BODY));
    frame.reserve(Broker::MAX_MESSAGE_BYTES);
    for (std::size_t i = 0; i < BODY; ++i)
    {
        frame.push_back(static_cast<std::uint8_t>(i * 31U));
    }
    auto sent = std::async(std::launch::async,
                           [&]
                           {
                               std::string error;
                               return BrokerPipe::sendFrame(pair.server.get(), frame, WAIT, {}, error);
                           });
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    EXPECT_TRUE(sent.get());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->size(), Broker::MAX_MESSAGE_BYTES);
    EXPECT_EQ(*received, frame);
}

TEST(BrokerPipeTest, AFrameOfLengthZeroIsAFrameNotAClose)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    ASSERT_TRUE(send(pair.server.get(), lengthPrefix(0)));
    const std::vector<std::uint8_t> next = Broker::encodeResponse(3, Broker::HelloReply{.accepted = true});
    ASSERT_TRUE(send(pair.server.get(), next));

    const auto empty = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(*empty, lengthPrefix(0)) << "the prefix alone, and no body read for it";
    const auto after = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    ASSERT_TRUE(after.has_value()) << "the pipe is still in step";
    EXPECT_EQ(*after, next);
}

TEST(BrokerPipeTest, ALengthWrittenInTwoPartsArrivesWhole)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const std::vector<std::uint8_t> frame = Broker::encodeResponse(5, Broker::HelloReply{.accepted = true});
    ASSERT_GT(frame.size(), 4U);
    auto received = std::async(std::launch::async, [pipe = pair.client.get()] { return BrokerPipe::receiveFrame(pipe, WAIT, {}); });

    // One byte of the length, a pause the reader waits through, then the rest of the length and the body.
    const std::span<const std::uint8_t> whole(frame);
    ASSERT_TRUE(send(pair.server.get(), whole.first(1)));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(received.wait_for(0ms), std::future_status::timeout) << "a length cut short is waited for, not taken";
    ASSERT_TRUE(send(pair.server.get(), whole.subspan(1, 3)));
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(send(pair.server.get(), whole.subspan(4)));

    ASSERT_EQ(received.wait_for(WAIT), std::future_status::ready);
    const auto frameReceived = received.get();
    ASSERT_TRUE(frameReceived.has_value());
    EXPECT_EQ(*frameReceived, frame);
}

TEST(BrokerPipeTest, AFrameTooLargeOrForAGonePeerIsNotSent)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    std::string error;
    const std::vector<std::uint8_t> tooLarge(Broker::MAX_MESSAGE_BYTES + 1);
    EXPECT_FALSE(BrokerPipe::sendFrame(pair.client.get(), tooLarge, WAIT, {}, error));
    EXPECT_EQ(error, "a frame larger than the protocol allows");

    pair.server.reset();
    EXPECT_FALSE(BrokerPipe::sendFrame(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{}), WAIT, {}, error));
}

TEST(BrokerPipeTest, AFrameCutShortByAClosedPipeIsAFailure)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    std::vector<std::uint8_t> partial = lengthPrefix(100);
    partial.resize(partial.size() + 10);
    ASSERT_TRUE(send(pair.server.get(), partial));
    pair.server.reset();
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed) << "cut off partway, not a clean close";
}

TEST(BrokerPipeTest, AFrameOneByteShortWhenThePipeClosesIsAFailure)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    constexpr std::uint32_t BODY = 100;
    std::vector<std::uint8_t> partial = lengthPrefix(BODY);
    partial.resize(partial.size() + BODY - 1, 0x5A);
    ASSERT_TRUE(send(pair.server.get(), partial));
    pair.server.reset();
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), WAIT, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed) << "not a frame one byte short, and not a clean close";
}

TEST(BrokerPipeTest, AFrameCutShortByTheTimeoutIsAFailure)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    std::vector<std::uint8_t> partial = lengthPrefix(100);
    partial.resize(partial.size() + 10);
    ASSERT_TRUE(send(pair.server.get(), partial));
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), 200ms, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed) << "mid-frame, so not a timeout the session survives";
}

TEST(BrokerPipeTest, ALengthCutShortIsAFailure)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const std::vector<std::uint8_t> twoBytes{1, 0};
    ASSERT_TRUE(send(pair.server.get(), twoBytes));
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), 200ms, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Failed);
}

TEST(BrokerPipeTest, NothingWithinTheTimeoutIsATimeout)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const auto start = std::chrono::steady_clock::now();
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), 100ms, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::TimedOut);
    EXPECT_GE(since(start), 90ms);
    EXPECT_LT(since(start), WAIT);
    EXPECT_EQ(BrokerPipe::bytesWaiting(pair.client.get()), DWORD{0}) << "the pipe is still open and in step";
}

TEST(BrokerPipeTest, AStoppedReceiveSaysSo)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    std::stop_source stop;
    stop.request_stop();
    const auto received = BrokerPipe::receiveFrame(pair.client.get(), WAIT, stop.get_token());
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Stopped);
}

TEST(BrokerPipeTest, AClosedPipeIsTheEndOfTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    pair.client.reset();
    const auto received = BrokerPipe::receiveFrame(pair.server.get(), WAIT, {});
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), BrokerPipe::ReceiveEnd::Closed);
    EXPECT_FALSE(BrokerPipe::bytesWaiting(pair.server.get()).has_value());
}

// --- The helper's serve loop -------------------------------------------------------------------------

/// Answers every action with success, and counts what reached it.
struct CountingHandler
{
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);

    [[nodiscard]] BrokerPipeServer::RequestHandler handler() const
    {
        return [calls = calls](const Broker::Request& request) -> Broker::Response
        {
            ++*calls;
            if (std::holds_alternative<Broker::SignalRequest>(request))
            {
                return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
            }
            return Broker::ErrorReply{.message = "not in this test"};
        };
    }
};

/// serve() on pair.server, on its own thread.
[[nodiscard]] std::future<BrokerPipeServer::SessionEnd> serveAsync(HANDLE pipe,
                                                                   BrokerPipeServer::RequestHandler handler,
                                                                   std::chrono::milliseconds idleTimeout,
                                                                   std::stop_token stopToken = {},
                                                                   Broker::IdleClock clock = {})
{
    return std::async(std::launch::async,
                      [pipe, handler = std::move(handler), idleTimeout, stopToken = std::move(stopToken), clock = std::move(clock)]
                      { return BrokerPipeServer::serve(pipe, handler, idleTimeout, stopToken, clock); });
}

/// An idle clock that moves only when it is stepped, so a test says when the idle timeout runs out (#2546).
class SteppedClock
{
  public:
    [[nodiscard]] Broker::IdleClock clock() const
    {
        return [ticks = m_Ticks]
        {
            return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{ticks->load()}};
        };
    }

    void step(std::chrono::steady_clock::duration by) const
    {
        *m_Ticks += by.count();
    }

  private:
    std::shared_ptr<std::atomic<std::chrono::steady_clock::rep>> m_Ticks = std::make_shared<std::atomic<std::chrono::steady_clock::rep>>(0);
};

TEST(BrokerPipeServeTest, AnswersRequestsAfterTheHelloAndEndsWhenTheClientCloses)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const CountingHandler counting;
    auto served = serveAsync(pair.server.get(), counting.handler(), WAIT);

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    const auto hello = receiveResponse(pair.client.get());
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->requestId, 1U);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(hello->message));
    EXPECT_TRUE(std::get<Broker::HelloReply>(hello->message).accepted);

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill})));
    const auto action = receiveResponse(pair.client.get());
    ASSERT_TRUE(action.has_value());
    EXPECT_EQ(action->requestId, 2U);
    ASSERT_TRUE(std::holds_alternative<Broker::ActionResult>(action->message));
    EXPECT_TRUE(std::get<Broker::ActionResult>(action->message).success);
    EXPECT_EQ(counting.calls->load(), 1) << "the Hello is the transport's, not the handler's";

    pair.client.reset();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ClientClosed);
}

TEST(BrokerPipeServeTest, AnActionBeforeTheHelloIsRefusedAndEndsTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const CountingHandler counting;
    auto served = serveAsync(pair.server.get(), counting.handler(), WAIT);

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill})));
    const auto refused = receiveResponse(pair.client.get());
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(refused->message));
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::HandshakeFailed);
    EXPECT_EQ(counting.calls->load(), 0);
}

TEST(BrokerPipeServeTest, AHelloOfAnotherVersionEndsTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), WAIT);

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{.version = Broker::PROTOCOL_VERSION + 1})));
    const auto reply = receiveResponse(pair.client.get());
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(reply->message));
    EXPECT_FALSE(std::get<Broker::HelloReply>(reply->message).accepted);
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::HandshakeFailed);
}

TEST(BrokerPipeServeTest, AMalformedFrameAfterTheHelloGetsAnErrorAndTheSessionGoesOn)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), WAIT);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    std::vector<std::uint8_t> garbage = lengthPrefix(3);
    garbage.insert(garbage.end(), {0xFF, 0xFF, 0xFF});
    ASSERT_TRUE(send(pair.client.get(), garbage));
    const auto error = receiveResponse(pair.client.get());
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(error->message));

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill})));
    const auto action = receiveResponse(pair.client.get());
    ASSERT_TRUE(action.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(action->message));

    pair.client.reset();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ClientClosed);
}

TEST(BrokerPipeServeTest, AFrameOfLengthZeroAfterTheHelloGetsAnErrorAndTheSessionGoesOn)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), WAIT);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    ASSERT_TRUE(send(pair.client.get(), lengthPrefix(0)));
    const auto error = receiveResponse(pair.client.get());
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(error->message));

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill})));
    const auto action = receiveResponse(pair.client.get());
    ASSERT_TRUE(action.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(action->message));

    pair.client.reset();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ClientClosed);
}

TEST(BrokerPipeServeTest, AFrameDeclaredTooLargeEndsTheSessionAsBroken)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), WAIT);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    ASSERT_TRUE(send(pair.client.get(), lengthPrefix(0xFFFF'FFF0U)));
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Broken);
}

TEST(BrokerPipeServeTest, AHandlerThatThrowsGetsAnErrorReplyAndTheSessionGoesOn)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), [](const Broker::Request&) -> Broker::Response { throw std::runtime_error("boom"); }, WAIT);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill})));
    const auto reply = receiveResponse(pair.client.get());
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(std::holds_alternative<Broker::ErrorReply>(reply->message));
    EXPECT_EQ(std::get<Broker::ErrorReply>(reply->message).message, "boom");

    pair.client.reset();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ClientClosed);
}

TEST(BrokerPipeServeTest, ReadsDontPutOffTheIdleExit)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // The idle timeout runs on a clock each read steps by a sixth of it, on the serve thread before the
    // reply goes out, so how the session ends hangs on the order of the requests, not on the machine's
    // speed (#2546). The clock's hour is far longer than the test runs, so nothing times out for real.
    constexpr std::chrono::milliseconds IDLE = 1h;
    constexpr int READS = 6;
    const SteppedClock clock;
    auto served = serveAsync(
        pair.server.get(),
        [clock, step = IDLE / READS](const Broker::Request&) -> Broker::Response
        {
            clock.step(step);
            return Broker::ErrorReply{.message = "not in this test"};
        },
        IDLE,
        {},
        clock.clock());
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    // PSS/USS reads, as the sampler's come: every one is answered, and the last takes the clock to the idle
    // timeout after the Hello, which ends the session.
    for (std::uint32_t id = 2; id < 2 + READS; ++id)
    {
        ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(id, Broker::MemoryRollupRequest{.target = TARGET})));
        ASSERT_TRUE(receiveResponse(pair.client.get()).has_value()) << "read " << id << " ended the session early";
    }
    const auto status = served.wait_for(WAIT);
    pair.client.reset(); // a session the reads put off would wait out IDLE for real; this ends it now
    ASSERT_EQ(status, std::future_status::ready) << "the reads put off the idle exit";
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Idle);
}

TEST(BrokerPipeServeTest, AnIdleSessionEnds)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), 100ms);
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Idle);
}

TEST(BrokerPipeServeTest, AStoppedSessionEnds)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    std::stop_source stop;
    auto served = serveAsync(pair.server.get(), CountingHandler{}.handler(), WAIT, stop.get_token());
    stop.request_stop();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Stopped);
}

// --- What puts off the Windows helper's idle exit, and a session for one action (#2550) ---------------

/// serve() under @p grant on its own thread, with an idle clock that @p handler steps.
[[nodiscard]] std::future<BrokerPipeServer::SessionEnd> serveGrantAsync(HANDLE pipe,
                                                                        BrokerPipeServer::RequestHandler handler,
                                                                        std::chrono::milliseconds idleTimeout,
                                                                        Broker::IdleClock clock,
                                                                        BrokerPipeServer::Grant grant)
{
    return std::async(std::launch::async,
                      [pipe, handler = std::move(handler), idleTimeout, clock = std::move(clock), grant]
                      { return BrokerPipeServer::serve(pipe, handler, idleTimeout, std::stop_token{}, clock, grant); });
}

/// A handler that steps @p clock by @p step on each request, before its reply goes out, and answers
/// an action with success.
[[nodiscard]] BrokerPipeServer::RequestHandler stepping(const SteppedClock& clock, std::chrono::steady_clock::duration step)
{
    return [clock, step](const Broker::Request& request) -> Broker::Response
    {
        clock.step(step);
        if (std::holds_alternative<Broker::SignalRequest>(request) || std::holds_alternative<Broker::PriorityRequest>(request) ||
            std::holds_alternative<Broker::IoPriorityRequest>(request) || std::holds_alternative<Broker::AffinityRequest>(request))
        {
            return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
        }
        return Broker::ErrorReply{.message = "not in this test"};
    };
}

TEST(BrokerPipeServeTest, OnlyActionsWindowsDoesAndTheHelloPutOffTheIdleExit)
{
    using Broker::Signal;
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::Hello{}));
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::SignalRequest{.target = TARGET, .signal = Signal::Terminate}));
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::SignalRequest{.target = TARGET, .signal = Signal::Kill}));
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::PriorityRequest{.target = TARGET, .nice = 0}));
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::AffinityRequest{.target = TARGET, .cpus = {}}));
    // Suspend and Resume are real actions on Windows since #2582 (NtSuspendProcess, NtResumeProcess).
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::SignalRequest{.target = TARGET, .signal = Signal::Suspend}));
    EXPECT_TRUE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::SignalRequest{.target = TARGET, .signal = Signal::Resume}));
    // What Windows always refuses keeps nothing alive.
    EXPECT_FALSE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::IoPriorityRequest{}));
    // Nor does a read.
    EXPECT_FALSE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::ModulesRequest{.target = TARGET}));
    EXPECT_FALSE(BrokerPipeServer::putsOffTheWindowsIdleExit(Broker::CrashesRequest{}));

    EXPECT_TRUE(BrokerPipeServer::isActionWindowsDoes(Broker::SignalRequest{.target = TARGET, .signal = Signal::Kill}));
    EXPECT_TRUE(BrokerPipeServer::isActionWindowsDoes(Broker::SignalRequest{.target = TARGET, .signal = Signal::Suspend}));
    EXPECT_TRUE(BrokerPipeServer::isActionWindowsDoes(Broker::SignalRequest{.target = TARGET, .signal = Signal::Resume}));
    EXPECT_FALSE(BrokerPipeServer::isActionWindowsDoes(Broker::IoPriorityRequest{}));
    EXPECT_FALSE(BrokerPipeServer::isActionWindowsDoes(Broker::Hello{}));
}

TEST(BrokerPipeServeTest, RequestsWindowsAlwaysRefusesDontPutOffTheIdleExit)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // Each request steps the idle clock by a quarter of the timeout; the fourth takes it to the timeout
    // after the Hello, unless one of them put the exit off.
    constexpr std::chrono::milliseconds IDLE = 1h;
    const SteppedClock clock;
    auto served = serveGrantAsync(pair.server.get(), stepping(clock, IDLE / 4), IDLE, clock.clock(), BrokerPipeServer::Grant::Session);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    // An I/O priority is the one action Windows always refuses (Suspend and Resume are real since #2582).
    const std::array<Broker::Request, 4> refused{
        Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 0},
        Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::BestEffort, .level = 4},
        Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 0},
        Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::BestEffort, .level = 7},
    };
    std::uint32_t id = 2;
    for (const Broker::Request& request : refused)
    {
        ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(id, request)));
        ASSERT_TRUE(receiveResponse(pair.client.get()).has_value()) << "request " << id << " ended the session early";
        ++id;
    }
    const auto status = served.wait_for(WAIT);
    pair.client.reset(); // a session they put off would wait out IDLE for real; this ends it now
    ASSERT_EQ(status, std::future_status::ready) << "a request Windows refuses put off the idle exit";
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Idle);
}

TEST(BrokerPipeServeTest, ActionsWindowsDoesPutOffTheIdleExit)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // Each action steps the clock by half the timeout after it has put the exit off: three of them
    // would reach it from the Hello, but each starts the wait again.
    constexpr std::chrono::milliseconds IDLE = 1h;
    const SteppedClock clock;
    auto served = serveGrantAsync(pair.server.get(), stepping(clock, IDLE / 2), IDLE, clock.clock(), BrokerPipeServer::Grant::Session);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    const std::array<Broker::Request, 5> actions{
        Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate},
        Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend}, // #2582
        Broker::PriorityRequest{.target = TARGET, .nice = 0},
        Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}, // #2582
        Broker::AffinityRequest{.target = TARGET, .cpus = {}},
    };
    std::uint32_t id = 2;
    for (const Broker::Request& request : actions)
    {
        ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(id, request)));
        const auto reply = receiveResponse(pair.client.get());
        ASSERT_TRUE(reply.has_value()) << "action " << id << " found the session ended";
        EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(reply->message));
        ++id;
    }
    EXPECT_NE(served.wait_for(0ms), std::future_status::ready) << "a same-user session goes on after an action";
    pair.client.reset();
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ClientClosed);
}

TEST(BrokerPipeServeTest, AOneActionSessionEndsOnceItsActionIsAnswered)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    const PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    const SteppedClock clock;
    auto served = serveGrantAsync(pair.server.get(), stepping(clock, {}), WAIT, clock.clock(), BrokerPipeServer::Grant::OneAction);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    // Reads aren't the action.
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::ModulesRequest{.target = TARGET})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(3, Broker::CrashesRequest{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    EXPECT_NE(served.wait_for(0ms), std::future_status::ready);

    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(4, Broker::PriorityRequest{.target = TARGET, .nice = 10})));
    const auto reply = receiveResponse(pair.client.get());
    ASSERT_TRUE(reply.has_value()) << "the action is answered before the session ends";
    EXPECT_EQ(reply->requestId, 4U);
    EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(reply->message));
    ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ActionDone);
}

// The rule fails closed (#2577 review, finding 2): any request that isn't the Hello or a read is the
// one action, even one Windows refuses today, so an action kind Windows does later is limited too.
// Suspend and Resume, real actions since #2582, are the one action like Kill.
TEST(BrokerPipeServeTest, AOneActionSessionEndsAfterAnActionWindowsRefusesToo)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    for (const Broker::Request& action : {Broker::Request{Broker::IoPriorityRequest{}},
                                          Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend}},
                                          Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}},
                                          Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}}})
    {
        const PipePair pair = connectedPair();
        ASSERT_TRUE(pair.server && pair.client);
        const SteppedClock clock;
        auto served = serveGrantAsync(pair.server.get(), stepping(clock, {}), WAIT, clock.clock(), BrokerPipeServer::Grant::OneAction);
        ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
        ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
        ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, action)));
        ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
        ASSERT_EQ(served.wait_for(WAIT), std::future_status::ready);
        EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::ActionDone);
    }
}

TEST(BrokerPipeServeTest, EveryRequestKindIsTheHelloAnActionOrARead)
{
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::Hello{}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::PriorityRequest{.target = TARGET, .nice = 0}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::IoPriorityRequest{}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::AffinityRequest{.target = TARGET, .cpus = {}}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::EnvironmentRequest{.target = TARGET}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::ModulesRequest{.target = TARGET}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::OpenFilesRequest{.target = TARGET}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::ConnectionsRequest{.target = TARGET}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::MemoryRollupRequest{.target = TARGET}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::FirmwareFileRequest{}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::CrashesRequest{}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::ProcessCountersRequest{.targets = {TARGET}}));
    EXPECT_TRUE(BrokerPipeServer::isRead(Broker::HiddenProcessesRequest{.visiblePids = {TARGET.pid}}));
    EXPECT_FALSE(BrokerPipeServer::isRead(Broker::ServiceActionRequest{}));
    EXPECT_FALSE(BrokerPipeServer::isActionWindowsDoes(Broker::ServiceActionRequest{})) << "an ErrorReply";
    static_assert(std::variant_size_v<Broker::Request> == 15, "a new request kind: add it above");
}

TEST(BrokerPipeServeTest, AOneActionSessionIsntKeptOpenByGreetingAgain)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    PipePair pair = connectedPair();
    ASSERT_TRUE(pair.server && pair.client);
    // Each read steps the clock by half the timeout. A second Hello between them would put the exit
    // off in a session; in a session for one action, only the first one counts.
    constexpr std::chrono::milliseconds IDLE = 1h;
    const SteppedClock clock;
    auto served = serveGrantAsync(pair.server.get(), stepping(clock, IDLE / 2), IDLE, clock.clock(), BrokerPipeServer::Grant::OneAction);
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(1, Broker::Hello{})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(2, Broker::ModulesRequest{.target = TARGET})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(3, Broker::Hello{})));
    const auto again = receiveResponse(pair.client.get());
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(std::holds_alternative<Broker::HelloReply>(again->message));
    ASSERT_TRUE(send(pair.client.get(), Broker::encodeRequest(4, Broker::ModulesRequest{.target = TARGET})));
    ASSERT_TRUE(receiveResponse(pair.client.get()).has_value());

    const auto status = served.wait_for(WAIT);
    pair.client.reset();
    ASSERT_EQ(status, std::future_status::ready) << "the second Hello put off the idle exit";
    EXPECT_EQ(served.get(), BrokerPipeServer::SessionEnd::Idle);
}
// --- PipeBroker, the client ----------------------------------------------------------------------------

/// A helper served on a thread of this process: the launcher makes the pipe it is given, as
/// TaskSmackHelper.exe will, accepts this process as its client, and runs @p body on the pipe.
class InProcessHelper
{
  public:
    using Body = std::function<void(HANDLE pipe, const std::stop_token& stopToken)>;

    explicit InProcessHelper(Body body) : m_Body(std::move(body))
    {}

    [[nodiscard]] PipeBroker::Launcher launcher()
    {
        return [this](const std::wstring& pipeName, const std::stop_token& /*stopToken*/) -> std::expected<UniqueHandle, std::string>
        {
            // NOLINTNEXTLINE(bugprone-exception-escape) - a test's helper thread: what escapes it ends the run, as a failed test should
            m_Thread = std::jthread([this, pipeName](const std::stop_token& stopToken) { run(pipeName, stopToken); });
            return ownProcess();
        };
    }

    /// Whether the helper's session ended (its pipe closed) within WAIT.
    [[nodiscard]] bool ended()
    {
        return m_EndedFuture.wait_for(WAIT) == std::future_status::ready;
    }

    /// Whether the helper's session has ended by now, without waiting for it to.
    [[nodiscard]] bool endedAlready()
    {
        return m_EndedFuture.wait_for(0ms) == std::future_status::ready;
    }

  private:
    void run(const std::wstring& pipeName, const std::stop_token& stopToken)
    {
        try
        {
            const auto pipe = BrokerPipe::createServerPipe(pipeName, mySid());
            if (pipe.has_value() && BrokerPipe::acceptClient(pipe->get(), self(), WAIT, stopToken).has_value())
            {
                m_Body(pipe->get(), stopToken);
            }
        } // the pipe closes here, as the helper's does when it exits
        catch (...)
        {
            ADD_FAILURE() << "the in-process helper threw";
        }
        m_Ended.set_value();
    }

    Body m_Body;
    std::promise<void> m_Ended;
    std::future<void> m_EndedFuture = m_Ended.get_future();
    std::jthread m_Thread; // last: joined first, while the rest is alive
};

/// A helper that serves with @p handler and @p idleTimeout, and records how the session ended.
struct Serving
{
    BrokerPipeServer::RequestHandler handler;
    std::chrono::milliseconds idleTimeout{};
    std::shared_ptr<std::atomic<int>> end;

    void operator()(HANDLE pipe, const std::stop_token& stopToken) const
    {
        *end = static_cast<int>(BrokerPipeServer::serve(pipe, handler, idleTimeout, stopToken));
    }
};

[[nodiscard]] InProcessHelper::Body serving(BrokerPipeServer::RequestHandler handler,
                                            std::chrono::milliseconds idleTimeout,
                                            std::shared_ptr<std::atomic<int>> end = std::make_shared<std::atomic<int>>(-1))
{
    return Serving{.handler = std::move(handler), .idleTimeout = idleTimeout, .end = std::move(end)};
}

[[nodiscard]] Broker::Request killRequest()
{
    return Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill};
}

TEST(PipeBrokerTest, ConnectsToTheHelperAndCallsThroughIt)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessHelper helper(serving(CountingHandler{}.handler(), WAIT));
    PipeBroker broker(helper.launcher());
    const auto connected = broker.connectWithin({}, WAIT);
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_TRUE(broker.connected());
    EXPECT_TRUE(broker.connectWithin({}, WAIT).has_value()) << "connected already: no second helper";

    const ProcessActionResult result = actionResultOf(broker.call(killRequest(), {}));
    EXPECT_TRUE(result.success) << result.errorMessage;
    const auto idle = broker.callIfIdle(killRequest(), WAIT);
    ASSERT_TRUE(idle.has_value()) << idle.error();
    EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(*idle));
}

TEST(PipeBrokerTest, ACallWithoutAHelperSaysSo)
{
    PipeBroker broker([](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string>
                      { return std::unexpected(std::string("unused")); });
    EXPECT_FALSE(broker.connected());
    EXPECT_EQ(broker.call(killRequest(), {}).error_or(""), "not connected");
    EXPECT_EQ(broker.callIfIdle(killRequest(), 100ms).error_or(""), "not connected");
}

TEST(PipeBrokerTest, ALauncherThatFailsSaysWhy)
{
    PipeBroker broker([](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string>
                      { return std::unexpected(std::string("the prompt was dismissed")); });
    const auto connected = broker.connectWithin({}, WAIT);
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), "the prompt was dismissed");
    EXPECT_FALSE(broker.connected());
}

TEST(PipeBrokerTest, AHelperThatNeverMakesItsPipeIsGivenUpOn)
{
    PipeBroker broker([](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string> { return ownProcess(); });
    const auto start = std::chrono::steady_clock::now();
    const auto connected = broker.connectWithin({}, 200ms);
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), "the helper didn't open its pipe in time");
    EXPECT_LT(since(start), WAIT);
    EXPECT_FALSE(broker.connected());
}

TEST(PipeBrokerTest, AHelperOfAnotherVersionIsRefused)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessHelper helper(
        [](HANDLE pipe, const std::stop_token&)
        {
            const auto hello = BrokerPipe::receiveFrame(pipe, WAIT, {});
            const auto decoded = hello.has_value() ? Broker::decodeRequest(*hello) : std::unexpected(std::string("none"));
            if (decoded.has_value())
            {
                static_cast<void>(
                    send(pipe,
                         Broker::encodeResponse(decoded->requestId,
                                                Broker::HelloReply{.version = Broker::PROTOCOL_VERSION + 1, .accepted = false})));
            }
        });
    PipeBroker broker(helper.launcher());
    const auto connected = broker.connectWithin({}, WAIT);
    ASSERT_FALSE(connected.has_value());
    EXPECT_TRUE(connected.error().contains("protocol version")) << connected.error();
    EXPECT_FALSE(broker.connected());
}

TEST(PipeBrokerTest, DisconnectEndsTheHelpersSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto end = std::make_shared<std::atomic<int>>(-1);
    InProcessHelper helper(serving(CountingHandler{}.handler(), WAIT, end));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());
    broker.disconnect();
    EXPECT_FALSE(broker.connected());
    ASSERT_TRUE(helper.ended());
    EXPECT_EQ(end->load(), static_cast<int>(BrokerPipeServer::SessionEnd::ClientClosed));
}

TEST(PipeBrokerTest, AHelperThatExitsIsNoLongerConnected)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto end = std::make_shared<std::atomic<int>>(-1);
    InProcessHelper helper(serving(CountingHandler{}.handler(), 200ms, end));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());
    ASSERT_TRUE(helper.ended());
    EXPECT_EQ(end->load(), static_cast<int>(BrokerPipeServer::SessionEnd::Idle));
    EXPECT_FALSE(broker.connected());
    EXPECT_EQ(broker.call(killRequest(), {}).error_or(""), "not connected");
}

/// A handler whose first PSS/USS read waits until the test sets @p release (or WAIT passes), and whose
/// other requests are answered at once: a read that is slow for as long as the test needs, not for a
/// guessed time.
[[nodiscard]] BrokerPipeServer::RequestHandler firstReadWaitsFor(const std::shared_ptr<std::promise<void>>& release)
{
    auto reads = std::make_shared<std::atomic<int>>(0);
    return [reads, released = release->get_future().share()](const Broker::Request& request) -> Broker::Response
    {
        if (std::holds_alternative<Broker::MemoryRollupRequest>(request))
        {
            if (++*reads == 1)
            {
                static_cast<void>(released.wait_for(WAIT));
            }
            return Broker::MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
        }
        return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
    };
}

TEST(PipeBrokerTest, ACallIfIdleThatTimesOutKeepsTheSessionAndItsLateReplyIsDropped)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto release = std::make_shared<std::promise<void>>();
    InProcessHelper helper(serving(firstReadWaitsFor(release), WAIT));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());

    const auto slow = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, 100ms);
    ASSERT_FALSE(slow.has_value());
    EXPECT_EQ(slow.error(), "the helper didn't answer in time");
    EXPECT_TRUE(broker.connected()) << "a slow read is skipped, not the session (#2406)";

    // The read finishes only now; the next call waits past its late reply, drops it, and gets its own.
    release->set_value();
    const auto next = broker.call(killRequest(), {});
    ASSERT_TRUE(next.has_value()) << next.error();
    EXPECT_TRUE(std::holds_alternative<Broker::ActionResult>(*next));
    EXPECT_TRUE(broker.connected());
}

TEST(PipeBrokerTest, ALateReplyIsTakenBeforeTheNextCallIfIdleAsks)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto release = std::make_shared<std::promise<void>>();
    InProcessHelper helper(serving(firstReadWaitsFor(release), WAIT));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());
    ASSERT_FALSE(broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, 50ms).has_value());
    EXPECT_EQ(broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, 50ms).error_or(""), "busy")
        << "the late reply isn't in yet, so this read is skipped";

    // Busy until the late reply is in; then that is taken and this read asked.
    release->set_value();
    const auto start = std::chrono::steady_clock::now();
    std::expected<Broker::Response, std::string> read = std::unexpected(std::string("busy"));
    while (!read.has_value() && read.error() == "busy" && since(start) < WAIT)
    {
        std::this_thread::sleep_for(10ms);
        read = broker.callIfIdle(Broker::MemoryRollupRequest{.target = TARGET}, WAIT);
    }
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(*read));
    EXPECT_TRUE(broker.connected());
}
TEST(PipeBrokerTest, ACallIfIdleWhileACallIsWaitingIsBusy)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto entered = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    InProcessHelper helper(serving(
        [entered, release, releaseFuture = release->get_future().share()](const Broker::Request&) -> Broker::Response
        {
            entered->set_value();
            static_cast<void>(releaseFuture.wait_for(WAIT));
            return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
        },
        WAIT));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());

    auto waiting = std::async(std::launch::async, [&broker] { return broker.call(killRequest(), {}); });
    ASSERT_EQ(entered->get_future().wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(broker.callIfIdle(killRequest(), 100ms).error_or(""), "busy");
    release->set_value();
    EXPECT_TRUE(waiting.get().has_value());
}

TEST(PipeBrokerTest, AStoppedCallIsCancelledAndEndsTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto release = std::make_shared<std::promise<void>>();
    InProcessHelper helper(serving(
        [releaseFuture = release->get_future().share()](const Broker::Request&) -> Broker::Response
        {
            static_cast<void>(releaseFuture.wait_for(WAIT));
            return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}};
        },
        WAIT));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());

    std::stop_source stop;
    auto stopper = std::async(std::launch::async,
                              [&stop]
                              {
                                  std::this_thread::sleep_for(100ms);
                                  stop.request_stop();
                              });
    const auto start = std::chrono::steady_clock::now();
    const auto response = broker.call(killRequest(), stop.get_token());
    stopper.get();
    release->set_value();
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "cancelled");
    EXPECT_LT(since(start), WAIT);
    EXPECT_FALSE(broker.connected());
}

/// A helper that answers the Hello, then sends half of the reply to the next request and @p thenClose
/// or keeps the pipe open, silent, until stopped.
[[nodiscard]] InProcessHelper::Body cutsOffItsReply(bool thenClose)
{
    return [thenClose](HANDLE pipe, const std::stop_token& stopToken)
    {
        for (int frame = 0; frame < 2; ++frame)
        {
            const auto received = BrokerPipe::receiveFrame(pipe, WAIT, stopToken);
            const auto decoded = received.has_value() ? Broker::decodeRequest(*received) : std::unexpected(std::string("none"));
            if (!decoded.has_value())
            {
                return;
            }
            if (frame == 0)
            {
                static_cast<void>(send(pipe, Broker::encodeResponse(decoded->requestId, Broker::HelloReply{.accepted = true})));
                continue;
            }
            const std::vector<std::uint8_t> reply =
                Broker::encodeResponse(decoded->requestId, Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}});
            static_cast<void>(send(pipe, std::span(reply).first(reply.size() / 2)));
        }
        if (!thenClose)
        {
            // Silent, mid-frame, until the client gives up (its pipe closes) or the test ends.
            while (!stopToken.stop_requested() && BrokerPipe::bytesWaiting(pipe).has_value())
            {
                std::this_thread::sleep_for(20ms);
            }
        }
    };
}

TEST(PipeBrokerTest, ACallIfIdleWhoseReplyIsCutOffByTheTimeoutEndsTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessHelper helper(cutsOffItsReply(false));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());
    // Long enough for the half reply to come on any runner: the timeout then ends the read mid-frame.
    const auto response = broker.callIfIdle(killRequest(), 2s);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "the helper's pipe failed") << "mid-frame is a failure, not a skipped read (#2451)";
    EXPECT_FALSE(broker.connected());
    EXPECT_TRUE(helper.ended());
}

TEST(PipeBrokerTest, ACallWhoseReplyIsCutOffByTheHelperExitingEndsTheSession)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessHelper helper(cutsOffItsReply(true));
    PipeBroker broker(helper.launcher());
    ASSERT_TRUE(broker.connectWithin({}, WAIT).has_value());
    const auto response = broker.call(killRequest(), {});
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "the helper's pipe failed");
    EXPECT_FALSE(broker.connected());
}

// --- A UAC prompt left up (#2549; finding 3 of #2520's review) -------------------------------------------

/// A launcher whose first run blocks, as ShellExecuteExW("runas") does while its UAC prompt is up, until
/// the test answers it (or WAIT passes), and whose later runs say "asked again" at once. Its state is
/// shared, since the launch thread may outlive the test's broker; answered on scope exit at the latest.
class PromptUp
{
  public:
    PromptUp() = default;
    /// A prompt that, once answered, is approved: the launcher then runs @p approved.
    explicit PromptUp(PipeBroker::Launcher approved)
    {
        m_State->approved = std::move(approved);
    }
    ~PromptUp()
    {
        try
        {
            answer();
        }
        catch (...) // NOLINT(bugprone-empty-catch) - answered already: the launcher has returned
        {}
    }
    PromptUp(const PromptUp&) = delete;
    PromptUp& operator=(const PromptUp&) = delete;
    PromptUp(PromptUp&&) = delete;
    PromptUp& operator=(PromptUp&&) = delete;

    [[nodiscard]] PipeBroker::Launcher launcher() const
    {
        return [state = m_State](const std::wstring& pipeName, const std::stop_token& stopToken) -> std::expected<UniqueHandle, std::string>
        {
            if (state->runs.fetch_add(1) > 0)
            {
                return std::unexpected(std::string("asked again"));
            }
            state->shown.set_value();
            static_cast<void>(state->answered.wait_for(WAIT));
            auto result = state->approved
                            ? state->approved(pipeName, stopToken)
                            : std::expected<UniqueHandle, std::string>(std::unexpected(std::string("the UAC prompt was dismissed")));
            state->returned.set_value();
            return result;
        };
    }

    /// How many times the launcher was run: how many prompts were raised.
    [[nodiscard]] int runs() const
    {
        return m_State->runs.load();
    }

    /// Whether the prompt is up, within WAIT.
    [[nodiscard]] bool shown() const
    {
        return m_State->shownFuture.wait_for(WAIT) == std::future_status::ready;
    }

    /// Answers the prompt; the launcher returns.
    void answer()
    {
        if (!m_Answered)
        {
            m_Answered = true;
            m_State->answer.set_value();
        }
    }

    /// Whether the launcher has returned, within WAIT.
    [[nodiscard]] bool returned() const
    {
        return m_State->returnedFuture.wait_for(WAIT) == std::future_status::ready;
    }

  private:
    struct State
    {
        std::atomic<int> runs{0};
        std::promise<void> shown;
        std::future<void> shownFuture = shown.get_future();
        std::promise<void> answer;
        std::shared_future<void> answered = answer.get_future().share();
        std::promise<void> returned;
        std::future<void> returnedFuture = returned.get_future();
        PipeBroker::Launcher approved; // what an approved prompt starts; none: it was dismissed
    };
    std::shared_ptr<State> m_State = std::make_shared<State>();
    bool m_Answered = false;
};

/// How long a call made while a prompt is up may take: it doesn't wait for the prompt at all.
constexpr std::chrono::milliseconds AT_ONCE{2'000};

TEST(PipeBrokerTest, CallsDontWaitForAPromptThatIsUp)
{
    PromptUp prompt;
    PipeBroker broker(prompt.launcher());
    auto connecting = std::async(std::launch::async, [&broker] { return broker.connectWithin({}, 60s); });
    ASSERT_TRUE(prompt.shown());

    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(broker.call(killRequest(), {}).error_or(""), "not connected");
    EXPECT_LT(since(start), AT_ONCE);
    start = std::chrono::steady_clock::now();
    EXPECT_EQ(broker.callIfIdle(killRequest(), WAIT).error_or(""), "busy") << "as PkexecBroker's is while it connects";
    EXPECT_LT(since(start), AT_ONCE);
    start = std::chrono::steady_clock::now();
    EXPECT_EQ(broker.connectWithin({}, WAIT).error_or(""), std::string(PipeBroker::ALREADY_ASKING)) << "never a second prompt";
    EXPECT_FALSE(broker.connected());
    EXPECT_LT(since(start), AT_ONCE);

    prompt.answer();
    ASSERT_EQ(connecting.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(connecting.get().error_or(""), "the UAC prompt was dismissed") << "a refusal, which the UI offers to retry";
    EXPECT_EQ(broker.callIfIdle(killRequest(), WAIT).error_or(""), "not connected");
}

TEST(PipeBrokerTest, AStoppedConnectGivesUpOnThePromptAndAsksAgainOnceItIsAnswered)
{
    PromptUp prompt;
    PipeBroker broker(prompt.launcher());
    std::stop_source stop;
    auto connecting = std::async(std::launch::async, [&broker, token = stop.get_token()] { return broker.connectWithin(token, 60s); });
    ASSERT_TRUE(prompt.shown());
    const auto start = std::chrono::steady_clock::now();
    stop.request_stop();
    ASSERT_EQ(connecting.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(connecting.get().error_or(""), "cancelled");
    EXPECT_LT(since(start), AT_ONCE);

    // Windows can't take the prompt down: until it is answered, a second connect doesn't raise another.
    EXPECT_EQ(broker.connectWithin({}, WAIT).error_or(""), std::string(PipeBroker::ALREADY_ASKING));
    prompt.answer();
    ASSERT_TRUE(prompt.returned());
    std::expected<void, std::string> again = std::unexpected(std::string(PipeBroker::ALREADY_ASKING));
    const auto retried = std::chrono::steady_clock::now();
    while (!again.has_value() && again.error() == std::string(PipeBroker::ALREADY_ASKING) && since(retried) < WAIT)
    {
        std::this_thread::sleep_for(10ms); // the launch's result follows its return at once
        again = broker.connectWithin({}, WAIT);
    }
    EXPECT_EQ(again.error_or(""), "asked again");
}

TEST(PipeBrokerTest, APromptLeftUnansweredTimesOut)
{
    const PromptUp prompt; // answered on scope exit
    PipeBroker broker(prompt.launcher());
    const auto start = std::chrono::steady_clock::now();
    const auto connected = broker.connectWithin({}, 200ms);
    ASSERT_FALSE(connected.has_value());
    // The prompt stays up, so the user is told what a late answer does (#2768).
    EXPECT_EQ(connected.error(),
              "the administrator's consent wasn't given in time; consent given later isn't acted on, so retry the action");
    EXPECT_EQ(connected.error(), std::string(PipeBroker::PROMPT_TIMED_OUT));
    EXPECT_LT(since(start), WAIT);
    EXPECT_FALSE(broker.connected());
}

TEST(PipeBrokerTest, TheDestructorDoesntWaitForAPrompt)
{
    PromptUp prompt;
    auto broker = std::make_unique<PipeBroker>(prompt.launcher());
    auto connecting = std::async(std::launch::async, [raw = broker.get()] { return raw->connectWithin({}, 60s); });
    ASSERT_TRUE(prompt.shown());
    const auto start = std::chrono::steady_clock::now();
    broker.reset();
    EXPECT_LT(since(start), AT_ONCE) << "the prompt is still up";
    ASSERT_EQ(connecting.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(connecting.get().error_or(""), "cancelled");
    // The launch thread outlives the broker; answered, it returns and lets go of what it shares.
    prompt.answer();
    EXPECT_TRUE(prompt.returned());
}

// #2572 review, finding 2: a prompt approved after its connect was stopped isn't wasted. The next
// connect uses the helper it started, rather than raise a second prompt. (One approved after its
// connect timed out is dismissed instead, #2768: the test after the next.)
TEST(PipeBrokerTest, AHelperApprovedAfterItsConnectGaveUpIsUsedByTheNext)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    InProcessHelper helper(serving(CountingHandler{}.handler(), WAIT));
    PromptUp prompt(helper.launcher());
    PipeBroker broker(prompt.launcher());
    std::stop_source stop;
    auto connecting = std::async(std::launch::async, [&broker, token = stop.get_token()] { return broker.connectWithin(token, 60s); });
    ASSERT_TRUE(prompt.shown());
    stop.request_stop();
    ASSERT_EQ(connecting.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(connecting.get().error_or(""), "cancelled");

    prompt.answer(); // approved: the helper starts, and waits for its client
    ASSERT_TRUE(prompt.returned());
    std::expected<void, std::string> again = std::unexpected(std::string(PipeBroker::ALREADY_ASKING));
    const auto retried = std::chrono::steady_clock::now();
    while (!again.has_value() && again.error() == std::string(PipeBroker::ALREADY_ASKING) && since(retried) < WAIT)
    {
        std::this_thread::sleep_for(10ms); // the launch's result follows its return at once
        again = broker.connectWithin({}, WAIT);
    }
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_TRUE(broker.connected());
    EXPECT_TRUE(broker.call(killRequest(), {}).has_value());
    EXPECT_EQ(prompt.runs(), 1) << "one prompt, not two";
}

// #2572 review, finding 3: a launch given up on is told so through its stop token, timeout included.
TEST(PipeBrokerTest, ALaunchGivenUpOnIsStopped)
{
    auto released = std::make_shared<std::promise<void>>();
    PipeBroker broker(
        [released](const std::wstring&, const std::stop_token& stopToken) -> std::expected<UniqueHandle, std::string>
        {
            const auto deadline = std::chrono::steady_clock::now() + WAIT;
            while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(10ms);
            }
            if (stopToken.stop_requested())
            {
                released->set_value();
            }
            return std::unexpected(std::string("stopped"));
        });
    const auto connected = broker.connectWithin({}, 100ms);
    EXPECT_EQ(connected.error_or(""), std::string(PipeBroker::PROMPT_TIMED_OUT));
    EXPECT_EQ(released->get_future().wait_for(WAIT), std::future_status::ready) << "the launcher's token was stopped";
}

// #2768: a prompt approved after its connect timed out starts a helper nothing will use. The action
// was reported as failed, and running it minutes later would surprise, so the helper is dismissed as
// soon as it has made its pipe (its launch thread opens and closes the pipe, which the helper takes as
// its client's close), never left waiting out its accept timeout for a client that won't come, and the
// next connect asks again.
TEST(PipeBrokerTest, AHelperApprovedAfterItsConnectTimedOutIsDismissedAtOnce)
{
    if (const auto why = brokerRefusesThisUser())
    {
        GTEST_SKIP() << "the broker doesn't serve this user: " << *why;
    }
    auto end = std::make_shared<std::atomic<int>>(-1);
    const CountingHandler counting;
    InProcessHelper helper(serving(counting.handler(), WAIT, end));
    PromptUp prompt(helper.launcher());
    PipeBroker broker(prompt.launcher());
    const auto connected = broker.connectWithin({}, 200ms);
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), std::string(PipeBroker::PROMPT_TIMED_OUT));
    EXPECT_FALSE(broker.connected());
    ASSERT_TRUE(prompt.shown()) << "the prompt is still up";
    EXPECT_FALSE(helper.endedAlready()) << "no helper yet: the prompt is unanswered";

    prompt.answer(); // approved, too late: the helper starts, makes its pipe and waits for its client
    ASSERT_TRUE(prompt.returned());
    // Nothing of this test's connects to it: the broker's launch thread does, and closes the pipe again.
    ASSERT_TRUE(helper.ended()) << "the helper was left waiting for a client";
    EXPECT_EQ(end->load(), static_cast<int>(BrokerPipeServer::SessionEnd::ClientClosed)) << "dismissed as a client's close, not timed out";
    EXPECT_EQ(counting.calls->load(), 0) << "nothing was asked of it";
    EXPECT_FALSE(broker.connected());

    // The next connect asks again, rather than use the dismissed helper.
    std::expected<void, std::string> again = std::unexpected(std::string(PipeBroker::ALREADY_ASKING));
    const auto retried = std::chrono::steady_clock::now();
    while (!again.has_value() && again.error() == std::string(PipeBroker::ALREADY_ASKING) && since(retried) < WAIT)
    {
        std::this_thread::sleep_for(10ms); // the launch's result follows its return at once
        again = broker.connectWithin({}, WAIT);
    }
    EXPECT_EQ(again.error_or(""), "asked again");
    EXPECT_EQ(prompt.runs(), 2) << "a second prompt, not the dismissed helper";
    EXPECT_FALSE(broker.connected());
}

// #2768: the launch thread, which may sit behind a UAC prompt for minutes, is named for WPA and the
// Threads window, as every TaskSmack thread is (Platform/ThreadName.h).
TEST(PipeBrokerTest, TheLauncherRunsOnANamedThread)
{
    static_assert(HELPER_LAUNCH_THREAD_NAME == "ts-uac-launch");
    static_assert(HELPER_LAUNCH_THREAD_NAME.size() <= MAX_LINUX_THREAD_NAME_LENGTH);
    auto observed = std::make_shared<std::promise<std::wstring>>();
    PipeBroker broker(
        [observed](const std::wstring&, const std::stop_token&) -> std::expected<UniqueHandle, std::string>
        {
            PWSTR description = nullptr;
            const HRESULT read = GetThreadDescription(GetCurrentThread(), &description);
            observed->set_value(SUCCEEDED(read) && description != nullptr ? std::wstring(description) : std::wstring(L"<error>"));
            LocalFree(description); // the OS allocated it; null is fine
            return std::unexpected(std::string("unused"));
        });
    EXPECT_EQ(broker.connectWithin({}, WAIT).error_or(""), "unused");
    auto named = observed->get_future();
    ASSERT_EQ(named.wait_for(WAIT), std::future_status::ready);
    EXPECT_EQ(named.get(), L"ts-uac-launch");
}

} // namespace
} // namespace Platform::Windows
