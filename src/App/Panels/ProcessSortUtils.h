#pragma once

#include "App/Panels/IoPriorityText.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/AsciiCase.h"
#include "Platform/ImageSignature.h"
#include "Platform/SchedPolicy.h"
#include "Platform/Utf8CaseFold.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace App::ProcessSortUtils
{

/// A value as a sort key that orders one the probe could not read (#1110) below every reading, 0
/// included (std::optional's ordering, as the GDI column already sorts): sorted by Handles without
/// root, other users' processes gather at one end instead of mixing in with real zeros.
template<typename T> [[nodiscard]] constexpr std::optional<T> readingSortKey(bool available, T value) noexcept
{
    return available ? std::optional<T>(value) : std::nullopt;
}

/// An engine list as the GPU Engine and NPU Engine columns sort it: by the number of engines, then by
/// the first engine's name. Equal lists, and two empty ones, compare false both ways.
[[nodiscard]] inline bool compareEngines(const std::vector<std::string>& a, const std::vector<std::string>& b, bool ascending)
{
    if (a.size() != b.size())
    {
        return ascending ? (a.size() < b.size()) : (b.size() < a.size());
    }
    if (!a.empty() && !b.empty())
    {
        return ascending ? (a[0] < b[0]) : (b[0] < a[0]);
    }
    return false;
}

/// The I/O Priority column's order (#3551): ascending, the highest-served first (real time, best-effort 0-7, idle;
/// Windows' hints from Critical down), descending the reverse; a process whose I/O priority is unknown (a kernel
/// thread, an unreadable one) after every known one, in both directions.
[[nodiscard]] inline bool compareIoPriority(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, bool ascending)
{
    if (a.ioPriority.has_value() != b.ioPriority.has_value())
    {
        return a.ioPriority.has_value();
    }
    const std::int32_t left = Detail::ioPriorityRank(a.ioPriority, a.nice, a.schedPolicy);
    const std::int32_t right = Detail::ioPriorityRank(b.ioPriority, b.nice, b.schedPolicy);
    return ascending ? left < right : left > right;
}

/// The Services column's text as it sorts (#1880, #4324): A to Z ignoring case, by Unicode simple case folding (Windows names services in
/// mixed case: "Appinfo", "BFE", "bits"), Z to A descending; a process hosting none sorts after every
/// one hosting some in either direction, so the hosts stay together at the top. Equal texts, and two
/// empty ones, compare false both ways.
[[nodiscard]] inline bool compareServicesText(std::string_view a, std::string_view b, bool ascending)
{
    if (a.empty() != b.empty())
    {
        return !a.empty();
    }
    // Unicode simple case folding (#4324): not std::tolower, which follows the C locale main() sets, so the app and
    // the tests (in the "C" locale) could order the column differently. Ties fall to the table's own tie-breakers.
    return ascending ? Platform::compareFolded(a, b) < 0 : Platform::compareFolded(b, a) < 0;
}

/// The column's own key comparison, with no tie-breaker: equal keys compare false both ways.
/// @return true if `a`'s key for `column` sorts before `b`'s in the given direction.
[[nodiscard]] inline bool
compareColumnKey(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, ProcessColumn column, bool ascending)
{
    const auto compare = [ascending](const auto& lhs, const auto& rhs) -> bool
    {
        return ascending ? (lhs < rhs) : (rhs < lhs);
    };
    // Text the user reads, as every other user-visible sort (#4324, #4509): by Unicode simple case folding, so "Émile"
    // sorts before "Zoe" and "bash" beside "Bash", then by bytes, so only identical texts tie.
    const auto compareText = [ascending](std::string_view lhs, std::string_view rhs) -> bool
    {
        const std::strong_ordering order = Platform::compareFoldedCase(lhs, rhs);
        return ascending ? order < 0 : order > 0;
    };

    switch (column)
    {
    case ProcessColumn::PID:
        return compare(a.pid, b.pid);
    case ProcessColumn::User: // By user name, then domain, so PC\alice's and CORP\alice's rows don't interleave (#3328)
    {
        // Folded name, folded domain, then the bytes of each: the order sortByColumn()'s keys give.
        std::strong_ordering order = Platform::compareFolded(a.user, b.user);
        if (order == 0)
        {
            order = Platform::compareFolded(a.userDomain, b.userDomain);
        }
        if (order == 0)
        {
            order = a.user <=> b.user;
        }
        if (order == 0)
        {
            order = a.userDomain <=> b.userDomain;
        }
        return ascending ? order < 0 : order > 0;
    }
    case ProcessColumn::CpuPercent:
        return compare(a.cpuPercent, b.cpuPercent);
    case ProcessColumn::MemPercent:
        return compare(a.memoryPercent, b.memoryPercent);
    case ProcessColumn::Virtual:
        return compare(a.virtualBytes, b.virtualBytes);
    case ProcessColumn::Resident:
        return compare(a.memoryBytes, b.memoryBytes);
    case ProcessColumn::PeakResident:
        return compare(a.peakMemoryBytes, b.peakMemoryBytes);
    case ProcessColumn::Shared:
        return compare(a.sharedBytes, b.sharedBytes);
    case ProcessColumn::Private:
        return compare(a.privateBytes, b.privateBytes);
    case ProcessColumn::Swap:
        return compare(a.swapBytes, b.swapBytes);
    case ProcessColumn::CpuTime:
        return compare(a.cpuTimeSeconds, b.cpuTimeSeconds);
    case ProcessColumn::StartTime:
        return compare(a.startTimeEpoch, b.startTimeEpoch);
    case ProcessColumn::State:
        return compare(a.displayState, b.displayState);
    case ProcessColumn::Status:
        return compare(a.status, b.status);
    case ProcessColumn::Name:
        return compareText(a.name, b.name);
    case ProcessColumn::PPID:
        return compare(a.parentPid, b.parentPid);
    case ProcessColumn::Priority:
        // By class, then nice: on Windows, Realtime sorts above High (#1280).
        return compare(Domain::Priority::prioritySortKey(a.priorityClass, a.nice),
                       Domain::Priority::prioritySortKey(b.priorityClass, b.nice));
    case ProcessColumn::Threads:
        return compare(a.threadCount, b.threadCount);
    case ProcessColumn::Handles:
        return compare(readingSortKey(a.handleCountAvailable, a.handleCount), readingSortKey(b.handleCountAvailable, b.handleCount));
    case ProcessColumn::PageFaults:
        return compare(a.pageFaults, b.pageFaults);
    case ProcessColumn::Affinity:
        return compare(a.cpuAffinity, b.cpuAffinity);
    case ProcessColumn::Command:
        return compareText(a.command, b.command);
    case ProcessColumn::IoRead:
        return compare(readingSortKey(a.ioAvailable, a.ioReadBytesPerSec), readingSortKey(b.ioAvailable, b.ioReadBytesPerSec));
    case ProcessColumn::IoWrite:
        return compare(readingSortKey(a.ioAvailable, a.ioWriteBytesPerSec), readingSortKey(b.ioAvailable, b.ioWriteBytesPerSec));
    case ProcessColumn::NetSent:
        return compare(readingSortKey(a.networkAvailable, a.netSentBytesPerSec), readingSortKey(b.networkAvailable, b.netSentBytesPerSec));
    case ProcessColumn::NetReceived:
        return compare(readingSortKey(a.networkAvailable, a.netReceivedBytesPerSec),
                       readingSortKey(b.networkAvailable, b.netReceivedBytesPerSec));
    case ProcessColumn::Power:
        return compare(a.powerWatts, b.powerWatts);
    case ProcessColumn::GpuPercent:
        return compare(a.gpuUtilPercent, b.gpuUtilPercent);
    case ProcessColumn::GpuMemory:
        return compare(a.gpuMemoryBytes, b.gpuMemoryBytes);
    case ProcessColumn::GpuEngine:
        return compareEngines(a.gpuEngines, b.gpuEngines, ascending);
    case ProcessColumn::GpuDevice:
        return compare(a.gpuDevices, b.gpuDevices);
    case ProcessColumn::NpuPercent:
        return compare(a.npuUtilPercent, b.npuUtilPercent);
    case ProcessColumn::NpuMemory:
        return compare(a.npuMemoryBytes, b.npuMemoryBytes);
    case ProcessColumn::NpuEngine:
        return compareEngines(a.npuEngines, b.npuEngines, ascending);
    case ProcessColumn::Publisher:
        return compare(a.publisher, b.publisher);
    case ProcessColumn::Signature: // Verified by signer, then Unsigned, Invalid, Not checked (#3070)
    {
        const std::strong_ordering order = Platform::compareSignature(a.signature.get(), b.signature.get());
        return ascending ? order < 0 : order > 0;
    }
    case ProcessColumn::Container:
        return compare(a.container, b.container);
    case ProcessColumn::Services:
        return compareServicesText(a.services, b.services, ascending);
    case ProcessColumn::Type:
        return compare(a.processType, b.processType);
    case ProcessColumn::Architecture:
        return compare(a.architecture, b.architecture);
    case ProcessColumn::Policy: // By policy (Idle .. FIFO), then real-time priority (#3086)
    {
        const std::strong_ordering order = Platform::compareSchedPolicy(a.schedPolicy, a.rtPriority, b.schedPolicy, b.rtPriority);
        return ascending ? order < 0 : order > 0;
    }
    case ProcessColumn::IoPriority: // Highest first, by the kernel's order, an unknown one last either way (#3551)
        return compareIoPriority(a, b, ascending);
    case ProcessColumn::GdiObjects:
        return compare(a.gdiObjectCount, b.gdiObjectCount);
    default:
        return false;
    }
}

/// Pure comparison logic for sorting the process table, extracted from
/// ProcessesPanel's ImGui table-sort handling so it can be unit-tested without an
/// ImGui context. Mirrors the column -> field mapping ProcessesPanel uses when
/// building the table (see ProcessColumnConfig.h for the column list).
///
/// Rows whose keys tie (most processes sit at 0.0% CPU) are ordered by PID, then by unique key,
/// in the same direction, so the order is total: std::ranges::sort is not stable, and without a
/// tie-breaker the tied rows reshuffled on every refresh (#1174).
/// @return true if `a` should sort before `b` for the given column and direction.
[[nodiscard]] inline bool
compareByColumn(const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b, ProcessColumn column, bool ascending)
{
    if (column >= ProcessColumn::Count)
    {
        return false; // Not a real column: no order at all, not even by PID.
    }
    if (compareColumnKey(a, b, column, ascending))
    {
        return true;
    }
    if (compareColumnKey(b, a, column, ascending))
    {
        return false;
    }
    if (a.pid != b.pid)
    {
        return ascending ? (a.pid < b.pid) : (b.pid < a.pid);
    }
    return ascending ? (a.uniqueKey < b.uniqueKey) : (b.uniqueKey < a.uniqueKey);
}

/// Appends @p text's sort key to @p key (#4509): its Unicode simple case fold, as Platform::appendFolded() makes it,
/// with ASCII-only text (most names and command lines) lower-cased in one pass instead of decoded a code point at
/// a time. Both give the same bytes for ASCII text: simpleCaseFold() folds A-Z and nothing else below 0x80.
inline void appendSortKey(std::string& key, std::string_view text)
{
    key.reserve(key.size() + text.size());
    if (std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80U; }))
    {
        std::ranges::transform(text, std::back_inserter(key), [](char c) { return Platform::foldAscii(c); });
        return;
    }
    Platform::appendFolded(key, text);
}

