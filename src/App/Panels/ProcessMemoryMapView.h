#pragma once

// Process Details' collapsible Memory map section (#3089): the selected process's address space, totals by
// kind (Image, Mapped file, Private, ...) above an ADDRESS / SIZE / KIND / PROTECTION / PATH table of every
// region, sortable by any column (by address by default) and filterable by kind, protection or path, with
// the region count in the section's header. A RESIDENT column, and the kind table's resident, PSS, private,
// shared and swap totals, show only when the platform measured them (Linux's smaps; Windows' QueryWorkingSetEx
// gives resident, private and shared, #4031, and no PSS or swap),
// and the RESERVED total only when some region is reserved (Windows). Windows writes a protection in its
// own tools' words ("Execute/Read"), Linux as /proc/[pid]/maps does ("r-xp").
//
// The read cadence is the Modules section's (ProcessModulesView.h): the panel owns the
// IProcessMemoryMapReader (the composition root's App::makeProcessMemoryMapReader() result) and passes it
// to update() from its per-frame update path, never from render(). update() starts a read only while the
// section was drawn open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_MEMORY_MAP_REFRESH_MS, on a worker thread (one at a time), because a read
// walks the whole address space. That worker also formats, sums up and sorts the read (prepare(), #3801): the UI
// thread only moves the prepared rows in, and render() only draws them (Domain/ProcessMemoryMap.h).
//
// Everything but render() is defined here, free of ImGui, so the cadence, the statuses, the totals, the
// sort and the filter are tested against a mock reader without an ImGui context; render() is run headless
// in tests/App/test_ProcessMemoryMapViewRender.cpp.

#include "Domain/ProcessMemoryMap.h"
#include "Domain/ProcessModules.h"
#include "Domain/SamplingConfig.h"
#include "Domain/Utf8Sanitize.h"
#include "LazyBackgroundRead.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/ThreadName.h"
#include "Platform/Utf8CaseFold.h"
#include "SectionCaches.h"
#include "UI/Format.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// Rows the regions table shows before it scrolls inside the section.
inline constexpr std::size_t MEMORY_MAP_TABLE_MAX_VISIBLE_ROWS = 12;

/// Whether the PROTECTION column uses Windows' words ("Execute/Read") rather than maps' letters ("r-xp").
#ifdef _WIN32
inline constexpr bool MEMORY_MAP_PROTECTION_WORDS = true;
#else
inline constexpr bool MEMORY_MAP_PROTECTION_WORDS = false;
#endif

/// The line shown in place of the tables for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view memoryMapStatusText(Platform::MemoryMapReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::MemoryMapReadStatus::Ok:
        return {};
    case Platform::MemoryMapReadStatus::PermissionDenied:
        return "Access denied: a protected or elevated process, or another user's";
    case Platform::MemoryMapReadStatus::ProcessExited:
        return "Process exited";
    case Platform::MemoryMapReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::MemoryMapReadStatus::IdentityUnknown:
        return "Not available for this process"; // retried at the next refresh, in case the start time arrives
    case Platform::MemoryMapReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// A mapping's PROTECTION cell: Windows' words when @p words, else maps' letters.
[[nodiscard]] inline std::string memoryMapProtectionCell(const Platform::MemoryMapping& mapping, bool words)
{
    return words ? Domain::MemoryMap::protectionWords(mapping) : Domain::MemoryMap::protectionText(mapping);
}

/// An optional size as a cell: formatted, or empty when the platform didn't measure it.
[[nodiscard]] inline std::string memoryMapBytesCell(const std::optional<std::uint64_t>& bytes)
{
    return bytes.has_value() ? UI::Format::formatBytes(static_cast<double>(*bytes)) : std::string{};
}

/// The regions table's columns, also its sort keys (the ImGui column user IDs).
enum class MemoryMapColumn : std::uint8_t
{
    Address,
    Size,
    Kind,
    Protection,
    Resident,
    Path,
};

} // namespace Detail

/// The Memory map section for the process Process Details shows.
class ProcessMemoryMapView
{
  public:
    /// One region as the table shows it: the raw mapping (for sorting) and its text, formatted once per read.
    struct Row
    {
        Platform::MemoryMapping mapping;
        std::string address;    ///< "0x7FF8A1B20000"
        std::string size;       ///< "2.4 MB"
        std::string kind;       ///< "Image", "Private"
        std::string protection; ///< "Execute/Read" (Windows), "r-xp" (Linux)
        std::string resident;   ///< "1.2 MB", or empty where not measured
        std::string path;       ///< the file, with " (deleted)" after a deleted one; the kernel's name; or empty
    };

