#pragma once

// The privileged helper's side of the broker (#2307): reads BrokerProtocol requests from its client
// over a BrokerSocket and answers each through LinuxProcessActions, which (running as root) re-verifies
// every target by PID and start time before it acts. handleRequest() is the pure mapping, unit-tested
// with a mock; serve() is the loop tasksmack-helper runs.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>

namespace Platform::BrokerServer
{

/// The answer to @p request through @p actions: a HelloReply to the handshake, an ActionResult to a
/// process action. Never throws past it: an exception from @p actions is a failed ActionResult.
/// The readers the helper answers Process Details' reads through (#2309); a read without its reader is
/// an ErrorReply.
struct Readers
{
    IProcessEnvironmentReader* environment = nullptr;
    IProcessModulesReader* modules = nullptr;
    IProcessOpenFilesReader* openFiles = nullptr;
    IProcessConnectionsReader* connections = nullptr;
    /// PSS and USS: process ID and start time to the read's result.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<MemoryRollupRead(std::int32_t, std::uint64_t)> memoryRollup{};
    /// System Information's root-only firmware files (#2310)
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<Broker::FirmwareFileReply(Broker::FirmwareFile)> firmwareFile{};
    /// System Information's Recent crashes, every user's (#2310)
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<CrashesInfo()> crashes{};
};

/// A read request goes to its reader in @p readers.
[[nodiscard]] Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, const Readers& readers = {});

/// How a session ended, for the helper's exit status and the tests.
enum class SessionEnd : std::uint8_t
{
    ClientClosed,    ///< The client closed its end (or exited): the normal end
    Idle,            ///< No Hello or action within the idle timeout
    Stopped,         ///< The stop token was stopped
    HandshakeFailed, ///< The first frame wasn't an accepted Hello
    Broken,          ///< The socket failed
};

/// Answers requests on @p fd until the client closes it, no Hello or action arrives for @p idleTimeout
/// (reads come on their own timers, so they don't put it off), or @p stopToken is stopped. The first
/// frame must be a Hello of this protocol version; until then nothing else is acted on. A frame that
/// doesn't decode gets an ErrorReply, and the session goes on.
[[nodiscard]] SessionEnd serve(
    int fd, IProcessActions& actions, std::chrono::milliseconds idleTimeout, const std::stop_token& stopToken, const Readers& readers = {});

} // namespace Platform::BrokerServer
