#pragma once

// The two per-frame caches Process Details' list sections (Environment, Modules, Memory map, Open files, Threads)
// each kept a copy of (#3887): the rows the filter lets through, and the header label with the row count. Free of
// ImGui, so the views' tests exercise them as they did.

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App::Detail
{

/// Indices into a section's rows that its filter lets through, built again only when the rows changed (markDirty())
/// or the filter text did, not every frame.
class FilteredRows
{
  public:
    /// The rows changed (a read, a sort, a revealed value): build the list again at the next get().
    void markDirty() noexcept
    {
        m_Dirty = true;
    }

    /// A different process was selected: the list and the filter it was built for go.
    void reset() noexcept
    {
        m_Indices.clear();
        m_Key.clear();
        m_Dirty = true;
    }

    /// The indices in [0, @p rowCount) for which @p matches(index) holds, for @p filter; cached until markDirty() or
    /// another filter.
    template<typename Matches>
    [[nodiscard]] std::span<const std::size_t> get(std::size_t rowCount, const std::string& filter, const Matches& matches)
    {
        if (m_Dirty || m_Key != filter)
        {
            m_Indices.clear();
            for (std::size_t i = 0; i < rowCount; ++i)
            {
                if (matches(i))
                {
                    m_Indices.push_back(i);
                }
            }
            m_Key = filter;
            m_Dirty = false;
        }
        return m_Indices;
    }

  private:
    std::vector<std::size_t> m_Indices;
    std::string m_Key; // the filter m_Indices was built for
    bool m_Dirty = true;
};

/// A section header's label: "<title> (<count>)<id>" while it lists rows, else "<title><id>", the "###" id keeping
/// the header's ImGui ID (and open state) the same whatever it says. Formatted only when the count changes.
class CountedHeader
{
  public:
    /// The label for a section that is @p listed with @p count rows. The pointer stays valid until the next call.
    [[nodiscard]] const char* label(bool listed, std::size_t count, std::string_view title, std::string_view id)
    {
        if (!listed)
        {
            if (m_Plain.empty())
            {
                m_Plain = std::format("{}{}", title, id);
            }
            return m_Plain.c_str();
        }
        if (count != m_Count)
        {
            m_Count = count;
            m_Counted = std::format("{} ({}){}", title, count, id);
        }
        return m_Counted.c_str();
    }

  private:
    std::string m_Plain;
    std::string m_Counted;
    std::size_t m_Count = SIZE_MAX; // the count m_Counted shows
};

} // namespace App::Detail
