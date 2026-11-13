#include "App/WindowsSystemMenu.h"

#ifdef _WIN32

#include "App/TitleBarView.h"
#include "Core/WindowGeometry.h"

#include <cstdint>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace App
{

// LCOV_EXCL_START - TrackPopupMenu runs a modal menu loop until it is answered. CiRealSystemSystemMenuTest
// (tests/Platform/test_CiRealSystemWindowsApp.cpp) answers it on the CI runner, but win-coverage doesn't run that
// group, so the lines would count as uncovered (#2039, #3490).
TitleBarView::Action showWindowsSystemMenu(void* nativeWindow, bool maximized, int screenX, int screenY)
{
    auto* hwnd = static_cast<HWND>(nativeWindow);
    HMENU systemMenu = hwnd != nullptr ? GetSystemMenu(hwnd, FALSE) : nullptr;
    if (systemMenu == nullptr)
    {
        return TitleBarView::Action::None;
    }

    // Windows enables the items from the OS's own maximized state, which a client-side maximize
    // doesn't set: it offered Maximize, not Restore, on a maximized window (#3161). Set them as the
    // Linux menu has them.
    const TitleBarView::SystemMenuItems items = TitleBarView::systemMenuItems(maximized);
    const auto enable = [systemMenu](UINT command, bool enabled)
    {
        // Returns the item's previous state, or -1 for a menu without it; either way there is nothing to do.
        static_cast<void>(EnableMenuItem(systemMenu, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED)));
    };
    enable(SC_RESTORE, items.restore);
    enable(SC_MOVE, items.move);
    enable(SC_SIZE, items.size);
    enable(SC_MINIMIZE, items.minimize);
    enable(SC_MAXIMIZE, items.maximize);
    enable(SC_CLOSE, items.close);

    const int cmd = TrackPopupMenu(systemMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, screenX, screenY, 0, hwnd, nullptr);
    if (cmd == 0)
    {
        return TitleBarView::Action::None; // dismissed
    }
    const TitleBarView::Action action =
        TitleBarView::actionForSystemCommand(Core::WindowGeometry::decodeSystemCommand(static_cast<std::uintptr_t>(cmd)));
    if (action == TitleBarView::Action::None)
    {
        // Move and Size: Windows' own keyboard move and size loops.
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
    }
    return action;
}
// LCOV_EXCL_STOP

} // namespace App

#endif // _WIN32
