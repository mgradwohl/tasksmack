#pragma once

// Pure text-sanitizing helper used when building ProcessSnapshot, so process-supplied strings can
// never carry control characters into the UI. Extracted as a small header following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (as
// ProcessTreeFlatten.h and ProcessTreeIndent.h do), so it is unit-testable directly rather than
// only through a live probe.

#include <string>
#include <string_view>

namespace Domain
{

/// True when @p byte is one toSingleLine() replaces: a C0 control (including NUL) or DEL.
[[nodiscard]] constexpr bool isSingleLineControl(char byte) noexcept
{
    const auto value = static_cast<unsigned char>(byte);
    return value < 0x20U || value == 0x7FU;
}

/// True when @p text has no byte toSingleLine() would replace, i.e. it is already its own single-line
/// form -- as almost every process name and command line is (#1624).
///
/// The loop has no early exit, so the compiler can vectorize it: one read-only pass is far cheaper
/// than the rewrite it lets the caller skip.
[[nodiscard]] constexpr bool isSingleLine(std::string_view text) noexcept
{
    unsigned controls = 0;
    for (const char c : text)
    {
        controls |= static_cast<unsigned>(isSingleLineControl(c));
    }
    return controls == 0U;
}

/// Writes the single-line form of @p text (see toSingleLine()) into @p out, reusing @p out's
/// capacity, and returns whether it differs from @p text, i.e. whether any byte was replaced.
inline bool toSingleLineInto(std::string& out, std::string_view text)
{
    out.assign(text);
    // Branch-free, so it vectorizes like isSingleLine(): every byte is rewritten, to itself or a space.
    unsigned controls = 0;
    for (char& c : out)
    {
        const bool control = isSingleLineControl(c);
        controls |= static_cast<unsigned>(control);
        c = control ? ' ' : c;
    }
    return controls != 0U;
}

/// Collapse any control character in `text` to a space, returning a string safe to render in a
/// single-line UI cell.
///
/// A process controls its own argv, and a newline inside an argument survives /proc/[pid]/cmdline
/// (which only uses NUL as the *separator*) and the Windows PEB command line alike. Rendered
/// verbatim, ImGui draws multi-line text, which grows that table row taller than every other row --
/// breaking the uniform-row-height assumption ImGuiListClipper relies on to map scroll offset to
/// row index, so the whole table's scrolling is thrown off, not just the offending row (#919).
///
/// Tabs and carriage returns are collapsed for the same reason. Bytes >= 0x80 are left untouched so
/// UTF-8 sequences survive intact; only C0 controls and DEL are replaced.
[[nodiscard]] inline std::string toSingleLine(std::string_view text)
{
    std::string out;
    toSingleLineInto(out, text);
    return out;
}

/// The single-line form of one per-process string (its name or its command), kept across refreshes
/// so a string that has not changed since the last refresh is not scanned or rewritten again (#1624).
///
/// Only a form that differs from its raw string is stored: the usual raw string, already single-line,
/// is its own single-line form, so the memo keeps just that fact and no copy.
class SingleLineMemo
{
  public:
    /// Brings the memo up to date with @p raw. With @p rawUnchanged -- @p raw equals the string the
    /// memo last saw -- it keeps what it has and returns false; otherwise it derives the single-line
    /// form afresh and returns true. The first update of a memo must pass false.
    bool update(std::string_view raw, bool rawUnchanged)
    {
        if (rawUnchanged)
        {
            return false;
        }
        m_RawIsSingleLine = isSingleLine(raw);
        if (m_RawIsSingleLine)
        {
            m_SingleLine.clear();
        }
        else
        {
            toSingleLineInto(m_SingleLine, raw);
        }
        return true;
    }

    /// The single-line form of @p raw, which must be the string last passed to update().
    [[nodiscard]] std::string value(std::string_view raw) const
    {
        return m_RawIsSingleLine ? std::string(raw) : m_SingleLine;
    }

  private:
    std::string m_SingleLine; // the single-line form, kept only when it differs from the raw string
    bool m_RawIsSingleLine = true;
};

} // namespace Domain
