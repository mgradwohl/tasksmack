#pragma once

// What the privileged helper does with each BrokerProtocol request (#2307, #2309): TaskSmackHelper.exe
// on Windows (Windows/BrokerPipeServer.h, #2517), which hands it its own actions and readers; the transport and
// its session rules stay the helper's. Linux had one too, until #3580: a Linux session is now Administrator
// (TaskSmack started as root) or Standard for its whole life.
//
// handleRequest() is a pure mapping, unit-tested with mocks. It checks nothing about a target itself:
// the platform's actions and readers re-verify each one (PID and start time) before they act or read.

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"

#include <chrono>
#include <functional>

namespace Platform::BrokerServer
{

/// How long the helper reads one OpenFilesBatchRequest for (#3578) before it answers with the results so far, well
/// inside TaskSmack's wait for a reply (PipeBroker::CALL_TIMEOUT, 10 s): the client asks again for the rest.
inline constexpr std::chrono::milliseconds OPEN_FILES_BATCH_BUDGET{4000};

/// The readers the helper answers Process Details' reads through (#2309); a read without its reader is an
/// ErrorReply.
struct Readers
{
    IProcessEnvironmentReader* environment = nullptr;
    IProcessModulesReader* modules = nullptr;
    IProcessOpenFilesReader* openFiles = nullptr;
    IProcessConnectionsReader* connections = nullptr;
    IProcessThreadsReader* threads = nullptr;     ///< Process Details' threads (#3069)
    IProcessMemoryMapReader* memoryMap = nullptr; ///< Process Details' memory map (#3665)
    /// Replace Task Manager (#3072): Windows' helper sets or clears taskmgr.exe's IFEO Debugger, for the
    /// TaskSmack.exe beside it only. A helper without it answers with an error.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<Broker::ActionResult(const Broker::TaskManagerReplacementRequest&)> taskManagerReplacement{};
    /// The Users tab's Disconnect and Sign out of another user's session (#3076): Windows' helper, after it
    /// checks the session against a fresh list. A helper without it answers with an error.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::function<ServiceActionResult(const Broker::SessionActionRequest&)> sessionAction{};
    /// Where an OpenFilesBatchRequest's budget (OPEN_FILES_BATCH_BUDGET) reads the time: empty is steady_clock.
    /// Tests pass one they step from a reader.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    Broker::IdleClock clock{};
};

/// The answer to @p request: a HelloReply to the handshake, an ActionResult to a process action through
/// @p actions, a read's reply through its reader in @p readers. Never throws past it: an exception from
/// @p actions is a failed ActionResult, one from a reader its kind's failure.
[[nodiscard]] Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, const Readers& readers = {});

} // namespace Platform::BrokerServer
