#pragma once

// systemd-coredump's journal entries through sd-journal (libsystemd), for the Recent crashes section
// (#1674). Optional at build time like SystemdBus: without libsystemd (TASKSMACK_HAS_SDBUS undefined) the
// reader reports that it can't, and the section lists /var/lib/systemd/coredump alone. Read on the System
// Information page's worker thread, never per frame.

#include "Platform/Linux/LinuxCoredumps.h"

namespace Platform::SystemdJournal
{

/// A reader of the local journal's systemd-coredump entries (MESSAGE_ID fc2e22bc...). It reads only the
/// COREDUMP_* fields LinuxCoredumps::crashFromJournal() uses -- never the core itself, which the journal
/// may hold too. A user reads their own entries; others' need the systemd-journal or adm group.
[[nodiscard]] LinuxCoredumps::JournalReader makeCoredumpJournalReader();

} // namespace Platform::SystemdJournal
