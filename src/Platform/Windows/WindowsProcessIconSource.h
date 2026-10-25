#pragma once

// Process icons on Windows (#1800): the executable's own icon, as Task Manager shows it, read with
// SHDefExtractIconW at the size asked for and copied out of its bitmaps with GetDIBits. Every Win32
// call goes through a table of function pointers, the system's by default, so the tests drive the
// alpha, mask-only, monochrome and failure paths with fakes (test_WindowsProcessIconSource.cpp) and
// check that every icon, bitmap and DC is released. The pixel conversion is
// WindowsProcessIconMath.h. A packaged app's logo (a startup entry's, #1800) is a PNG, not an
// executable: its variant nearest the size is picked (WindowsPackageLogoMath.h) and decoded with
// stb_image.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/IProcessIconSource.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Platform
{

namespace Windows
{

/// The calls WindowsProcessIconSource makes: kernel32's, shell32's, user32's and gdi32's, or a
/// test's fakes.
struct ProcessIconFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using QueryFullProcessImageNameFn = BOOL(WINAPI*)(HANDLE, DWORD, LPWSTR, PDWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using ExtractIconFn = HRESULT(WINAPI*)(LPCWSTR, int, UINT, HICON*, HICON*, UINT);
    using DestroyIconFn = BOOL(WINAPI*)(HICON);
    using GetIconInfoFn = BOOL(WINAPI*)(HICON, PICONINFO);
    using GetObjectFn = int(WINAPI*)(HANDLE, int, LPVOID);
    using CreateCompatibleDCFn = HDC(WINAPI*)(HDC);
    using DeleteDCFn = BOOL(WINAPI*)(HDC);
    using GetDIBitsFn = int(WINAPI*)(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);
    using DeleteObjectFn = BOOL(WINAPI*)(HGDIOBJ);

    OpenProcessFn openProcess = nullptr;
    QueryFullProcessImageNameFn queryFullProcessImageName = nullptr;
    CloseHandleFn closeHandle = nullptr;
    ExtractIconFn extractIcon = nullptr; ///< SHDefExtractIconW
    DestroyIconFn destroyIcon = nullptr;
    GetIconInfoFn getIconInfo = nullptr;
    GetObjectFn getObject = nullptr; ///< GetObjectW
    CreateCompatibleDCFn createCompatibleDC = nullptr;
    DeleteDCFn deleteDC = nullptr;
    GetDIBitsFn getDIBits = nullptr;
    DeleteObjectFn deleteObject = nullptr;

    /// The system's functions.
    [[nodiscard]] static ProcessIconFunctions system() noexcept;
};

} // namespace Windows

/// Reads executables' icons. Stateless beyond its fixed function table, so safe from any thread.
class WindowsProcessIconSource final : public IProcessIconSource
{
  public:
    explicit WindowsProcessIconSource(Windows::ProcessIconFunctions fns = Windows::ProcessIconFunctions::system());

    [[nodiscard]] std::string executablePath(std::int32_t pid) override;
    [[nodiscard]] std::optional<IconPixels> loadIcon(std::string_view executablePath, std::string_view processName, int sizePx) override;
    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return true;
    }

    /// The pixels of packaged-app logo @p logoPath (as its manifest names it, under the package's
    /// folder) for drawing @p sizePx square: the file among the logo's scale and target-size variants
    /// in that folder that PackageLogo::pickLogo() picks, decoded. nullopt when the folder has no
    /// variant TaskSmack draws, or it can't be read.
    [[nodiscard]] static std::optional<IconPixels> loadPackageLogo(std::string_view logoPath, int sizePx);

    /// The pixels of @p icon, through @p fns; nullopt when its bitmaps can't be read. Releases the
    /// bitmaps GetIconInfo hands over, never the icon itself (the caller owns it).
    [[nodiscard]] static std::optional<IconPixels> pixelsOf(const Windows::ProcessIconFunctions& fns, HICON icon);

  private:
    Windows::ProcessIconFunctions m_Fns;
};

} // namespace Platform

#endif // _WIN32
