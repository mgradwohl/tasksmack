#pragma once

// The session rules the privileged broker's server keeps (#2307, #2311): Windows' pipe helper's, since Linux's
// socket helper went with #3580. Not part of the wire format:
// BrokerProtocol.h says what a frame carries, this says what the helper does with the session.

#include "Platform/BrokerProtocol.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>

namespace Platform::Broker
{

/// What a request kind is to a session (#2550, #3783): the handshake, an action the user chose, or a read.
/// Which one a kind is is a security property: a one-action session (#2550) ends after the first request
/// that isn't the Hello or a read, and the over-the-shoulder read filter (BrokerHelper::readsOnlyOwnProcesses)
/// refuses every read it doesn't know.
enum class RequestCategory : std::uint8_t
{
    Hello,
    Action,
    Read,
};

/// Each request kind's one trait (#3783): its category, and whether it puts off the helper's idle exit.
/// Only what the user does puts it off, the Hello (which an Authenticate and retry or Authenticate to read
/// connects with) and an action. Every read kind can come on its own: Process Details re-reads
/// Environment, Modules, Open files and Connections through a connected helper on its refresh timer
/// (#2369, #2405).
///
/// There is no primary definition, so a kind added to Request without a specialization here fails to
/// compile (the static_assert below says so). A new read needs BrokerHelper::readsOnlyOwnProcesses
/// revisited too, or the over-the-shoulder filter refuses it.
template<typename Kind> struct RequestKind;

/// The handshake.
template<> struct RequestKind<Hello>
{
    static constexpr RequestCategory CATEGORY = RequestCategory::Hello;
    static constexpr bool PUTS_OFF_THE_IDLE_EXIT = true;
};

/// An action the user chose: what a one-action session's consent is for, and what puts off the idle exit.
struct ActionKind
{
    static constexpr RequestCategory CATEGORY = RequestCategory::Action;
    static constexpr bool PUTS_OFF_THE_IDLE_EXIT = true;
};

/// A read: never the one action, and never puts off the idle exit.
struct ReadKind
{
    static constexpr RequestCategory CATEGORY = RequestCategory::Read;
    static constexpr bool PUTS_OFF_THE_IDLE_EXIT = false;
};

// Every signal: Terminate, Kill, and since #2582 Suspend and Resume (NtSuspendProcess, NtResumeProcess).
template<> struct RequestKind<SignalRequest> : ActionKind
{};
template<> struct RequestKind<PriorityRequest> : ActionKind
{};
// An I/O priority hint (#3362): High needs an administrator.
template<> struct RequestKind<IoPriorityRequest> : ActionKind
{};
template<> struct RequestKind<AffinityRequest> : ActionKind
{};
// Efficiency mode (#3071).
template<> struct RequestKind<EfficiencyModeRequest> : ActionKind
{};
// Close connection (#3358): SetTcpEntry needs an administrator for any connection.
template<> struct RequestKind<CloseConnectionRequest> : ActionKind
{};
// Replace Task Manager (#3072) isn't a process action but is one the user chose, so it is the one action of an
// over-the-shoulder session too.
template<> struct RequestKind<TaskManagerReplacementRequest> : ActionKind
{};
// Group: User's Disconnect and Sign out of another user's session (#3076).
template<> struct RequestKind<SessionActionRequest> : ActionKind
{};
// One thread suspended, resumed or ended (#3069).
template<> struct RequestKind<ThreadActionRequest> : ActionKind
{};

// Process Details' reads (#2309).
template<> struct RequestKind<EnvironmentRequest> : ReadKind
{};
template<> struct RequestKind<ModulesRequest> : ReadKind
{};
template<> struct RequestKind<OpenFilesRequest> : ReadKind
{};
template<> struct RequestKind<ConnectionsRequest> : ReadKind
{};
// Process Details' threads (#3069).
template<> struct RequestKind<ThreadsRequest> : ReadKind
{};
// Find handles' administrator pass (#3578).
template<> struct RequestKind<OpenFilesBatchRequest> : ReadKind
{};
// Process Details' memory map (#3665): the region list only, never the process's memory.
template<> struct RequestKind<MemoryMapRequest> : ReadKind
{};

/// Whether @p Kind has its RequestKind trait.
template<typename Kind>
concept SortedRequestKind = requires {
    { RequestKind<Kind>::CATEGORY } -> std::convertible_to<RequestCategory>;
    { RequestKind<Kind>::PUTS_OFF_THE_IDLE_EXIT } -> std::convertible_to<bool>;
};

namespace Detail
{

template<typename Variant> inline constexpr bool EVERY_KIND_SORTED = false;
template<typename... Kinds> inline constexpr bool EVERY_KIND_SORTED<std::variant<Kinds...>> = (SortedRequestKind<Kinds> && ...);

/// Each kind's traits, by its index in the variant: built from RequestKind, so a kind with no trait fails to compile.
template<typename Variant> struct KindTable;
template<typename... Kinds> struct KindTable<std::variant<Kinds...>>
{
    static constexpr std::array<RequestCategory, sizeof...(Kinds)> CATEGORIES{RequestKind<Kinds>::CATEGORY...};
    static constexpr std::array<bool, sizeof...(Kinds)> PUTS_OFF_THE_IDLE_EXIT{RequestKind<Kinds>::PUTS_OFF_THE_IDLE_EXIT...};
};

} // namespace Detail

static_assert(Detail::EVERY_KIND_SORTED<Request>, "give the new request kind its RequestKind trait: the Hello, an action or a read");

/// Whether @p request is of @p category. A valueless request (index variant_npos) is of none, so it counts as an
/// action, never a read.
[[nodiscard]] constexpr bool isRequestOf(const Request& request, RequestCategory category) noexcept
{
    constexpr const auto& categories = Detail::KindTable<Request>::CATEGORIES;
    const std::size_t index = request.index();
    return index < categories.size() && categories[index] == category;
}

/// Whether @p request puts off the helper's idle exit: its kind's RequestKind trait. A valueless request doesn't.
[[nodiscard]] constexpr bool putsOffTheIdleExit(const Request& request) noexcept
{
    constexpr const auto& putsOff = Detail::KindTable<Request>::PUTS_OFF_THE_IDLE_EXIT;
    const std::size_t index = request.index();
    return index < putsOff.size() && putsOff[index];
}

/// Where a server's idle timeout reads the time: an empty one is steady_clock. Tests pass a clock they
/// step from the handler, so whether a session ends idle doesn't hang on how fast the machine is (#2546).
using IdleClock = std::function<std::chrono::steady_clock::time_point()>;

/// The time on @p clock, or steady_clock's when it is empty.
[[nodiscard]] inline std::chrono::steady_clock::time_point idleNow(const IdleClock& clock)
{
    return clock ? clock() : std::chrono::steady_clock::now();
}

} // namespace Platform::Broker
