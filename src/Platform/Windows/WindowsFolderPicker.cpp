#include "WindowsFolderPicker.h"

#include "ComPtr.h"
#include "ComScope.h"
#include "Platform/IProcessActions.h"
#include "WinString.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <objbase.h> // NOLINT(misc-include-cleaner) - CoCreateInstance, CoTaskMemFree
#include <shobjidl_core.h> // NOLINT(misc-include-cleaner) - IFileOpenDialog, IShellItem, SHCreateItemFromParsingName
// clang-format on

#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// The folder @p item names, from its file-system path; empty when it has none.
[[nodiscard]] std::filesystem::path itemPath(IShellItem& item)
{
    PWSTR path = nullptr;
    if (FAILED(item.GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr)
    {
        return {};
    }
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

[[nodiscard]] std::string failure(std::string_view step, HRESULT hr)
{
    return std::format("Couldn't show the folder dialog: {} failed (0x{:08X})", step, static_cast<unsigned long>(hr));
}

} // namespace

FolderChoice pickFolder(void* ownerWindow, const std::filesystem::path& initial, std::wstring_view title)
{
    const ComScope com;
    if (!com.usable())
    {
        return std::unexpected(std::string("Couldn't show the folder dialog: COM is unavailable on this thread"));
    }
    // The reinterpret_casts below are COM's out-parameter convention: the call writes an interface pointer
    // of the IID's type through void**, and the ComPtr's slot holds exactly that type.
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr =
        CoCreateInstance(CLSID_FileOpenDialog,
                         nullptr,
                         CLSCTX_INPROC_SERVER,
                         IID_IFileOpenDialog,
                         reinterpret_cast<void**>(dialog.releaseAndGetAddressOf())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr))
    {
        return std::unexpected(failure("CoCreateInstance", hr));
    }
    FILEOPENDIALOGOPTIONS options = 0;
    static_cast<void>(dialog->GetOptions(&options));
    // Folders only, on the file system (a dump is a file), that exist. The FOS_ values are a signed C
    // enum; FILEOPENDIALOGOPTIONS is the DWORD they are ORed into.
    constexpr FILEOPENDIALOGOPTIONS FOLDER_OPTIONS =
        static_cast<FILEOPENDIALOGOPTIONS>(FOS_PICKFOLDERS) | static_cast<FILEOPENDIALOGOPTIONS>(FOS_FORCEFILESYSTEM) |
        static_cast<FILEOPENDIALOGOPTIONS>(FOS_PATHMUSTEXIST) | static_cast<FILEOPENDIALOGOPTIONS>(FOS_NOCHANGEDIR);
    hr = dialog->SetOptions(options | FOLDER_OPTIONS);
    if (FAILED(hr))
    {
        return std::unexpected(failure("SetOptions", hr));
    }
    const std::wstring titleText(title);
    static_cast<void>(dialog->SetTitle(titleText.c_str()));
    static_cast<void>(dialog->SetOkButtonLabel(L"Select folder"));

    std::error_code existsError;
    if (!initial.empty() && std::filesystem::is_directory(initial, existsError))
    {
        ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial.c_str(),
                nullptr,
                IID_IShellItem,
                reinterpret_cast<void**>(start.releaseAndGetAddressOf())))) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        {
            // SetFolder, not SetDefaultFolder: it opens where the dumps last went, not where the shell
            // last left any folder dialog.
            static_cast<void>(dialog->SetFolder(start.get()));
        }
    }

    hr = dialog->Show(static_cast<HWND>(ownerWindow));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return std::optional<std::filesystem::path>{};
    }
    if (FAILED(hr))
    {
        return std::unexpected(failure("Show", hr));
    }
    ComPtr<IShellItem> chosen;
    hr = dialog->GetResult(chosen.releaseAndGetAddressOf());
    if (FAILED(hr) || !chosen)
    {
        return std::unexpected(failure("GetResult", hr));
    }
    std::filesystem::path folder = itemPath(*chosen.get());
    if (folder.empty())
    {
        return std::unexpected(std::string("That folder isn't on the file system; choose another one"));
    }
    spdlog::info("Dump folder chosen: {}", WinString::wideToUtf8(folder.wstring()));
    return std::optional<std::filesystem::path>(std::move(folder));
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
