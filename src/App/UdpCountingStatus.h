#pragma once

// What Count UDP traffic per process is doing (#3366), as the Processes panel's collector last said: Settings shows it
// beside the setting, and keeps a counter known not to load from being turned on. Published by the panel on the UI
// thread when it changes, read by Settings on the same thread; a mutex keeps a later reader on another thread safe.

#include <string>

namespace App
{

struct UdpCountingStatus
{
    bool delivering = false;  ///< UDP is in the per-process counters now
    bool unsupported = false; ///< The counter couldn't be loaded or read on this system
    std::string problem{};    // NOLINT(readability-redundant-member-init) - {} lets designated initializers omit it

    friend bool operator==(const UdpCountingStatus&, const UdpCountingStatus&) = default;
};

/// The status a collector last published; all defaults before one has.
[[nodiscard]] UdpCountingStatus udpCountingStatus();

/// Publishes @p status, replacing the last one.
void publishUdpCountingStatus(UdpCountingStatus status);

} // namespace App
