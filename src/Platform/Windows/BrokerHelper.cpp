#include "BrokerHelper.h"

#include "Platform/IProcessActions.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessIdentity.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace Platform::Windows::BrokerHelper
{

namespace
{

constexpr std::wstring_view BROKER_FLAG = L"--broker";

/// @p text as an unsigned decimal number no larger than @p max and not 0: digits only, no sign, no
/// spaces, no leading zero, so each number has exactly one spelling.
[[nodiscard]] std::expected<std::uint64_t, std::string> parsePositive(std::wstring_view text, std::uint64_t max, std::string_view what)
{
    constexpr std::size_t MAX_DIGITS = 20; // std::uint64_t's largest has 20
    if (text.empty() || text.size() > MAX_DIGITS || text.front() == L'0')
    {
        return std::unexpected(std::format("{} isn't a positive decimal number", what));
    }
    std::uint64_t value = 0;
    constexpr std::uint64_t BASE = 10;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            return std::unexpected(std::format("{} isn't a positive decimal number", what));
        }
        const auto digit = static_cast<std::uint64_t>(c - L'0');
        if (value > (max - digit) / BASE)
        {
            return std::unexpected(std::format("{} is out of range", what));
        }
        value = (value * BASE) + digit;
    }
    return value;
}

[[nodiscard]] std::string lastErrorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// Why openProcessVerified() refused the parent, in the helper's words.
[[nodiscard]] std::string parentRefusal(const IdentityFailure& failure)
{
    switch (failure.kind)
    {
    case IdentityFailureKind::InvalidPid:
    case IdentityFailureKind::IdentityUnknown:
        return "TaskSmack's process wasn't named";
    case IdentityFailureKind::ProcessGone:
    case IdentityFailureKind::Exited:
        return "TaskSmack has exited";
    case IdentityFailureKind::Mismatch:
        return "the process with TaskSmack's PID isn't the TaskSmack that started the helper";
    case IdentityFailureKind::AccessDenied:
    case IdentityFailureKind::OpenFailed:
    case IdentityFailureKind::TimesFailed:
        break;
    }
    return std::format("couldn't open TaskSmack's process: {}", lastErrorText(failure.win32Error));
}

} // namespace

std::expected<Arguments, std::string> parseArguments(std::span<const std::wstring_view> arguments)
{
    constexpr std::size_t COUNT = 4;
    if (arguments.size() != COUNT || arguments[0] != BROKER_FLAG)
    {
        return std::unexpected(std::string("usage: --broker <pipe name> <parent PID> <parent start time>"));
    }
    if (!BrokerPipe::isPipeName(arguments[1]))
    {
        return std::unexpected(std::string("the pipe name isn't one TaskSmack makes"));
    }
    const auto pid = parsePositive(arguments[2], std::numeric_limits<DWORD>::max(), "the parent's PID");
    if (!pid.has_value())
    {
        return std::unexpected(pid.error());
    }
    const auto ticks = parsePositive(arguments[3], std::numeric_limits<std::uint64_t>::max(), "the parent's start time");
    if (!ticks.has_value())
    {
        return std::unexpected(ticks.error());
    }
    return Arguments{.pipeName = std::wstring(arguments[1]), .parentPid = static_cast<DWORD>(*pid), .parentStartTicks = *ticks};
}

std::wstring commandLine(const Arguments& arguments)
{
    return std::format(L"{} {} {} {}", BROKER_FLAG, arguments.pipeName, arguments.parentPid, arguments.parentStartTicks);
}

std::expected<UniqueHandle, std::string> openParent(DWORD pid, std::uint64_t startTicks)
{
    if (pid == 0 || pid > static_cast<DWORD>(std::numeric_limits<std::int32_t>::max()))
    {
        return std::unexpected(std::string("TaskSmack's process wasn't named"));
    }
    // The one identity check every action uses (#2149): the creation time is read from the handle
    // opened, so the process checked is the process held.
    auto process = openProcessVerified({}, ProcessTarget{.pid = static_cast<std::int32_t>(pid), .startTimeTicks = startTicks}, SYNCHRONIZE);
    if (!process.has_value())
    {
        return std::unexpected(parentRefusal(process.error()));
    }
    return UniqueHandle(process->release());
}

ProcessWatch::ProcessWatch(HANDLE process, const std::stop_token& outer)
    : m_Wake(CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_Outer(outer, [this] { m_Stop.request_stop(); })
{
    if (!m_Wake)
    {
        throw std::runtime_error(std::format("couldn't watch TaskSmack's process: {}", lastErrorText(GetLastError())));
    }
    m_Thread = std::jthread(
        [this, process]
        {
            const std::array<HANDLE, 2> handles{process, m_Wake.get()};
            // Either ends the wait: the process (or a failed wait) stops the session, the wake ends this thread.
            if (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            {
                m_Stop.request_stop();
            }
        });
}

ProcessWatch::~ProcessWatch()
{
    SetEvent(m_Wake.get()); // m_Thread, joined next, returns at once
}

Exit run(const Arguments& arguments,
         const BrokerPipeServer::RequestHandler& handler,
         std::chrono::milliseconds idleTimeout,
         std::chrono::milliseconds acceptTimeout,
         const std::stop_token& stopToken)
{
    try
    {
        // TaskSmack's process, held from here to the end, so no other process can take its PID.
        const auto parent = openParent(arguments.parentPid, arguments.parentStartTicks);
        if (!parent.has_value())
        {
            return Exit::NoParent;
        }
        // Whom the pipe lets in: TaskSmack's user, from its token, which needn't be the helper's own.
        const auto user = BrokerPipe::userSidOfProcess(parent->get());
        if (!user.has_value())
        {
            return Exit::NoParent;
        }
        const ProcessWatch watch(parent->get(), stopToken);
        const auto pipe = BrokerPipe::createServerPipe(arguments.pipeName, *user);
        if (!pipe.has_value())
        {
            return Exit::NoClient;
        }
        if (!BrokerPipe::acceptClient(pipe->get(), parent->get(), acceptTimeout, watch.token()).has_value())
        {
            return Exit::NoClient;
        }
        switch (BrokerPipeServer::serve(pipe->get(), handler, idleTimeout, watch.token()))
        {
        case BrokerPipeServer::SessionEnd::ClientClosed:
        case BrokerPipeServer::SessionEnd::Idle:
        case BrokerPipeServer::SessionEnd::Stopped:
            return Exit::Done;
        case BrokerPipeServer::SessionEnd::HandshakeFailed:
        case BrokerPipeServer::SessionEnd::Broken:
            break;
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch) - an allocation failure or the like: TaskSmack sees the pipe close
    {}
    return Exit::Failed;
}

} // namespace Platform::Windows::BrokerHelper
