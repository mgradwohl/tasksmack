#pragma once

// What Count UDP traffic per process is doing (#3366), as the Processes panel's collector last said: Settings shows it
// beside the setting, and keeps a counter known not to load from being turned on. Published by the panel on the UI
// thread when it changes, read by Settings on the same thread; a mutex keeps a later reader on another thread safe.

#include <string>
#include <string_view>

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

/// The line Settings shows under Count UDP traffic per process (#3879), on both platforms: its parts, drawn one after
/// the other with no allocation each frame. Views into string literals and the problem it was made from.
struct UdpCountingLine
{
    std::string_view lead;   ///< The line, or what comes before the collector's problem
    std::string_view detail; ///< The collector's problem; empty for a line of its own
    std::string_view end;    ///< "." after a problem that doesn't end with one
    bool warning = false;    ///< Drawn in the warning colour: this system can't count UDP

    /// The whole line (tests).
    [[nodiscard]] std::string text() const;
};

/// What Settings says about counting UDP, from what the collector last said and whether the setting is @p on (#3879):
/// counting, this system can't count, the collector's reason it isn't counting (not administrator; on Windows, a
/// trace that didn't start), and otherwise what turning it on or off does.
[[nodiscard]] UdpCountingLine udpCountingLine(bool on, bool delivering, bool unsupported, std::string_view problem) noexcept;

} // namespace App
