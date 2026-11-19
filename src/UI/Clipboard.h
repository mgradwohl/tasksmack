#pragma once

// The one way TaskSmack puts text on the clipboard (#4325, the UTF-8 audit's D1). Text copied from a process
// (a command line holding a Latin-1 file name, a 15-byte comm cut mid-character) can be ill-formed UTF-8, and a
// GTK4 or Qt app then pastes nothing or mojibake. setClipboardUtf8() runs Domain::sanitizedUtf8() first, so each
// maximal ill-formed subpart becomes U+FFFD; well-formed text is copied as it is. A pre-commit hook bans the bare
// ImGui::SetClipboardText() everywhere else.

#include "Domain/Utf8Sanitize.h"

#include <imgui.h>

#include <string>
#include <string_view>

namespace UI
{

/// What a clipboard setter takes: ImGui::SetClipboardText's signature, so a test can pass its own.
using ClipboardSetter = void (*)(const char* text);

/// Puts @p text on the clipboard as well-formed UTF-8, through @p set (ImGui::SetClipboardText unless a test
/// passes another).
inline void setClipboardUtf8(std::string_view text, ClipboardSetter set = &ImGui::SetClipboardText)
{
    const std::string wellFormed = Domain::sanitizedUtf8(text);
    set(wellFormed.c_str());
}

/// Empties the clipboard through @p set, allocating nothing: for a noexcept path whose own copy failed, so the
/// previous clipboard isn't passed off as its data.
inline void clearClipboard(ClipboardSetter set = &ImGui::SetClipboardText) noexcept
{
    set("");
}

} // namespace UI
