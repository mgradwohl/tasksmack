#pragma once

// The main window's native handle (#2550), for a system dialog or prompt to belong to: the dump
// folder picker's owner, and the UAC prompt the privileged helper is started behind. Its own header,
// so a panel view that needs it doesn't include the composition root's PanelPlatforms.h.

namespace App
{

/// An HWND on Windows; null where there is no window (headless tests) or none of that kind.
[[nodiscard]] void* mainNativeWindow();

} // namespace App
