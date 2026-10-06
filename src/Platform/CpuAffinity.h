#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{

/// The logical processors a process may run on, as a bitset of any width (#1247): bit N is logical
/// processor N, numbered as the per-core CPU figures are (Linux: the kernel's CPU number). A single
/// 64-bit mask could not describe a CPU at index 64 or above.
///
/// Up to 64 processors live inline, so copying a process's counters and snapshot every sample costs
/// no allocation on such machines; only an affinity that includes a processor at 64 or above keeps
/// its words on the heap. Empty (no processors) means the affinity could not be read.
class CpuAffinity
{
  public:
    static constexpr std::size_t BITS_PER_WORD = 64;
    /// Processor indices from here up are rejected as malformed: far beyond any kernel's NR_CPUS
    /// (8192 at most today), and it bounds what a corrupt list can allocate (1024 words, 8 KiB).
    static constexpr std::size_t MAX_CPUS = std::size_t{1} << 16U;

    CpuAffinity() = default;

    /// From a 64-bit mask, bit N = processor N: Windows GetProcessAffinityMask()'s process mask.
    [[nodiscard]] static CpuAffinity fromMask(std::uint64_t mask) noexcept
    {
        CpuAffinity affinity;
        affinity.m_Inline = mask;
        return affinity;
    }

    /// Parses a kernel CPU list, as /proc/<pid>/status Cpus_allowed_list prints it: comma-separated
    /// processor numbers and inclusive ranges, e.g. "0-3,8,64-127". Surrounding whitespace is
    /// ignored. nullopt for anything else -- empty, an empty item, a reversed range, a stride, a
    /// number at or above MAX_CPUS, stray characters -- rather than whatever part of it parsed.
    [[nodiscard]] static std::optional<CpuAffinity> fromCpuList(std::string_view text)
    {
        constexpr std::string_view WHITESPACE = " \t\r\n";
        const auto first = text.find_first_not_of(WHITESPACE);
        if (first == std::string_view::npos)
        {
            return std::nullopt;
        }
        text = text.substr(first, text.find_last_not_of(WHITESPACE) - first + 1);

        CpuAffinity affinity;
        const char* p = text.data();
        const char* const end = text.data() + text.size();
        const auto parseCpu = [&p, end](std::size_t& cpu) -> bool
        {
            // from_chars takes no sign or space, so "-1", "+1" and " 1" fail here too.
            const auto [next, ec] = std::from_chars(p, end, cpu);
            if (ec != std::errc{} || cpu >= MAX_CPUS)
            {
                return false;
            }
            p = next;
            return true;
        };
        while (true)
        {
            std::size_t low = 0;
            if (!parseCpu(low))
            {
                return std::nullopt;
            }
            std::size_t high = low;
            if (p < end && *p == '-')
            {
                ++p;
                if (!parseCpu(high) || high < low)
                {
                    return std::nullopt;
                }
            }
            affinity.setRange(low, high);
            if (p == end)
            {
                return affinity;
            }
            if (*p != ',')
            {
                return std::nullopt;
            }
            ++p; // A trailing comma leaves nothing to parse, which fails above
        }
    }

    /// Adds processor `cpu`.
    void set(std::size_t cpu)
    {
        setRange(cpu, cpu);
    }

    /// Adds processors `low` through `high`, inclusive. Requires low <= high.
    void setRange(std::size_t low, std::size_t high)
    {
        const std::size_t lastWord = high / BITS_PER_WORD;
        if (lastWord > 0 && m_Words.empty())
        {
            // Spill: from now on every word, word 0 included, lives in m_Words.
            m_Words.assign(lastWord + 1, 0);
            m_Words.front() = m_Inline;
            m_Inline = 0;
        }
        else if (lastWord >= m_Words.size() && !m_Words.empty())
        {
            m_Words.resize(lastWord + 1, 0);
        }
        for (std::size_t word = low / BITS_PER_WORD; word <= lastWord; ++word)
        {
            const std::size_t from = (word == low / BITS_PER_WORD) ? low % BITS_PER_WORD : 0;
            const std::size_t to = (word == lastWord) ? high % BITS_PER_WORD : BITS_PER_WORD - 1;
            const std::size_t width = to - from + 1;
            const std::uint64_t bits = (width == BITS_PER_WORD) ? ~std::uint64_t{0} : (((std::uint64_t{1} << width) - 1U) << from);
            if (m_Words.empty())
            {
                m_Inline |= bits;
            }
            else
            {
                m_Words[word] |= bits;
            }
        }
    }

