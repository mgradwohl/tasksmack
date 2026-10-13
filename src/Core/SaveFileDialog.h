#pragma once

// The platform's save-file dialog (#1866), through SDL3's SDL_ShowSaveFileDialog. Asynchronous: the
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
void showSaveFileDialog(SDL_Window* window, std::vector<FileFilter> filters, const std::string& defaultLocation, SaveFileCallback callback);

/// The user's Documents folder with a trailing separator, else their home folder, else empty.
[[nodiscard]] std::string defaultSaveFolder();

} // namespace Core
