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
    std::string out(text);
    for (char& c : out)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || byte == 0x7FU)
        {
            c = ' ';
        }
    }
    return out;
}

} // namespace Domain
