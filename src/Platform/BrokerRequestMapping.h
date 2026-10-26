#pragma once

// What the privileged helper does with each BrokerProtocol request (#2307, #2309, #2310), shared by
// both helpers (#2181, decision 4: one protocol): tasksmack-helper on Linux (Linux/BrokerServer.h) and
// TaskSmackHelper.exe on Windows (Windows/BrokerPipeServer.h, #2517). Each hands it its own platform's
// actions and readers; the transport and its session rules stay the helper's.
//
// handleRequest() is a pure mapping, unit-tested with mocks. It checks nothing about a target itself:
// the platform's actions and readers re-verify each one (PID and start time) before they act or read.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceActions.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace Platform::BrokerServer
{

/// The readers the helper answers Process Details' and System Information's reads through (#2309,
/// #2310); a read without its reader is an ErrorReply.
struct Readers
{
    IProcessEnvironmentReader* environment = nullptr;
    IProcessModulesReader* modules = nullptr;
    IProcessOpenFilesReader* openFiles = nullptr;
    IProcessConnectionsReader* connections = nullptr;
    /// PSS and USS: process ID and start time to the read's result.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<MemoryRollupRead(std::int32_t, std::uint64_t)> memoryRollup{};
    /// One process's FD count, I/O bytes and (when asked) socket inodes, for ReadProcessCounters (#2728); nullopt when the target has
    /// exited or its PID now names another process.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<std::optional<Broker::ProcessCountersEntry>(const ProcessTarget&, bool withSockets)> processCounters{};
    /// The processes hidepid hides from TaskSmack, as their /proc files' text (#2728): every one whose PID
    /// isn't among the visible PIDs given.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<Broker::HiddenProcessesReply(std::span<const std::int32_t>)> hiddenProcesses{};
    /// A service started, stopped, restarted or given a start type (#2728): Linux's helper, as root over the
    /// system bus it opened before its sandbox. The reader checks the unit name.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<ServiceActionResult(const Broker::ServiceActionRequest&)> serviceAction{};
    /// System Information's privileged firmware files (#2310)
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<Broker::FirmwareFileReply(Broker::FirmwareFile)> firmwareFile{};
    /// System Information's Recent crashes, every user's (#2310)
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<CrashesInfo()> crashes{};
};

/// The answer to @p request: a HelloReply to the handshake, an ActionResult to a process action through
/// @p actions, a read's reply through its reader in @p readers. Never throws past it: an exception from
/// @p actions is a failed ActionResult, one from a reader its kind's failure.
[[nodiscard]] Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, const Readers& readers = {});

} // namespace Platform::BrokerServer
