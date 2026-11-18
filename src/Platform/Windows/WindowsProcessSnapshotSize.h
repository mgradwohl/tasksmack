#pragma once

// How big a buffer a NtQuerySystemInformation(SystemProcessInformation) snapshot starts in, and how it grows
// when the kernel answers STATUS_INFO_LENGTH_MISMATCH (#4277). Every reader of the whole snapshot shares these:
// the process sampler (WindowsProcessProbe), Process Details > Threads (WindowsProcessThreadsReader), the
// system sampler's Memory Compression search (WindowsSystemProbe) and the Connections tab's process list
// (WindowsSystemConnectionsReader). Each keeps its buffer between reads, so after its first read it starts
// from the size that read needed, and a later query fits on the first try unless the machine has grown since.
//
// Free of the OS; tested in tests/Platform/WindowsMath/test_WindowsProcessSnapshotSize.cpp.

#include <algorithm>
#include <cstddef>

namespace Platform::Windows
{

/// The first buffer for a SystemProcessInformation snapshot (#4277): 1 MiB. A desktop of 344 processes and about
/// 5,100 threads takes 618 KiB, so this fits one with over half as much again on the first try. The 512 KiB it
/// replaced was smaller than that desktop's snapshot, so the first sample walked every process and thread in the
/// kernel twice (about 3 ms of a 13 ms first sample). A bigger machine grows it once, by
/// grownProcessSnapshotBytes(), and keeps it.
inline constexpr std::size_t PROCESS_SNAPSHOT_INITIAL_BYTES = std::size_t{1024} * 1024;

/// The buffer to try next after a snapshot didn't fit in @p currentBytes, when the kernel said it needed
/// @p neededBytes (its ReturnLength; 0 when it didn't say): a quarter more than it needed, since processes and
/// threads can start between the two calls, and never less than twice the current size, so a buffer that
/// keeps falling short grows geometrically and the retries stay few.
[[nodiscard]] constexpr std::size_t grownProcessSnapshotBytes(std::size_t currentBytes, std::size_t neededBytes) noexcept
{
    return std::max(currentBytes * 2, neededBytes + (neededBytes / 4));
}

} // namespace Platform::Windows
