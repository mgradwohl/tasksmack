#pragma once

// The sd-bus plumbing every system-bus reader shares (#2171): the connection, error and message
// owners, opening the system bus with CALL_TIMEOUT_USEC on every call, and telling a timeout from an
// answer. The readers themselves are split by service: SystemdUnits.cpp (systemd's units and boot
// timestamps), UDisks2Smart.cpp, NetworkManager.cpp (Wi-Fi and DHCP) and NetworkdResolved.cpp
// (networkd's DHCP and resolved's DNS). Their API stays in SystemdBus.h. Only for those files, and
// LinuxProcessActions.cpp, whose Open file location borrows the owners for one session-bus call (#3064).

#ifdef TASKSMACK_HAS_SDBUS

#include "Platform/Linux/SystemdBus.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

// Newer libsystemd (259 here) declares sd_bus and sd_bus_error in sd-bus-protocol.h; older releases
// (Ubuntu 24.04's, in CI) declare them in sd-bus.h, so include-cleaner wants this line on one and not the other.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>

namespace Platform::SystemdBus::Detail
{

/// Owns an sd_bus connection.
class Bus
{
  public:
    Bus() = default;
    ~Bus()
    {
        if (m_Bus != nullptr && m_Borrowed)
        {
            sd_bus_unref(m_Bus); // another Bus owns the connection: leave it open
        }
        else if (m_Bus != nullptr)
        {
            sd_bus_flush_close_unref(m_Bus);
        }
    }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;

    [[nodiscard]] sd_bus** out() noexcept
    {
        return &m_Bus;
    }
    [[nodiscard]] sd_bus* get() const noexcept
    {
        return m_Bus;
    }
    /// Uses @p bus, a connection another Bus owns, without closing it here (#2728). Call on an empty Bus.
    void borrow(sd_bus* bus) noexcept
    {
        m_Bus = sd_bus_ref(bus);
        m_Borrowed = true;
    }

  private:
    sd_bus* m_Bus = nullptr;
    bool m_Borrowed = false;
};

/// Opens @p bus on the system bus with CALL_TIMEOUT_USEC on every call; sd_bus_open_system()'s result.
[[nodiscard]] inline int openSystemBus(Bus& bus)
{
    const int result = sd_bus_open_system(bus.out());
    if (result >= 0)
    {
        (void) sd_bus_set_method_call_timeout(bus.get(), CALL_TIMEOUT_USEC);
    }
    return result;
}

/// Whether a call failed by running out of time rather than with an answer.
[[nodiscard]] inline bool timedOut(const sd_bus_error& error, int result) noexcept
{
    const auto named = [&error](std::string_view name)
    {
        return error.name != nullptr && name == error.name;
    };
    return result == -ETIMEDOUT || named("org.freedesktop.DBus.Error.Timeout") || named("org.freedesktop.DBus.Error.NoReply");
}

/// Owns an sd_bus_error.
class BusError
{
  public:
    BusError() = default;
    ~BusError()
    {
        sd_bus_error_free(&m_Error);
    }
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    BusError(BusError&&) = delete;
    BusError& operator=(BusError&&) = delete;

    [[nodiscard]] sd_bus_error* get() noexcept
    {
        return &m_Error;
    }

    [[nodiscard]] const sd_bus_error& raw() const noexcept
    {
        return m_Error;
    }

    [[nodiscard]] bool is(const char* name) const noexcept
    {
        return m_Error.name != nullptr && std::string_view(m_Error.name) == name;
    }

    /// The error's message, or @p result's errno text when the bus gave none.
    [[nodiscard]] std::string text(int result) const
    {
        return (m_Error.message != nullptr) ? std::string(m_Error.message)
                                            : std::string(std::strerror(-result)); // NOLINT(concurrency-mt-unsafe) - one worker thread
    }

  private:
    sd_bus_error m_Error{}; // SD_BUS_ERROR_NULL, written as C++
};

/// Owns an sd_bus_message.
class Message
{
  public:
    Message() = default;
    ~Message()
    {
        sd_bus_message_unref(m_Message);
    }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) = delete;
    Message& operator=(Message&&) = delete;

    [[nodiscard]] sd_bus_message** out() noexcept
    {
        return &m_Message;
    }

    [[nodiscard]] sd_bus_message* get() const noexcept
    {
        return m_Message;
    }

  private:
    sd_bus_message* m_Message = nullptr;
};

} // namespace Platform::SystemdBus::Detail

#endif
