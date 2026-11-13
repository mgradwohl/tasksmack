#pragma once

/// @file WindowsSystemMenu.h
/// @brief The native Windows system menu that the title-bar icon opens (#3161), apart from TitleBarLayer so a
/// test can show it on a window of its own (#3490: CiRealSystemSystemMenuTest) without a running Core::Application.

#include "App/TitleBarView.h"

namespace App
{

#ifdef _WIN32
/// Shows @p nativeWindow's (an HWND) system menu at screen point (@p screenX, @p screenY), its items enabled for a
/// window that is @p maximized or not (TitleBarView::systemMenuItems()), and blocks in the modal menu loop until it
/// is answered. Returns the window command chosen, for the caller to carry out as the title-bar buttons do,
/// client-side maximize included (#3161); None when the menu was dismissed, when the window has no system menu, or
/// when the choice (Move, Size) was posted to the window as WM_SYSCOMMAND for Windows' own keyboard loops.
[[nodiscard]] TitleBarView::Action showWindowsSystemMenu(void* nativeWindow, bool maximized, int screenX, int screenY);
#endif

} // namespace App