    /// One line of the totals table: a kind's totals, or all of them, formatted once per read.
    struct TotalsRow
    {
        std::string label; ///< "Image", "Mapped file", ... or "Total"
        Domain::MemoryMap::Totals totals;
        std::string regions;
        std::string size;
        std::string reserved;
        std::string resident;
        std::string pss;
        std::string privateBytes;
        std::string shared;
        std::string swap;
    };

    /// A read made ready to draw on its worker (#3801): its status and, for Ok, its regions formatted and sorted in
    /// the order the view had when the read started, and its totals. Up to the reader's cap of regions, six
    /// strings each, a summarize() and a stable sort: off the UI thread, which only moves it in.
    struct Prepared
    {
        Platform::MemoryMapReadStatus status = Platform::MemoryMapReadStatus::Ok;
        std::string detail;
        bool truncated = false;
        bool hasResident = false;
        std::vector<Row> rows;
        std::vector<TotalsRow> totals;
        Detail::MemoryMapColumn sortColumn = Detail::MemoryMapColumn::Address; ///< The order rows are in
        bool sortAscending = true;
    };

    /// Makes @p result ready to draw (#3801), with the protection written in words (@p protectionWords) or in
    /// letters, and the rows sorted by @p column (@p ascending). Pure: the read's worker runs it.
    [[nodiscard]] static Prepared
    prepare(const Platform::MemoryMapReadResult& result, bool protectionWords, Detail::MemoryMapColumn column, bool ascending)
    {
        Prepared prepared{
            .status = result.status,
            .detail = result.detail,
            .truncated = result.truncated,
            .hasResident = false,
            .rows = {},
            .totals = {},
            .sortColumn = column,
            .sortAscending = ascending,
        };
        if (result.status != Platform::MemoryMapReadStatus::Ok)
        {
            return prepared;
        }
        prepared.rows.reserve(result.mappings.size());
        for (const Platform::MemoryMapping& mapping : result.mappings)
        {
            // The shown path is well-formed UTF-8 (#4301); mapping keeps the raw bytes.
            std::string path = Domain::sanitizedUtf8(mapping.path);
            if (mapping.deleted)
            {
                path += " (deleted)";
            }
            prepared.rows.push_back({
                .mapping = mapping,
                .address = Domain::Modules::formatAddress(mapping.start),
                .size = UI::Format::formatBytes(static_cast<double>(mapping.sizeBytes())),
                .kind = std::string(Domain::MemoryMap::kindLabel(mapping.kind)),
                .protection = Detail::memoryMapProtectionCell(mapping, protectionWords),
                .resident = Detail::memoryMapBytesCell(mapping.residentBytes),
                .path = std::move(path),
            });
        }
        prepared.hasResident =
            std::ranges::any_of(result.mappings, [](const Platform::MemoryMapping& m) { return m.residentBytes.has_value(); });
        const Domain::MemoryMap::Summary summary = Domain::MemoryMap::summarize(result.mappings);
        prepared.totals.reserve(summary.byKind.size() + 1);
        for (const Domain::MemoryMap::KindTotals& kind : summary.byKind)
        {
            prepared.totals.push_back(totalsRow(std::string(Domain::MemoryMap::kindLabel(kind.kind)), kind.totals));
        }
        prepared.totals.push_back(totalsRow("Total", summary.all));
        sortRowsBy(prepared.rows, column, ascending);
        return prepared;
    }

    /// Draws the section: a collapsing "Memory map (N)" header and, while it is open, the totals, the filter
    /// box and the regions table, or the last read's status line. Draws nothing when @p hasMemoryMap is false
    /// (synthetic runs).
    void render(bool hasMemoryMap);

