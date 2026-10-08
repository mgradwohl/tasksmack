#pragma once

// Process Details' collapsible Environment section (#179): the selected process's environment
// variables in a NAME / VALUE table, sorted by name, with a filter box once there are more than a
// screenful, and secret-looking values masked until their row's reveal button is pressed.
//
// The view never creates a reader. The panel owns the IProcessEnvironmentReader (the composition
// root's Platform::makeProcessEnvironmentReader() result) and passes it to update(), which the panel
// calls from its per-frame update path, never from render(). update() reads only while the section
// was drawn open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_ENVIRONMENT_REFRESH_MS; render() only draws what the last read returned.
//
// Everything but render() is defined here, free of ImGui, so the read cadence, the statuses and the
// reveal state are tested against a mock reader without an ImGui context
// (tests/App/test_ProcessEnvironmentView.cpp); render() is run headless in
// tests/App/test_ProcessEnvironmentViewRender.cpp.
//
// Environment values are never logged (#179).

#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "ProcessEnvironmentMasking.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// What a masked value is drawn as: eight bullets (U+2022), whatever the value's length.
inline constexpr std::string_view MASKED_ENVIRONMENT_VALUE = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
                                                             "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";

/// The filter box appears once the environment has more variables than this.
inline constexpr std::size_t ENVIRONMENT_FILTER_MIN_ROWS = 20;

