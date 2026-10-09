#pragma once

// Process Details' collapsible Open files section (#183), beside Modules (ProcessModulesView.h) and in
// its shape: the selected process's open descriptors (Linux) or File handles (Windows) in an
// FD / TYPE / MODE / PATH table (HANDLE / TYPE / PATH on Windows), sortable by any column (by
// descriptor by default), filterable by path or type, with the count in the section's header.
//
// The panel owns the IProcessOpenFilesReader (Platform::makeProcessOpenFilesReader()) and passes it to
// update() from its per-frame update path. Reads run lazily on a worker (Detail::LazyBackgroundRead):
// only while the section was drawn open, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_OPEN_FILES_REFRESH_MS. render() only draws the last read, formatted once.
//
// Everything but render() is defined here, free of ImGui; render() is run headless in
// tests/App/test_ProcessOpenFilesViewRender.cpp.

#include "Domain/ProcessOpenFiles.h"
#include "Domain/SamplingConfig.h"
#include "LazyBackgroundRead.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/ThreadName.h"
#include "ProcessEnvironmentView.h"

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
inline constexpr std::size_t OPEN_FILES_TABLE_MAX_VISIBLE_ROWS = 12;

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view openFilesStatusText(Platform::OpenFilesReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::OpenFilesReadStatus::Ok:
        return {};
    case Platform::OpenFilesReadStatus::PermissionDenied:
        return "Access denied: a protected or elevated process, or another user's";
    case Platform::OpenFilesReadStatus::ProcessExited:
        return "Process exited";
    case Platform::OpenFilesReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::OpenFilesReadStatus::IdentityUnknown:
        return "Not available for this process";
    case Platform::OpenFilesReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// The table's columns, also its sort keys (the ImGui column user IDs).
enum class OpenFilesColumn : std::uint8_t
{
    Descriptor,
    Type,
    Mode,
    Path,
};

} // namespace Detail

/// The Open files section for the process Process Details shows.
class ProcessOpenFilesView
{
  public:
    /// One open file as the table shows it: the raw row (for sorting) and its text, formatted once per read.
    struct Row
    {
        Platform::OpenFile file;
        std::string descriptor; ///< "3", "0x1A4"
        std::string type;       ///< "File", "Socket", ...
        std::string mode;       ///< "rw", "w append", or empty (Windows)
        std::string path;       ///< the path, "socket:[1234]", "/tmp/x (deleted)", or "(name not read)"
    };

    /// Draws the section: a collapsing "Open files (N)" header and, while it is open, the filter box and
    /// table, or the last read's status line. Draws nothing when @p hasOpenFiles is false.
    void render(bool hasOpenFiles);

