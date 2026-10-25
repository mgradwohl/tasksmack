#pragma once

// systemd-coredump's journal entries through sd-journal (libsystemd), for the Recent crashes section
// (#1674). Optional at build time like SystemdBus: without libsystemd (TASKSMACK_HAS_SDBUS undefined) the
// reader reports that it can't, and the section lists /var/lib/systemd/coredump alone. Read on the System
// Information page's worker thread, never per frame.

#include "Platform/Linux/LinuxCoredumps.h"

#include <cstdint>
#include <optional>

namespace Platform::SystemdJournal
{

/// A reader of the local journal's systemd-coredump entries (MESSAGE_ID fc2e22bc...). It reads only the
/// COREDUMP_* fields LinuxCoredumps::crashFromJournal() uses -- never the core itself, which the journal
/// may hold too. A user reads their own entries; others' need the systemd-journal or adm group.
/// Each read looks systemd-coredump's user up first (lookUpCoredumpUid()).
[[nodiscard]] LinuxCoredumps::JournalReader makeCoredumpJournalReader();

/// The same reader, given systemd-coredump's user ID instead of looking it up: tasksmack-helper looks it
/// up once, before its seccomp filter leaves NSS no sockets or modules to load (#2752).
[[nodiscard]] LinuxCoredumps::JournalReader makeCoredumpJournalReader(std::optional<std::uint32_t> coredumpUid);

/// The user systemd-coredump drops to, through NSS (getpwnam_r), or nullopt when there is none (or this
/// build reads no journal). Thread-safe.
[[nodiscard]] std::optional<std::uint32_t> lookUpCoredumpUid();

} // namespace Platform::SystemdJournal
