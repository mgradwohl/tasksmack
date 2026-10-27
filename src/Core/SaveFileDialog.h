#pragma once

// The platform's save-file dialog (#1866), through SDL3's SDL_ShowSaveFileDialog, and its folder dialog
// (#807), through SDL_ShowOpenFolderDialog. Asynchronous: the
// result comes back through a callback that SDL may call on another thread, so callers hand it to their
// own thread (SystemInfoPanel keeps it in a mutex-guarded slot it polls each frame).

#include <SDL3/SDL_video.h>

#include <functional>
#include <string>
#include <vector>

namespace Core
{

/// One file type the dialog offers: "Text" with the pattern "txt" (extensions, ';'-separated, no dots).
struct FileFilter
{
    std::string name;
    std::string pattern;
};

/// What the dialog came back with.
struct SaveFileResult
{
    std::string path;  ///< The file chosen (it may exist already); empty when cancelled or failed
    int filter = -1;   ///< The index of the filter chosen; -1 when the platform doesn't say
    std::string error; ///< Why the dialog couldn't be shown; empty when it was
};

using SaveFileCallback = std::function<void(SaveFileResult)>;

/// Shows the save dialog, modal for @p window when the platform allows (may be null), offering
/// @p filters and starting at @p defaultLocation (a folder, or a folder and file name). Returns at once;
/// @p callback gets the result later, maybe on another thread, exactly once. Call from the main thread.
/// Should @p callback throw, the exception is caught and logged, never left to unwind into SDL (#2162).
/// Should building the result fail (out of memory), @p callback still gets one, with a non-empty error (#2415).
void showSaveFileDialog(SDL_Window* window, std::vector<FileFilter> filters, const std::string& defaultLocation, SaveFileCallback callback);

/// Shows the folder-picking dialog (SDL_ShowOpenFolderDialog; on Linux, the desktop portal's chooser),
/// modal for @p window when the platform allows (may be null), starting in @p defaultLocation. Returns at
/// once; @p callback gets the result later, maybe on another thread, exactly once: SaveFileResult::path is
/// the folder chosen, empty when cancelled (filter is unused). Call from the main thread. Used where the
/// platform layer has no folder dialog of its own (Create dump file on Linux, #807). A throwing
/// @p callback is caught and logged, as for the save dialog.
void showFolderDialog(SDL_Window* window, const std::string& defaultLocation, SaveFileCallback callback);

/// Shows the open-file dialog (SDL_ShowOpenFileDialog; on Linux, the desktop portal's chooser) for one
/// existing file, modal for @p window when the platform allows (may be null), starting in
/// @p defaultLocation. As showFolderDialog(): returns at once, and @p callback gets SaveFileResult::path,
/// the file chosen (empty when cancelled), later, maybe on another thread, exactly once. Run new task's
/// Browse... (#3065).
void showOpenFileDialog(SDL_Window* window, const std::string& defaultLocation, SaveFileCallback callback);

/// The user's Documents folder with a trailing separator, else their home folder, else empty.
[[nodiscard]] std::string defaultSaveFolder();

} // namespace Core
