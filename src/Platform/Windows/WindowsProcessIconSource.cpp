#include "WindowsProcessIconSource.h"

#include "Platform/IProcessIconSource.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
// clang-format on

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include "WinString.h"
#include "WindowsProcessIconMath.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

namespace
{

/// The largest icon read: Windows' own largest icon size. Anything bigger is refused, not scaled.
constexpr int MAX_ICON_SIZE_PX = 256;

/// QueryFullProcessImageNameW's buffer: MAX_PATH first, then the long-path limit once.
constexpr std::array<DWORD, 2> IMAGE_PATH_BUFFER_SIZES{MAX_PATH, 32767};

/// Owns one handle and releases it through the function table's call for it (CloseHandle,
/// DestroyIcon, DeleteDC, DeleteObject), so a fake table sees every release.
template<typename Handle, typename Release> class TableOwned
{
  public:
    TableOwned(Handle handle, Release release) noexcept : m_Handle(handle), m_Release(release)
    {}

    ~TableOwned()
    {
        if (m_Handle != nullptr && m_Release != nullptr)
        {
            static_cast<void>(m_Release(m_Handle));
        }
    }

    TableOwned(const TableOwned&) = delete;
    TableOwned& operator=(const TableOwned&) = delete;
    TableOwned(TableOwned&&) = delete;
    TableOwned& operator=(TableOwned&&) = delete;

    [[nodiscard]] Handle get() const noexcept
    {
        return m_Handle;
    }

  private:
    Handle m_Handle;
    Release m_Release;
};

/// @p rows rows of @p bitmap, @p width wide, as a 32 bpp top-down DIB; nullopt when GetDIBits
/// copies fewer rows.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
readBitmapBits(const Windows::ProcessIconFunctions& fns, HDC dc, HBITMAP bitmap, int width, int rows)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -rows; // Negative: top-down rows, as IconPixels holds them
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint8_t> bits(Windows::IconMath::imageBytes(width, rows));
    const int copied = fns.getDIBits(dc, bitmap, 0, static_cast<UINT>(rows), bits.data(), &info, DIB_RGB_COLORS);
    if (copied != rows)
    {
        return std::nullopt;
    }
    return bits;
}

} // namespace

namespace Windows
{

ProcessIconFunctions ProcessIconFunctions::system() noexcept
{
    return ProcessIconFunctions{
        .openProcess = &::OpenProcess,
        .queryFullProcessImageName = &::QueryFullProcessImageNameW,
        .closeHandle = &::CloseHandle,
        .extractIcon = &::SHDefExtractIconW,
        .destroyIcon = &::DestroyIcon,
        .getIconInfo = &::GetIconInfo,
        .getObject = &::GetObjectW,
        .createCompatibleDC = &::CreateCompatibleDC,
        .deleteDC = &::DeleteDC,
        .getDIBits = &::GetDIBits,
        .deleteObject = &::DeleteObject,
    };
}

} // namespace Windows

WindowsProcessIconSource::WindowsProcessIconSource(Windows::ProcessIconFunctions fns) : m_Fns(fns)
{}

std::string WindowsProcessIconSource::executablePath(std::int32_t pid)
{
    if (pid <= 0)
    {
        return {}; // The Idle "process" and the like: no executable
    }
    const TableOwned process(m_Fns.openProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)), m_Fns.closeHandle);
    if (process.get() == nullptr)
    {
        return {};
    }

    std::size_t attempt = 0;
    while (attempt < IMAGE_PATH_BUFFER_SIZES.size())
    {
        std::wstring path(IMAGE_PATH_BUFFER_SIZES[attempt], L'\0');
        auto size = static_cast<DWORD>(path.size());
        // On success, size is the number of characters written, without the terminator.
        if (m_Fns.queryFullProcessImageName(process.get(), 0, path.data(), &size) != 0)
        {
            path.resize(size);
            return WinString::wideToUtf8(path);
        }
        ++attempt;
    }
    return {};
}

std::optional<IconPixels> WindowsProcessIconSource::loadIcon(std::string_view executablePath, std::string_view /*processName*/, int sizePx)
{
    if (executablePath.empty() || sizePx <= 0 || sizePx > MAX_ICON_SIZE_PX)
    {
        return std::nullopt;
    }
    const std::wstring widePath = WinString::utf8ToWide(executablePath);
    if (widePath.empty())
    {
        return std::nullopt;
    }

    HICON large = nullptr;
    // The low word of the size is the large icon's; no small icon is asked for. Index 0 is the icon
    // Explorer and Task Manager show for the file.
    const HRESULT result = m_Fns.extractIcon(widePath.c_str(), 0, 0, &large, nullptr, static_cast<UINT>(sizePx));
    const TableOwned icon(large, m_Fns.destroyIcon);
    if (result != S_OK || icon.get() == nullptr)
    {
        return std::nullopt; // S_FALSE: the file has no icon
    }
    return pixelsOf(m_Fns, icon.get());
}

std::optional<IconPixels> WindowsProcessIconSource::pixelsOf(const Windows::ProcessIconFunctions& fns, HICON icon)
{
    ICONINFO info{};
    if (fns.getIconInfo(icon, &info) == 0)
    {
        return std::nullopt;
    }
    // GetIconInfo hands over copies of both bitmaps; the caller deletes them.
    const TableOwned color(info.hbmColor, fns.deleteObject);
    const TableOwned mask(info.hbmMask, fns.deleteObject);
    if (mask.get() == nullptr)
    {
        return std::nullopt; // Every icon has a mask; without one there is nothing to read
    }

    const bool monochrome = color.get() == nullptr;
    BITMAP bitmap{};
    if (fns.getObject(monochrome ? mask.get() : color.get(), sizeof(BITMAP), &bitmap) == 0)
    {
        return std::nullopt;
    }
    const int width = bitmap.bmWidth;
    // A monochrome icon's mask is the AND mask above the XOR mask: twice the icon's height.
    const int height = monochrome ? (bitmap.bmHeight / 2) : bitmap.bmHeight;
    if (width <= 0 || height <= 0 || width > MAX_ICON_SIZE_PX || height > MAX_ICON_SIZE_PX)
    {
        return std::nullopt;
    }

    const TableOwned dc(fns.createCompatibleDC(nullptr), fns.deleteDC);
    if (dc.get() == nullptr)
    {
        return std::nullopt;
    }

    if (monochrome)
    {
        const auto maskBits = readBitmapBits(fns, dc.get(), mask.get(), width, height * 2);
        if (!maskBits.has_value())
        {
            return std::nullopt;
        }
        return Windows::IconMath::iconFromMonochrome(*maskBits, width, height);
    }

    const auto colorBits = readBitmapBits(fns, dc.get(), color.get(), width, height);
    if (!colorBits.has_value())
    {
        return std::nullopt;
    }
    // The mask matters only to an icon without alpha; unreadable, such an icon is drawn opaque.
    const auto maskBits = readBitmapBits(fns, dc.get(), mask.get(), width, height);
    const std::span<const std::uint8_t> maskSpan =
        maskBits.has_value() ? std::span<const std::uint8_t>(*maskBits) : std::span<const std::uint8_t>{};
    return Windows::IconMath::iconFromColor(*colorBits, maskSpan, width, height);
}

} // namespace Platform
