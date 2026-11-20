#pragma once

// Pure text-sanitizing helper used when building ProcessSnapshot, so process-supplied strings can
// never carry control characters into the UI. Extracted as a small header following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (as
// ProcessTreeFlatten.h and ProcessTreeIndent.h do), so it is unit-testable directly rather than
// only through a live probe.

#include "Utf8Sanitize.h"

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

/// True when @p text has nothing toSingleLine() would replace -- no control byte, and well-formed UTF-8 -- i.e.
/// it is already its own single-line form, as almost every process name and command line is (#1624).
///
/// The loops have no early exit, so the compiler can vectorize them: read-only passes are far cheaper than the
/// rewrite they let the caller skip, and ASCII text is never decoded (isValidUtf8()).
[[nodiscard]] constexpr bool isSingleLine(std::string_view text) noexcept
{
    unsigned controls = 0;
    for (const char c : text)
    {
        controls |= static_cast<unsigned>(isSingleLineControl(c));
    }
    return controls == 0U && isValidUtf8(text);
}

namespace SingleLineDetail
{
/// toSingleLineInto() for text that isn't well-formed UTF-8: sanitized, then its controls made spaces. Rare, so kept
/// out of the hot path.
inline bool toSingleLineIntoSanitizing(std::string& out, std::string_view text)
{
    out.clear();
    appendSanitizedUtf8(out, text);
    for (char& c : out)
    {
        c = isSingleLineControl(c) ? ' ' : c;
    }
    return true;
}
} // namespace SingleLineDetail

/// Writes the single-line form of @p text (see toSingleLine()) into @p out, reusing @p out's
/// capacity, and returns whether it differs from @p text, i.e. whether any byte was replaced.
inline bool toSingleLineInto(std::string& out, std::string_view text)
{
    // Well-formed text, almost all of it, takes the one copy-and-rewrite loop it took before UTF-8 was checked; the
    // check itself is isValidUtf8()'s word-at-a-time ASCII scan (#4525). A single loop that also gathers the high
    // bit measured twice as slow: the extra reduction stops the rewrite vectorizing.
    if (!isValidUtf8(text))
    {
        return SingleLineDetail::toSingleLineIntoSanitizing(out, text);
    }
    out.assign(text);
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
/// Tabs and carriage returns are collapsed for the same reason. Well-formed UTF-8 survives intact; an
/// ill-formed sequence (a 15-byte Linux comm cut mid-character, a Latin-1 file name in a command line)
/// becomes U+FFFD, one per maximal ill-formed subpart (Utf8Sanitize.h, #4301).
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
