#include "SaveFileDialog.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_video.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Core
{

namespace
{

/// Everything SDL needs to stay valid until the callback runs: the filters (SDL keeps pointers to their
/// strings) and the caller's callback. Owned by SDL's userdata from the call until the callback.
struct DialogRequest
{
    std::vector<FileFilter> filters;
    std::vector<SDL_DialogFileFilter> sdlFilters;
    SaveFileCallback callback;
};

/// The error the callback gets when building its result throws (#2415). Short enough to fit every
/// standard library's small-string buffer, so assigning it allocates nothing even while allocation fails.
constexpr const char* RESULT_FAILED_ERROR = "out of memory";

/// Logs that part of onDialogClosed() threw, best-effort: logging allocates too, and nothing may leave
/// onDialogClosed() (as guardLayerCall does).
void logDialogFailure(const char* what, const char* reason) noexcept
{
    try
    {
        spdlog::error("File dialog {}: {}", what, reason);
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- intentional: logging is best-effort here, must not throw
    {}
}

/// Turns SDL's arguments into the caller's result. Copying the path or SDL's error allocates, so this
/// can throw std::bad_alloc.
[[nodiscard]] SaveFileResult makeResult(const char* const* fileList, int filter)
{
    SaveFileResult result;
    if (fileList == nullptr)
    {
        result.error = SDL_GetError();
    }
    else if (fileList[0] != nullptr) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): SDL's null-terminated list
    {
        result.path = fileList[0]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        result.filter = filter;
    }
    return result;
}

/// SDL's C callback, maybe on SDL's dialog thread (the portal or zenity thread on Linux). Nothing may
/// unwind out of it into SDL's C frames (#2162): building the result allocates and the caller's callback
/// can throw, so both are caught and logged here. The request is freed either way. Should building the
/// result fail, the callback still runs, once, with RESULT_FAILED_ERROR: callers clear their busy state
/// only there, so skipping it would leave Save or Dump stuck (#2415).
void SDLCALL onDialogClosed(void* userdata, const char* const* fileList, int filter) noexcept
{
    const std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
    try
    {
        SaveFileResult result;
        try
        {
            result = makeResult(fileList, filter);
        }
        catch (const std::exception& e)
        {
            // result is still default-constructed: the assignment above never ran.
            logDialogFailure("result could not be built", e.what());
            result.error = RESULT_FAILED_ERROR;
        }
        if (request->callback)
        {
            request->callback(std::move(result));
        }
    }
    catch (const std::exception& e)
    {
        logDialogFailure("callback threw", e.what());
    }
    catch (...)
    {
        logDialogFailure("callback threw", "unknown exception");
    }
}

} // namespace

void showSaveFileDialog(SDL_Window* window, std::vector<FileFilter> filters, const std::string& defaultLocation, SaveFileCallback callback)
{
    auto request = std::make_unique<DialogRequest>();
    request->filters = std::move(filters);
    request->callback = std::move(callback);
    request->sdlFilters.reserve(request->filters.size());
    for (const FileFilter& filter : request->filters)
    {
        request->sdlFilters.push_back({.name = filter.name.c_str(), .pattern = filter.pattern.c_str()});
    }
    const SDL_DialogFileFilter* sdlFilters = request->sdlFilters.empty() ? nullptr : request->sdlFilters.data();
    const auto count = static_cast<int>(request->sdlFilters.size());
    // From here SDL owns the request: onDialogClosed() takes it back and frees it.
    SDL_ShowSaveFileDialog(
        &onDialogClosed, request.release(), window, sdlFilters, count, defaultLocation.empty() ? nullptr : defaultLocation.c_str());
}

void showFolderDialog(SDL_Window* window, const std::string& defaultLocation, SaveFileCallback callback)
{
    auto request = std::make_unique<DialogRequest>();
    request->callback = std::move(callback);
    // From here SDL owns the request, as for the save dialog.
    SDL_ShowOpenFolderDialog(
        &onDialogClosed, request.release(), window, defaultLocation.empty() ? nullptr : defaultLocation.c_str(), false);
}

void showOpenFileDialog(SDL_Window* window, const std::string& defaultLocation, SaveFileCallback callback)
{
    auto request = std::make_unique<DialogRequest>();
    request->callback = std::move(callback);
    // From here SDL owns the request, as for the save dialog. No filters: any file can be a program.
    SDL_ShowOpenFileDialog(
        &onDialogClosed, request.release(), window, nullptr, 0, defaultLocation.empty() ? nullptr : defaultLocation.c_str(), false);
}

std::string defaultSaveFolder()
{
    if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); documents != nullptr && *documents != '\0')
    {
        return documents;
    }
    if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME); home != nullptr && *home != '\0')
    {
        return home;
    }
    return {};
}

} // namespace Core
