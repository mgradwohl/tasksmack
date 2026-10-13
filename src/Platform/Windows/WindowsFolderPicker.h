#pragma once

// The Windows folder dialog (#2001): the shell's IFileOpenDialog with FOS_PICKFOLDERS, for "Save dump
// to...". Blocking until the user answers, so callers run it on a worker thread; it is modal to the
// owner window it is given, which Windows disables while it is up, as SDL's file dialogs do.

#include "Platform/IProcessActions.h"

#include <filesystem>
#include <string_view>

namespace Platform::Windows
{

/// Shows the folder dialog titled @p title, starting in @p initial (when it exists), modal to
/// @p ownerWindow (an HWND, or null), and returns the folder chosen; nullopt when it was cancelled.
/// Sets up COM (single-threaded apartment) on the calling thread for the call.
[[nodiscard]] FolderChoice pickFolder(void* ownerWindow, const std::filesystem::path& initial, std::wstring_view title);

} // namespace Platform::Windows
