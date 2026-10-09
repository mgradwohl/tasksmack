#pragma once

// Process Details' collapsible Modules section (#802): the code modules the selected process has
// loaded (its executable and every DLL or shared object) in a NAME / VERSION / BASE / SIZE / PATH
// table, sortable by any column (by name by default), filterable by name or path, with the count in
// the section's header.
//
// The read cadence is the Connections section's (ProcessConnectionsView.h): the panel owns the
// IProcessModulesReader (the composition root's Platform::makeProcessModulesReader() result) and
// passes it to update() from its per-frame update path, never from render(). update() starts a read
// only while the section was drawn open on the last frame, once when it opens or the selection
// changes and then every Domain::Sampling::PROCESS_MODULES_REFRESH_MS, on a worker thread (one at a
// time), because a read crosses into the process and reads each new module's version from disk.
// render() only draws what the last read returned, formatted once per read (Domain/ProcessModules.h).
//
// Everything but render() is defined here, free of ImGui, so the cadence, the statuses, the sort and
// the filter are tested against a mock reader without an ImGui context; render() is run headless in
// tests/App/test_ProcessModulesViewRender.cpp.

#include "Domain/ProcessModules.h"
#include "Domain/SamplingConfig.h"
#include "LazyBackgroundRead.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/ThreadName.h"
#include "ProcessEnvironmentView.h"
#include "UI/Format.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// Rows the table shows before it scrolls inside the section.
inline constexpr std::size_t MODULES_TABLE_MAX_VISIBLE_ROWS = 12;

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view modulesStatusText(Platform::ModulesReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::ModulesReadStatus::Ok:
        return {};
    case Platform::ModulesReadStatus::PermissionDenied:
        return "Access denied: a protected or elevated process, or another user's";
    case Platform::ModulesReadStatus::ProcessExited:
        return "Process exited";
    case Platform::ModulesReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::ModulesReadStatus::IdentityUnknown:
        return "Not available for this process"; // retried at the next refresh, in case the start time arrives
    case Platform::ModulesReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// The table's columns, also its sort keys (the ImGui column user IDs).
enum class ModulesColumn : std::uint8_t
{
    Name,
    Version,
    Base,
    Size,
    Path,
};

} // namespace Detail

/// The Modules section for the process Process Details shows.
class ProcessModulesView
{
  public:
    /// One module as the table shows it: the raw row (for sorting) and its text, formatted once per read.
    struct Row
    {
        Platform::ProcessModule module;
        std::string name;    ///< "ntdll.dll", "libc.so.6 (deleted)"
        std::string version; ///< "10.0.26100.4202", or empty
        std::string base;    ///< "0x7FF8A1B20000"
        std::string size;    ///< "2.4 MB"
    };

    /// Draws the section: a collapsing "Modules (N)" header and, while it is open, the filter box and
    /// table, or the last read's status line. Draws nothing when @p hasModules is false (synthetic runs).
    void render(bool hasModules);