    /// Keeps only the processors `other` also includes (bitwise AND), e.g. a process's allowed CPUs
    /// with the online ones. Never allocates: the result is no wider than this one was, and a result
    /// within processors 0-63 moves back inline. Empty when the two have no processor in common.
    void intersectWith(const CpuAffinity& other) noexcept
    {
        const auto theirs = other.words();
        if (m_Words.empty())
        {
            m_Inline &= theirs.empty() ? std::uint64_t{0} : theirs.front();
            return;
        }
        // Spilled. `other` may be this very object; ANDing a word with itself is harmless.
        std::size_t kept = std::min(m_Words.size(), theirs.size());
        for (std::size_t word = 0; word < kept; ++word)
        {
            m_Words[word] &= theirs[word];
        }
        while (kept > 0 && m_Words[kept - 1] == 0)
        {
            --kept; // Drop trailing zero words: equality and ordering compare words() as-is
        }
        if (kept <= 1)
        {
            m_Inline = (kept == 1) ? m_Words.front() : std::uint64_t{0};
            m_Words.clear(); // Keeps the capacity; no allocation either way
        }
        else
        {
            m_Words.erase(m_Words.begin() + static_cast<std::ptrdiff_t>(kept), m_Words.end());
        }
    }

    /// Whether processor `cpu` is included.
    [[nodiscard]] bool test(std::size_t cpu) const noexcept
    {
        const auto all = words();
        const std::size_t word = cpu / BITS_PER_WORD;
        return word < all.size() && ((all[word] >> (cpu % BITS_PER_WORD)) & 1U) != 0;
    }

    /// No processors: the affinity could not be read.
    [[nodiscard]] bool empty() const noexcept
    {
        return words().empty();
    }

    /// How many processors are included.
    [[nodiscard]] std::size_t count() const noexcept
    {
        std::size_t total = 0;
        for (const std::uint64_t word : words())
        {
            total += static_cast<std::size_t>(std::popcount(word)); // popcount of a 64-bit word is <= 64
        }
        return total;
    }

    /// The bitset as 64-bit words, processors 0-63 first, with no trailing zero word (so empty when
    /// no processor is included). Valid until this object is modified or destroyed.
    [[nodiscard]] std::span<const std::uint64_t> words() const noexcept
    {
        if (!m_Words.empty())
        {
            return m_Words;
        }
        return (m_Inline != 0) ? std::span<const std::uint64_t>(&m_Inline, 1) : std::span<const std::uint64_t>{};
    }

    friend bool operator==(const CpuAffinity& lhs, const CpuAffinity& rhs) noexcept
    {
        return std::ranges::equal(lhs.words(), rhs.words());
    }

    /// Orders as the bitsets' numeric values: by the highest processor included, then down. For
    /// affinities within processors 0-63 that's the order the old 64-bit mask sorted in, and an
    /// unreadable (empty) affinity sorts below every reading.
    friend std::strong_ordering operator<=>(const CpuAffinity& lhs, const CpuAffinity& rhs) noexcept
    {
        const auto left = lhs.words();
        const auto right = rhs.words();
        if (left.size() != right.size())
        {
            return left.size() <=> right.size(); // No trailing zero words: more words is a bigger value
        }
        for (std::size_t i = left.size(); i > 0; --i)
        {
            if (left[i - 1] != right[i - 1])
            {
                return left[i - 1] <=> right[i - 1];
            }
        }
        return std::strong_ordering::equal;
    }

  private:
    // Invariant: m_Words is empty, or holds every word (at least two) with a non-zero last word;
    // m_Inline is then 0. setRange() only adds processors, so it can't leave a trailing zero word;
    // intersectWith() trims any it leaves, and moves a result of one word or none back inline.
    std::uint64_t m_Inline = 0;         // Processors 0-63, while no higher one is included
    std::vector<std::uint64_t> m_Words; // Every word, once a processor at 64 or above is included
};

} // namespace Platform