/// Whether @p column sorts by text the user reads (#4509): Name, Command and User.
[[nodiscard]] constexpr bool isTextColumn(ProcessColumn column) noexcept
{
    return column == ProcessColumn::Name || column == ProcessColumn::Command || column == ProcessColumn::User;
}

/// Sorts @p order (indices into @p rows) as compareByColumn() orders them. For a text column (#4509) each row's text
/// is folded once (Platform::appendFolded), and the sort compares those keys as bytes: a folded compare on every one of
/// the sort's n log n comparisons cost 8-10 times a byte compare for 2,000 rows with long command lines (they share
/// long prefixes). UTF-8's byte order is code-point order, so for well-formed text the keys order as
/// Platform::compareFolded does; the raw bytes break their ties, then the PID and the unique key, as compareByColumn().
inline void
sortByColumn(const std::vector<Domain::ProcessSnapshot>& rows, std::vector<std::size_t>& order, ProcessColumn column, bool ascending)
{
    if (!isTextColumn(column))
    {
        std::ranges::sort(order,
                          [&rows, column, ascending](std::size_t a, std::size_t b)
                          { return compareByColumn(rows[a], rows[b], column, ascending); });
        return;
    }
    // A folded key per sorted row; for User the folded name, a NUL (below every character), then the folded domain.
    std::vector<std::string> keys(rows.size());
    for (const std::size_t i : order)
    {
        const Domain::ProcessSnapshot& row = rows[i];
        std::string& key = keys[i];
        if (column == ProcessColumn::Name)
        {
            appendSortKey(key, row.name);
        }
        else if (column == ProcessColumn::Command)
        {
            appendSortKey(key, row.command);
        }
        else
        {
            appendSortKey(key, row.user);
            key += '\0';
            appendSortKey(key, row.userDomain);
        }
    }
    const auto rawOrder = [&rows, column](std::size_t a, std::size_t b) -> std::strong_ordering
    {
        const Domain::ProcessSnapshot& x = rows[a];
        const Domain::ProcessSnapshot& y = rows[b];
        if (column == ProcessColumn::Name)
        {
            return x.name <=> y.name;
        }
        if (column == ProcessColumn::Command)
        {
            return x.command <=> y.command;
        }
        const std::strong_ordering byUser = x.user <=> y.user;
        return byUser != 0 ? byUser : x.userDomain <=> y.userDomain;
    };
    std::ranges::sort(order,
                      [&rows, &keys, &rawOrder, ascending](std::size_t a, std::size_t b)
                      {
                          std::strong_ordering byKey = keys[a] <=> keys[b];
                          if (byKey == 0)
                          {
                              byKey = rawOrder(a, b);
                          }
                          if (byKey != 0)
                          {
                              return ascending ? byKey < 0 : byKey > 0;
                          }
                          if (rows[a].pid != rows[b].pid)
                          {
                              return ascending ? (rows[a].pid < rows[b].pid) : (rows[b].pid < rows[a].pid);
                          }
                          return ascending ? (rows[a].uniqueKey < rows[b].uniqueKey) : (rows[b].uniqueKey < rows[a].uniqueKey);
                      });
}

} // namespace App::ProcessSortUtils