    /// The panel's per-frame update; as ProcessModulesView::update(). Never blocks. The caller keeps
    /// @p reader alive until a read in flight finishes (the destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessOpenFilesReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        takeFinishedRead(target, false);
        if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasOpenFiles())
        {
            return false;
        }
        if (auto failed = m_Read.start(target, [reader](const Platform::ProcessTarget& t) { return reader->readOpenFiles(t); }))
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

    /// A different process was selected: drop its files and read afresh when next shown. The sort and
    /// the filter are viewing preferences and stay.
    void onSelectionChanged() noexcept
    {
        m_Read.reset();
        m_HasRead = false;
        m_Status = Platform::OpenFilesReadStatus::Ok;
        m_Detail.clear();
        m_Truncated = false;
        m_NamesIncomplete = false;
        m_Rows.clear();
        m_FilteredRows.clear();
        m_FilterDirty = true;
    }

    /// Takes in a read: its status and notes and, for Ok, its files formatted and sorted.
    void applyResult(Platform::OpenFilesReadResult result)
    {
        m_HasRead = true;
        m_Status = result.status;
        m_Detail = std::move(result.detail);
        m_Truncated = result.truncated;
        m_NamesIncomplete = result.namesIncomplete;
        m_HexDescriptors = result.hexDescriptors;
        m_Rows.clear();
        m_Rows.reserve(result.files.size());
        for (Platform::OpenFile& file : result.files)
        {
            std::string path = file.path;
            if (file.deleted)
            {
                path += " (deleted)";
            }
            else if (path.empty() && file.kind == Platform::OpenFileKind::Other)
            {
                path = "(name not read)";
            }
            std::string descriptor = Domain::OpenFiles::formatDescriptor(file.descriptor, result.hexDescriptors);
            std::string type(Domain::OpenFiles::kindLabel(file.kind));
            std::string mode = Domain::OpenFiles::formatMode(file.flags);
            m_Rows.push_back({.file = std::move(file),
                              .descriptor = std::move(descriptor),
                              .type = std::move(type),
                              .mode = std::move(mode),
                              .path = std::move(path)});
        }
        sortRows();
    }

    void markDrawnOpen() noexcept
    {
        m_Read.markDrawnOpen();
    }

    /// Sorts by @p column, as clicking its header does. Ties fall back to the descriptor.
    void setSort(Detail::OpenFilesColumn column, bool ascending)
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

    /// Indices into rows() whose path or type contains the filter (ignoring case). Cached until the rows
    /// or the filter change.
    [[nodiscard]] std::span<const std::size_t> filteredRows()
    {
        if (m_FilterDirty || m_FilterKey != m_Filter)
        {
            m_FilteredRows.clear();
            for (std::size_t i = 0; i < m_Rows.size(); ++i)
            {
                if (Detail::containsIgnoringCase(m_Rows[i].path, m_Filter) || Detail::containsIgnoringCase(m_Rows[i].type, m_Filter))
                {
                    m_FilteredRows.push_back(i);
                }
            }
            m_FilterKey = m_Filter;
            m_FilterDirty = false;
        }
        return m_FilteredRows;
    }

    /// Whether any row has open flags: the Mode column is left out when none does (Windows).
    [[nodiscard]] bool hasModes() const noexcept
    {
        return std::ranges::any_of(m_Rows, [](const Row& row) { return row.file.flags.has_value(); });
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::OpenFilesReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// The last read's files, in the current sort order.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    void takeFinishedRead(const Platform::ProcessTarget& target, bool wait)
    {
        if (auto result = m_Read.takeFinished(target, wait))
        {
            applyResult(std::move(*result));
        }
    }

    [[nodiscard]] static Platform::OpenFilesReadResult failedRead(std::string detail)
    {
        Platform::OpenFilesReadResult result;
        result.status = Platform::OpenFilesReadStatus::Failed;
        result.detail = std::move(detail);
        return result;
    }

    [[nodiscard]] static std::strong_ordering compareOn(Detail::OpenFilesColumn column, const Row& a, const Row& b) noexcept
    {
        switch (column)
        {
        case Detail::OpenFilesColumn::Type:
            return a.file.kind <=> b.file.kind;
        case Detail::OpenFilesColumn::Mode:
            return a.mode <=> b.mode;
        case Detail::OpenFilesColumn::Path:
            return a.path <=> b.path;
        case Detail::OpenFilesColumn::Descriptor:
            break;
        }
        return a.file.descriptor <=> b.file.descriptor;
    }

    void sortRows()
    {
        const Detail::OpenFilesColumn column = m_SortColumn;
        const bool ascending = m_SortAscending;
        std::ranges::stable_sort(m_Rows,
                                 [column, ascending](const Row& a, const Row& b)
                                 {
                                     if (const auto primary = compareOn(column, a, b); primary != 0)
                                     {
                                         return ascending ? primary < 0 : primary > 0;
                                     }
                                     return a.file.descriptor < b.file.descriptor;
                                 });
        m_FilterDirty = true;
    }

    void renderTable();

    bool m_HasRead = false;
    Platform::OpenFilesReadStatus m_Status = Platform::OpenFilesReadStatus::Ok;
    std::string m_Detail;
    bool m_Truncated = false;       // the read listed only the first MAX_OPEN_FILES
    bool m_NamesIncomplete = false; // some handles were not named (Windows)
    bool m_HexDescriptors = false;  // Windows handles: the column is HANDLE, not FD
    std::vector<Row> m_Rows;
    Detail::OpenFilesColumn m_SortColumn = Detail::OpenFilesColumn::Descriptor;
    bool m_SortAscending = true;
    std::string m_Filter;
    std::string m_FilterKey;
    std::vector<std::size_t> m_FilteredRows;
    bool m_FilterDirty = true;
    std::string m_CountLabel;            // "Open files (42)"
    std::size_t m_LabelCount = SIZE_MAX; // the count m_CountLabel shows

    // The read in flight and its cadence. Destroying the view waits for at most one read.
    Detail::LazyBackgroundRead<Platform::OpenFilesReadResult> m_Read{
        Domain::Sampling::PROCESS_OPEN_FILES_REFRESH_MS, Platform::OPEN_FILES_READ_THREAD_NAME, &failedRead};
};

} // namespace App