    /// The panel's per-frame update; as ProcessModulesView::update(). Never blocks. The caller must keep
    /// @p reader alive until a read in flight finishes (the destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessMemoryMapReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        takeFinishedRead(target, false);
        return m_Read.startAuthorizedOrDue(
            reader,
            target,
            deltaSeconds,
            [](const Platform::IProcessMemoryMapReader& r) { return r.hasMemoryMap(); },
            [](Platform::IProcessMemoryMapReader& r, const Platform::ProcessTarget& t) { return r.readMemoryMap(t); },
            [column = m_SortColumn, ascending = m_SortAscending](const Platform::MemoryMapReadResult& result)
            { return prepare(result, Detail::MEMORY_MAP_PROTECTION_WORDS, column, ascending); },
            [this](Prepared&& failed) { applyPrepared(std::move(failed)); });
    }

    /// Whether the status line offers "Authenticate to read" (#2309): the read was refused for want of
    /// permission and the reader can read as an administrator.
    [[nodiscard]] bool offersAuthenticateToRead() const noexcept
    {
        return m_Read.offersAuthorizedRead(m_HasRead && m_Status == Platform::MemoryMapReadStatus::PermissionDenied);
    }

    /// Whether the last read was refused for want of permission, with no read in flight: where no helper can
    /// read it as an administrator, the status line offers "Restart as administrator..." (#3658).
    [[nodiscard]] bool readRefusedForPermission() const noexcept
    {
        return m_Read.refusedAndIdle(m_HasRead && m_Status == Platform::MemoryMapReadStatus::PermissionDenied);
    }

    /// "Authenticate to read" was pressed: the next update() reads as an administrator.
    void requestAuthorizedRead() noexcept
    {
        m_Read.requestAuthorized();
    }

    /// Waits for the read in flight, if any, and takes it in as update() would (tests only).
    void finishPendingRead(const Platform::ProcessTarget& target)
    {
        takeFinishedRead(target, true);
    }

    /// A different process was selected: drop its map and read afresh when next shown. A read in flight is
    /// dropped when it arrives. The sort and the filter are viewing preferences and stay.
    void onSelectionChanged() noexcept
    {
        m_Read.reset();
        m_HasRead = false;
        m_Status = Platform::MemoryMapReadStatus::Ok;
        m_Detail.clear();
        m_Truncated = false;
        m_Rows.clear();
        m_HasResident = false;
        m_Totals.clear();
        m_Filtered.reset();
    }

    /// Takes in a read: its status and, for Ok, its totals and its regions formatted and sorted by the
    /// current order. The PROTECTION column follows the platform (Detail::MEMORY_MAP_PROTECTION_WORDS). Tests; the
    /// view's own reads are prepared on their worker (prepare(), #3801).
    void applyResult(const Platform::MemoryMapReadResult& result)
    {
        applyResult(result, Detail::MEMORY_MAP_PROTECTION_WORDS);
    }

    /// applyResult(), with the protection written in words (@p protectionWords) or in letters (tests: both).
    void applyResult(const Platform::MemoryMapReadResult& result, bool protectionWords)
    {
        applyPrepared(prepare(result, protectionWords, m_SortColumn, m_SortAscending));
    }

    /// Takes in a prepared read by moving it in. Sorted again only when the order changed since the read started.
    void applyPrepared(Prepared prepared)
    {
        m_HasRead = true;
        m_Status = prepared.status;
        m_Detail = std::move(prepared.detail);
        m_Truncated = prepared.truncated;
        m_HasResident = prepared.hasResident;
        m_Rows = std::move(prepared.rows);
        m_Totals = std::move(prepared.totals);
        if (prepared.sortColumn != m_SortColumn || prepared.sortAscending != m_SortAscending)
        {
            sortRows();
        }
        m_Filtered.markDirty();
    }

    void markDrawnOpen() noexcept
    {
        m_Read.markDrawnOpen();
    }

    /// Sorts by @p column, as clicking its header does. Ties fall back to the address.
    void setSort(Detail::MemoryMapColumn column, bool ascending)
    {
        m_SortColumn = column;
        m_SortAscending = ascending;
        sortRows();
    }

    /// Sets the filter text, as typing in the filter box does.
    void setFilter(std::string filter)
    {
        m_Filter = std::move(filter);
    }

    /// Indices into rows() whose kind, protection or path contains the filter (ignoring case). Cached until
    /// the rows or the filter change.
    [[nodiscard]] std::span<const std::size_t> filteredRows()
    {
        return m_Filtered.get(m_Rows.size(),
                              m_Filter,
                              [this](std::size_t i)
                              {
                                  const Row& row = m_Rows[i];
                                  return Platform::containsFoldedCase(row.kind, m_Filter) ||
                                         Platform::containsFoldedCase(row.protection, m_Filter) ||
                                         Platform::containsFoldedCase(row.path, m_Filter);
                              });
    }

    /// Whether any region's resident size was measured: the RESIDENT column is left out when none was (Windows).
    [[nodiscard]] bool hasResident() const noexcept
    {
        return m_HasResident;
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::MemoryMapReadStatus status() const noexcept
    {
        return m_Status;
    }

    [[nodiscard]] bool truncated() const noexcept
    {
        return m_Truncated;
    }

    /// The last read's regions, in the current sort order.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

    /// The last read's totals: each kind present, largest first, then "Total".
    [[nodiscard]] std::span<const TotalsRow> totals() const noexcept
    {
        return m_Totals;
    }

  private:
    /// Takes in the read in flight once it has finished (or, with @p wait, after waiting for it), if it is
    /// for this selection and @p target; a stale one is dropped.
    void takeFinishedRead(const Platform::ProcessTarget& target, bool wait)
    {
        if (auto result = m_Read.takeFinished(target, wait))
        {
            applyPrepared(std::move(*result));
        }
    }

    [[nodiscard]] static Prepared failedRead(std::string detail)
    {
        return {
            .status = Platform::MemoryMapReadStatus::Failed,
            .detail = std::move(detail),
            .truncated = false,
            .hasResident = false,
            .rows = {},
            .totals = {},
            .sortColumn = Detail::MemoryMapColumn::Address,
            .sortAscending = true,
        };
    }

    [[nodiscard]] static TotalsRow totalsRow(std::string label, const Domain::MemoryMap::Totals& totals)
    {
        return {
            .label = std::move(label),
            .totals = totals,
            .regions = std::to_string(totals.mappings),
            .size = UI::Format::formatBytes(static_cast<double>(totals.sizeBytes)),
            .reserved = UI::Format::formatBytes(static_cast<double>(totals.reservedBytes)),
            .resident = Detail::memoryMapBytesCell(totals.residentBytes),
            .pss = Detail::memoryMapBytesCell(totals.pssBytes),
            .privateBytes = Detail::memoryMapBytesCell(totals.privateBytes),
            .shared = Detail::memoryMapBytesCell(totals.sharedBytes),
            .swap = Detail::memoryMapBytesCell(totals.swapBytes),
        };
    }

    [[nodiscard]] static std::strong_ordering compareOn(Detail::MemoryMapColumn column, const Row& a, const Row& b) noexcept
    {
        switch (column)
        {
        case Detail::MemoryMapColumn::Size:
            return a.mapping.sizeBytes() <=> b.mapping.sizeBytes();
        case Detail::MemoryMapColumn::Kind:
            return Platform::compareFolded(a.kind, b.kind);
        case Detail::MemoryMapColumn::Protection:
            return Platform::compareFolded(a.protection, b.protection);
        case Detail::MemoryMapColumn::Resident:
            return a.mapping.residentBytes <=> b.mapping.residentBytes; // none sorts first
        case Detail::MemoryMapColumn::Path:
            return Platform::compareFolded(a.path, b.path);
        case Detail::MemoryMapColumn::Address:
            break;
        }
        return a.mapping.start <=> b.mapping.start;
    }

    static void sortRowsBy(std::vector<Row>& rows, Detail::MemoryMapColumn column, bool ascending)
    {
        std::ranges::stable_sort(rows,
                                 [column, ascending](const Row& a, const Row& b)
                                 {
                                     if (const auto primary = compareOn(column, a, b); primary != 0)
                                     {
                                         return ascending ? primary < 0 : primary > 0;
                                     }
                                     return compareOn(Detail::MemoryMapColumn::Address, a, b) < 0;
                                 });
    }

    void sortRows()
    {
        sortRowsBy(m_Rows, m_SortColumn, m_SortAscending);
        m_Filtered.markDirty();
    }

    void renderTotals();
    void renderTable();

    bool m_HasRead = false; // a read was taken in since the selection changed
    Platform::MemoryMapReadStatus m_Status = Platform::MemoryMapReadStatus::Ok;
    std::string m_Detail;
    bool m_Truncated = false;   // the read kept only the first regions (past the reader's cap)
    bool m_HasResident = false; ///< Set once per read, so the table doesn't scan every region each frame (#3800).
    std::vector<Row> m_Rows;
    std::vector<TotalsRow> m_Totals;
    Detail::MemoryMapColumn m_SortColumn = Detail::MemoryMapColumn::Address;
    bool m_SortAscending = true;
    std::string m_Filter;
    Detail::FilteredRows m_Filtered; // the rows the filter lets through, cached (#3887)
    Detail::CountedHeader m_Header;  // the header with the count, formatted when it changes

    // The read in flight, its cadence and the "Authenticate to read" request (#2309). Destroying the view
    // waits for at most one read.
    Detail::LazyBackgroundRead<Prepared> m_Read{
        Domain::Sampling::PROCESS_MEMORY_MAP_REFRESH_MS, Platform::MEMORY_MAP_READ_THREAD_NAME, &failedRead};
};

} // namespace App
