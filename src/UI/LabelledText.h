#pragma once

// A value from outside the process with a label or note around it, drawn whole (#4306).
//
// ImGui::Text("Command: %s", value) formats into ImGui's 3 KB temp buffer and silently drops what does
// not fit, often part-way through a UTF-8 character; only a bare "%s" or "%.*s" skips that buffer. These
// join the parts first and draw the result through the bare format, so a long command line, path or
// error message reaches the screen whole, and text of a normal length looks exactly as it did.

#include "UI/InlineText.h"

#include <imgui.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace UI::Widgets
{

/// @p prefix, @p value and @p suffix joined into @p out, reusing its capacity. Returns the joined text.
inline std::string_view joinLabelledText(std::string& out, std::string_view prefix, std::string_view value, std::string_view suffix = {})
{
    out.clear();
    out.reserve(prefix.size() + value.size() + suffix.size());
    out.append(prefix).append(value).append(suffix);
    return out;
}

namespace Detail
{

/// Where the joined text is built: kept between calls, so redrawing a line every frame doesn't allocate
/// once the buffer has grown to fit it.
[[nodiscard]] inline std::string& labelledTextScratch()
{
    thread_local std::string scratch;
    return scratch;
}

/// The byte count for ImGui's "%.*s", which takes an int: at most INT_MAX, cut on a code point boundary.
[[nodiscard]] inline int imguiTextLength(std::string_view text) noexcept
{
    return static_cast<int>(utf8PrefixLength(text, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

/// @p prefix, @p value and @p suffix joined in the scratch buffer, which is null-terminated.
[[nodiscard]] inline const std::string& joined(std::string_view prefix, std::string_view value, std::string_view suffix)
{
    std::string& scratch = labelledTextScratch();
    static_cast<void>(joinLabelledText(scratch, prefix, value, suffix));
    return scratch;
}

} // namespace Detail

/// ImGui::Text() of @p prefix, @p value and @p suffix, with no limit on the value's length.
inline void labelledText(std::string_view prefix, std::string_view value, std::string_view suffix = {})
{
    const std::string& text = Detail::joined(prefix, value, suffix);
    ImGui::Text("%.*s", Detail::imguiTextLength(text), text.c_str());
}

/// ImGui::TextColored() of @p prefix, @p value and @p suffix, with no limit on the value's length.
inline void labelledTextColored(const ImVec4& color, std::string_view prefix, std::string_view value, std::string_view suffix = {})
{
    const std::string& text = Detail::joined(prefix, value, suffix);
    ImGui::TextColored(color, "%.*s", Detail::imguiTextLength(text), text.c_str());
}

/// ImGui::TextDisabled() of @p prefix, @p value and @p suffix, with no limit on the value's length.
inline void labelledTextDisabled(std::string_view prefix, std::string_view value, std::string_view suffix = {})
{
    const std::string& text = Detail::joined(prefix, value, suffix);
    ImGui::TextDisabled("%.*s", Detail::imguiTextLength(text), text.c_str());
}

/// ImGui::TextWrapped() of @p prefix, @p value and @p suffix, with no limit on the value's length.
inline void labelledTextWrapped(std::string_view prefix, std::string_view value, std::string_view suffix = {})
{
    const std::string& text = Detail::joined(prefix, value, suffix);
    ImGui::TextWrapped("%.*s", Detail::imguiTextLength(text), text.c_str());
}

} // namespace UI::Widgets