    /// The panel's per-frame update; as ProcessConnectionsView::update(). Never blocks. The caller must
    /// keep @p reader alive until a read in flight finishes (the destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessModulesReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        takeFinishedRead(target, false);
        if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasModules())
        {
            return false;
        }
        if (auto failed = m_Read.start(target, [reader](const Platform::ProcessTarget& t) { return reader->readModules(t); }))
        {
            applyResult(*failed);
        }
        return true;
    }

    /// Waits for the read in flight, if any, and takes it in as update() would (tests only).
    void finishPendingRead(const Platform::ProcessTarget& target)
    {
        takeFinishedRead(target, true);
    }

    /// A different process was selected: drop its modules and read afresh when next shown. A read in
    /// flight is dropped when it arrives. The sort and the filter are viewing preferences and stay.
    void onSelectionChanged() noexcept
    {
        m_Read.reset();
        m_HasRead = false;
        m_Status = Platform::ModulesReadStatus::Ok;
        m_Detail.clear();
        m_Rows.clear();
        m_FilteredRows.clear();
        m_FilterDirty = true;
    }

    /// Takes in a read: its status and, for Ok, its modules formatted and sorted by the current order.
    void applyResult(const Platform::ModulesReadResult& result)
    {
        m_HasRead = true;
        m_Status = result.status;
        m_Detail = result.detail;
        m_Rows.clear();
        m_Rows.reserve(result.modules.size());
        for (const Platform::ProcessModule& module : result.modules)
        {
            std::string name(Domain::Modules::fileName(module.path));
            if (module.deleted)
            {
                name += " (deleted)";
            }
            m_Rows.push_back({.module = module,
                              .name = std::move(name),
                              .version = Domain::Modules::formatVersion(module.version),
                              .base = Domain::Modules::formatAddress(module.baseAddress),
                              .size = UI::Format::formatBytes(static_cast<double>(module.sizeBytes))});
        }
        sortRows();
    }

    void markDrawnOpen() noexcept
    {
        m_Read.markDrawnOpen();
    }

    /// Sorts by @p column, as clicking its header does. Ties fall back to the base address.
    void setSort(Detail::ModulesColumn column, bool ascending)
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

    /// Indices into rows() whose name or path contains the filter (ignoring case). Cached until the
    /// rows or the filter change.
    [[nodiscard]] std::span<const std::size_t> filteredRows()
    {
        if (m_FilterDirty || m_FilterKey != m_Filter)
        {
            m_FilteredRows.clear();
            for (std::size_t i = 0; i < m_Rows.size(); ++i)
            {
                if (Detail::containsIgnoringCase(m_Rows[i].name, m_Filter) || Detail::containsIgnoringCase(m_Rows[i].module.path, m_Filter))
                {
                    m_FilteredRows.push_back(i);
                }
            }
            m_FilterKey = m_Filter;
            m_FilterDirty = false;
        }
        return m_FilteredRows;
    }

    /// Whether any module has a version: the Version column is left out when none does (Linux).
    [[nodiscard]] bool hasVersions() const noexcept
    {
        return std::ranges::any_of(m_Rows, [](const Row& row) { return row.module.version.has_value(); });
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::ModulesReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// The last read's modules, in the current sort order.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    /// Takes in the read in flight once it has finished (or, with @p wait, after waiting for it), if it
    /// is for this selection and @p target; a stale one is dropped.
    void takeFinishedRead(const Platform::ProcessTarget& target, bool wait)
    {
        if (auto result = m_Read.takeFinished(target, wait))
        {
            applyResult(*result);
        }
    }

    [[nodiscard]] static Platform::ModulesReadResult failedRead(std::string detail)
    {
        return {.status = Platform::ModulesReadStatus::Failed, .modules = {}, .detail = std::move(detail)};
    }

    [[nodiscard]] static std::strong_ordering compareOn(Detail::ModulesColumn column, const Row& a, const Row& b) noexcept
    {
        switch (column)
        {
        case Detail::ModulesColumn::Name:
            return compareIgnoringCase(a.name, b.name);
        case Detail::ModulesColumn::Version:
            return a.module.version <=> b.module.version; // none sorts first
        case Detail::ModulesColumn::Size:
            return a.module.sizeBytes <=> b.module.sizeBytes;
        case Detail::ModulesColumn::Path:
            return compareIgnoringCase(a.module.path, b.module.path);
        case Detail::ModulesColumn::Base:
            break;
        }
        return a.module.baseAddress <=> b.module.baseAddress;
    }

    [[nodiscard]] static std::strong_ordering compareIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        const auto lower = [](char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return std::lexicographical_compare_three_way(
            a.begin(), a.end(), b.begin(), b.end(), [&lower](char x, char y) { return lower(x) <=> lower(y); });
    }

    void sortRows()
    {
        const Detail::ModulesColumn column = m_SortColumn;
        const bool ascending = m_SortAscending;
        std::ranges::stable_sort(m_Rows,
                                 [column, ascending](const Row& a, const Row& b)
                                 {
                                     if (const auto primary = compareOn(column, a, b); primary != 0)
                                     {
                                         return ascending ? primary < 0 : primary > 0;
                                     }
                                     return compareOn(Detail::ModulesColumn::Base, a, b) < 0;
                                 });
        m_FilterDirty = true;
    }

    void renderTable();

    bool m_HasRead = false; // a read was taken in since the selection changed
    Platform::ModulesReadStatus m_Status = Platform::ModulesReadStatus::Ok;
    std::string m_Detail;
    std::vector<Row> m_Rows;
    Detail::ModulesColumn m_SortColumn = Detail::ModulesColumn::Name;
    bool m_SortAscending = true;
    std::string m_Filter;
    std::string m_FilterKey; // the filter m_FilteredRows was built for
    std::vector<std::size_t> m_FilteredRows;
    bool m_FilterDirty = true;
    std::string m_CountLabel;            // the header with the count: "Modules (87)"
    std::size_t m_LabelCount = SIZE_MAX; // the count m_CountLabel shows

    // The read in flight and its cadence. Destroying the view waits for at most one read.
    Detail::LazyBackgroundRead<Platform::ModulesReadResult> m_Read{
        Domain::Sampling::PROCESS_MODULES_REFRESH_MS, Platform::MODULES_READ_THREAD_NAME, &failedRead};
};

} // namespace App