/// Rows the table shows before it scrolls inside the section, so a long environment does not push the
/// Overview's charts out of the pane.
inline constexpr std::size_t ENVIRONMENT_TABLE_MAX_VISIBLE_ROWS = 12;

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view environmentStatusText(Platform::EnvironmentReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::EnvironmentReadStatus::Ok:
        return {};
    case Platform::EnvironmentReadStatus::PermissionDenied:
        return "Not readable (permission denied)";
    case Platform::EnvironmentReadStatus::ProcessExited:
        return "Process exited";
    case Platform::EnvironmentReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::EnvironmentReadStatus::IdentityUnknown:
        return "Not available yet"; // retried at the next refresh, once the start time is known
    case Platform::EnvironmentReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// Whether a cell's text needs a tooltip to be read whole: it was cut to @p shownBytes of its
/// @p totalBytes, or its drawn width @p textWidth exceeds the @p availableWidth left in the cell
/// (after the cell padding and a reveal button), where the column clips it.
[[nodiscard]] constexpr bool
environmentCellNeedsTooltip(std::size_t shownBytes, std::size_t totalBytes, float textWidth, float availableWidth) noexcept
{
    return shownBytes < totalBytes || textWidth > availableWidth;
}

/// Whether @p haystack contains @p needle, ignoring ASCII case. An empty needle matches everything.
[[nodiscard]] inline bool containsIgnoringCase(std::string_view haystack, std::string_view needle) noexcept
{
    const auto lower = [](char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (needle.empty())
    {
        return true;
    }
    return !std::ranges::search(haystack, needle, [&lower](char a, char b) { return lower(a) == lower(b); }).empty();
}

} // namespace Detail

/// The Environment section for the process Process Details shows.
class ProcessEnvironmentView
{
  public:
    /// One variable as the table shows it.
    struct Row
    {
        std::string name;
        std::string value;
        bool secret = false; ///< Detail::isSecretEnvironmentName(name): drawn masked until revealed
        /// How many earlier entries of the read had the same name. execve() allows duplicate names and
        /// /proc/[pid]/environ keeps every entry, so (name, occurrence) -- not the name alone -- is the
        /// row's identity for its reveal state, and stays the same across re-reads of the same environment.
        std::size_t occurrence = 0;
    };

    /// A row's identity for its reveal state (Row::occurrence).
    using RowKey = std::pair<std::string, std::size_t>;

    /// Draws the section: a collapsing "Environment" header and, while it is open, the last read's
    /// table or its status line. Draws nothing at all when @p hasEnvironment is false (the platform
    /// cannot read environments: Windows, synthetic runs).
    void render(bool hasEnvironment);

    /// The panel's per-frame update: reads @p target's environment through @p reader when the section
    /// was drawn open on the last frame and a read is due -- none yet since it opened or the selection
    /// changed, or PROCESS_ENVIRONMENT_REFRESH_MS since the last. @p reader may be null (nothing is read).
    /// @return Whether it read.
    bool update(Platform::IProcessEnvironmentReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        m_SecondsSinceRead += deltaSeconds;
        const bool shownOpen = std::exchange(m_DrawnOpen, false);
        // Kept counting while the section is closed, so one reopened after the interval reads at once.
        if (!shownOpen || reader == nullptr || !reader->hasEnvironment())
        {
            return false;
        }
        if (m_HasRead && (m_SecondsSinceRead * 1000.0F) < static_cast<float>(Domain::Sampling::PROCESS_ENVIRONMENT_REFRESH_MS))
        {
            return false;
        }
        applyResult(reader->readEnvironment(target));
        return true;
    }

    /// A different process was selected: drop its variables, the values revealed for it and the
    /// filter, so nothing of one process is shown for the next, and read afresh when next shown.
    void onSelectionChanged() noexcept
    {
        m_HasRead = false;
        m_Status = Platform::EnvironmentReadStatus::Ok;
        m_Rows.clear();
        m_Revealed.clear();
        m_Filter.clear();
        m_FilterKey.clear();
        m_FilteredRows.clear();
        m_FilterDirty = true;
        m_SecondsSinceRead = 0.0F;
    }

    /// Takes in a read: its status and, for Ok, its variables sorted by name with their mask flags.
    /// Values revealed for this process stay revealed across re-reads.
    void applyResult(Platform::EnvironmentReadResult result)
    {
        m_HasRead = true;
        m_SecondsSinceRead = 0.0F;
        m_Status = result.status;
        m_Rows.clear();
        m_Rows.reserve(result.variables.size());
        for (Platform::EnvironmentVariable& variable : result.variables)
        {
            const bool secret = Detail::isSecretEnvironmentName(variable.name);
            m_Rows.push_back({.name = std::move(variable.name), .value = std::move(variable.value), .secret = secret, .occurrence = 0});
        }
        // Stable: duplicates of a name keep their read order, which the occurrence numbers follow.
        std::ranges::stable_sort(m_Rows, {}, &Row::name);
        for (std::size_t i = 1; i < m_Rows.size(); ++i)
        {
            if (m_Rows[i].name == m_Rows[i - 1].name)
            {
                m_Rows[i].occurrence = m_Rows[i - 1].occurrence + 1;
            }
        }
        m_FilterDirty = true;
    }

    /// Records that render() drew the section open this frame (render() calls it; tests may too).
    void markDrawnOpen() noexcept
    {
        m_DrawnOpen = true;
    }

    /// Shows @p row's value if it is masked, or masks it again if it was revealed. Only that row:
    /// another entry with the same name stays as it was.
    void toggleReveal(const Row& row)
    {
        RowKey key{row.name, row.occurrence};
        const auto it = std::ranges::lower_bound(m_Revealed, key);
        if (it != m_Revealed.end() && *it == key)
        {
            m_Revealed.erase(it);
        }
        else
        {
            m_Revealed.insert(it, std::move(key));
        }
        m_FilterDirty = true; // a revealed value becomes searchable
    }

    /// Whether @p row's value has been revealed since the selection changed.
    [[nodiscard]] bool isRevealed(const Row& row) const
    {
        return std::ranges::binary_search(m_Revealed, RowKey{row.name, row.occurrence});
    }

    /// Whether @p row's value is drawn as Detail::MASKED_ENVIRONMENT_VALUE now.
    [[nodiscard]] bool isMasked(const Row& row) const
    {
        return row.secret && !isRevealed(row);
    }

    /// Whether a read has been taken in since the selection changed (false: "Reading...").
    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    /// The last read's status (Ok before the first).
    [[nodiscard]] Platform::EnvironmentReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// The last read's variables, sorted by name.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

    /// Sets the filter text, as typing in the filter box does.
    void setFilter(std::string filter)
    {
        m_Filter = std::move(filter);
    }

    /// Indices into rows() that the filter keeps: the name contains the filter text (ignoring case),
    /// or the value does and is not masked -- a masked value is never searched, so typing guesses into
    /// the filter cannot probe a secret. Cached until the rows, the filter or a reveal change.
    [[nodiscard]] std::span<const std::size_t> filteredRows()
    {
        if (m_FilterDirty || m_FilterKey != m_Filter)
        {
            m_FilteredRows.clear();
            for (std::size_t i = 0; i < m_Rows.size(); ++i)
            {
                const Row& row = m_Rows[i];
                if (Detail::containsIgnoringCase(row.name, m_Filter) ||
                    (!isMasked(row) && Detail::containsIgnoringCase(row.value, m_Filter)))
                {
                    m_FilteredRows.push_back(i);
                }
            }
            m_FilterKey = m_Filter;
            m_FilterDirty = false;
        }
        return m_FilteredRows;
    }

  private:
    void renderTable();

    bool m_DrawnOpen = false; // render() drew the section open since the last update()
    bool m_HasRead = false;
    float m_SecondsSinceRead = 0.0F;
    Platform::EnvironmentReadStatus m_Status = Platform::EnvironmentReadStatus::Ok;
    std::vector<Row> m_Rows;
    std::vector<RowKey> m_Revealed; // sorted keys of the rows whose values are shown
    std::string m_Filter;
    std::string m_FilterKey; // m_Filter as m_FilteredRows was built for
    std::vector<std::size_t> m_FilteredRows;
    bool m_FilterDirty = true;
};

} // namespace App
