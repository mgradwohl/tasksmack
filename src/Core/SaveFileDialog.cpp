#include "SaveFileDialog.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_video.h>

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

void SDLCALL onDialogClosed(void* userdata, const char* const* fileList, int filter)
{
    const std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
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
    if (request->callback)
    {
        request->callback(std::move(result));
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
