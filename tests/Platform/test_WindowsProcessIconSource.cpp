/// @file test_WindowsProcessIconSource.cpp
/// @brief WindowsProcessIconSource (#1800) through a fake function table: an executable's icon with
/// alpha, one whose alpha comes from its mask, a monochrome one, and every failure path -- no icon,
/// GetIconInfo, the DC or GetDIBits failing -- each releasing every icon, bitmap, DC and process
/// handle it was given. The process's path read, including the long-path retry. No real icon or
/// process is touched.

#include "Platform/IProcessIconSource.h"
#include "Platform/Windows/WindowsProcessIconSource.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

/// Fake handles: small integers, never dereferenced.
template<typename Handle> [[nodiscard]] Handle fakeHandle(std::uintptr_t value)
{
    return reinterpret_cast<Handle>(value); // NOLINT(performance-no-int-to-ptr): a fake handle
}

[[nodiscard]] HANDLE processHandle()
{
    return fakeHandle<HANDLE>(0x10);
}
[[nodiscard]] HICON iconHandle()
{
    return fakeHandle<HICON>(0x20);
}
[[nodiscard]] HBITMAP colorBitmap()
{
    return fakeHandle<HBITMAP>(0x30);
}
[[nodiscard]] HBITMAP maskBitmap()
{
    return fakeHandle<HBITMAP>(0x40);
}
[[nodiscard]] HDC dcHandle()
{
    return fakeHandle<HDC>(0x50);
}

/// What the fakes answer and what they were asked. Reset per test.
struct FakeIconApi
{
    bool openSucceeds = true;
    std::wstring imagePath = L"C:\\Program Files\\App\\app.exe";
    DWORD imagePathNeeds = 0; ///< A buffer at least this large is needed; 0: any
    HRESULT extractResult = S_OK;
    bool getIconInfoSucceeds = true;
    bool monochrome = false;
    int width = 2;
    int height = 2;
    bool dcSucceeds = true;
    std::map<HBITMAP, std::vector<std::uint8_t>> bits; ///< By bitmap; absent: GetDIBits fails

    int processesOpened = 0;
    int processesClosed = 0;
    int iconsExtracted = 0;
    std::vector<HICON> iconsDestroyed;
    std::vector<HGDIOBJ> objectsDeleted;
    int dcsCreated = 0;
    int dcsDeleted = 0;
    std::vector<DWORD> pathBufferSizes;
    UINT requestedSize = 0;
};

FakeIconApi& api()
{
    static FakeIconApi instance;
    return instance;
}

HANDLE WINAPI fakeOpenProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
{
    if (!api().openSucceeds)
    {
        return nullptr;
    }
    ++api().processesOpened;
    return processHandle();
}

BOOL WINAPI fakeQueryImageName(HANDLE process, DWORD /*flags*/, LPWSTR buffer, PDWORD size)
{
    EXPECT_EQ(process, processHandle());
    api().pathBufferSizes.push_back(*size);
    const auto needed = static_cast<DWORD>(api().imagePath.size() + 1);
    if (*size < std::max(needed, api().imagePathNeeds))
    {
        return FALSE;
    }
    std::memcpy(buffer, api().imagePath.c_str(), needed * sizeof(wchar_t));
    *size = needed - 1;
    return TRUE;
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, processHandle());
    ++api().processesClosed;
    return TRUE;
}

HRESULT WINAPI fakeExtractIcon(LPCWSTR /*file*/, int index, UINT /*flags*/, HICON* large, HICON* small, UINT size)
{
    EXPECT_EQ(index, 0);
    EXPECT_EQ(small, nullptr);
    api().requestedSize = size;
    if (api().extractResult == S_OK)
    {
        ++api().iconsExtracted;
        *large = iconHandle();
    }
    return api().extractResult;
}

BOOL WINAPI fakeDestroyIcon(HICON icon)
{
    api().iconsDestroyed.push_back(icon);
    return TRUE;
}

BOOL WINAPI fakeGetIconInfo(HICON icon, PICONINFO info)
{
    EXPECT_EQ(icon, iconHandle());
    if (!api().getIconInfoSucceeds)
    {
        return FALSE;
    }
    info->fIcon = TRUE;
    info->hbmColor = api().monochrome ? nullptr : colorBitmap();
    info->hbmMask = maskBitmap();
    return TRUE;
}

int WINAPI fakeGetObject(HANDLE object, int size, LPVOID out)
{
    EXPECT_EQ(size, static_cast<int>(sizeof(BITMAP)));
    BITMAP bitmap{};
    bitmap.bmWidth = api().width;
    // A monochrome icon's mask holds the AND and XOR masks, one above the other.
    bitmap.bmHeight = (object == maskBitmap() && api().monochrome) ? api().height * 2 : api().height;
    std::memcpy(out, &bitmap, sizeof(bitmap));
    return size;
}

HDC WINAPI fakeCreateDC(HDC /*dc*/)
{
    if (!api().dcSucceeds)
    {
        return nullptr;
    }
    ++api().dcsCreated;
    return dcHandle();
}

BOOL WINAPI fakeDeleteDC(HDC dc)
{
    EXPECT_EQ(dc, dcHandle());
    ++api().dcsDeleted;
    return TRUE;
}

int WINAPI fakeGetDIBits(HDC dc, HBITMAP bitmap, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO info, UINT usage)
{
    EXPECT_EQ(dc, dcHandle());
    EXPECT_EQ(start, 0U);
    EXPECT_EQ(usage, static_cast<UINT>(DIB_RGB_COLORS));
    EXPECT_EQ(info->bmiHeader.biBitCount, 32);
    EXPECT_EQ(info->bmiHeader.biHeight, -static_cast<LONG>(lines)); // Top-down
    const auto it = api().bits.find(bitmap);
    if (it == api().bits.end())
    {
        return 0;
    }
    const std::size_t bytes = static_cast<std::size_t>(info->bmiHeader.biWidth) * lines * 4U;
    EXPECT_EQ(it->second.size(), bytes);
    std::memcpy(bits, it->second.data(), std::min(bytes, it->second.size()));
    return static_cast<int>(lines);
}

BOOL WINAPI fakeDeleteObject(HGDIOBJ object)
{
    api().objectsDeleted.push_back(object);
    return TRUE;
}

[[nodiscard]] Windows::ProcessIconFunctions fakes()
{
    return Windows::ProcessIconFunctions{
        .openProcess = &fakeOpenProcess,
        .queryFullProcessImageName = &fakeQueryImageName,
        .closeHandle = &fakeCloseHandle,
        .extractIcon = &fakeExtractIcon,
        .destroyIcon = &fakeDestroyIcon,
        .getIconInfo = &fakeGetIconInfo,
        .getObject = &fakeGetObject,
        .createCompatibleDC = &fakeCreateDC,
        .deleteDC = &fakeDeleteDC,
        .getDIBits = &fakeGetDIBits,
        .deleteObject = &fakeDeleteObject,
    };
}

/// @p count BGRX pixels, each b, g, r, x.
[[nodiscard]] std::vector<std::uint8_t> pixels(int count, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t x)
{
    std::vector<std::uint8_t> bytes;
    int i = 0;
    while (i < count)
    {
        bytes.insert(bytes.end(), {b, g, r, x});
        ++i;
    }
    return bytes;
}

class WindowsProcessIconSourceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        api() = FakeIconApi{};
    }

    /// Every icon, bitmap and DC the calls were given was released, exactly once.
    static void expectEverythingReleased(bool bitmaps)
    {
        EXPECT_EQ(api().iconsDestroyed, std::vector<HICON>(static_cast<std::size_t>(api().iconsExtracted), iconHandle()));
        EXPECT_EQ(api().dcsDeleted, api().dcsCreated);
        if (bitmaps)
        {
            std::vector<HGDIOBJ> expected{maskBitmap()};
            if (!api().monochrome)
            {
                expected.insert(expected.begin(), colorBitmap());
            }
            std::vector<HGDIOBJ> deleted = api().objectsDeleted;
            std::ranges::sort(deleted);
            std::ranges::sort(expected);
            EXPECT_EQ(deleted, expected);
        }
        else
        {
            EXPECT_TRUE(api().objectsDeleted.empty());
        }
    }

    WindowsProcessIconSource m_Source{fakes()};
};

TEST_F(WindowsProcessIconSourceTest, ReadsAnIconWithAlpha)
{
    api().bits[colorBitmap()] = pixels(4, 10, 20, 30, 200);
    api().bits[maskBitmap()] = pixels(4, 255, 255, 255, 0);
    const auto icon = m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 16);
    ASSERT_TRUE(icon.has_value());
    EXPECT_EQ(icon->width, 2);
    EXPECT_EQ(icon->height, 2);
    EXPECT_EQ(icon->rgba, pixels(4, 30, 20, 10, 200)); // RGBA, straight alpha kept
    EXPECT_EQ(api().requestedSize, 16U);
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, TakesAnOldIconsAlphaFromItsMask)
{
    api().bits[colorBitmap()] = pixels(4, 10, 20, 30, 0); // No alpha anywhere
    std::vector<std::uint8_t> mask = pixels(2, 0, 0, 0, 0);
    const std::vector<std::uint8_t> transparent = pixels(2, 255, 255, 255, 0);
    mask.insert(mask.end(), transparent.begin(), transparent.end());
    api().bits[maskBitmap()] = mask;

    const auto icon = m_Source.loadIcon("C:\\App\\old.exe", "old.exe", 16);
    ASSERT_TRUE(icon.has_value());
    std::vector<std::uint8_t> expected = pixels(2, 30, 20, 10, 255);
    const std::vector<std::uint8_t> hidden = pixels(2, 30, 20, 10, 0);
    expected.insert(expected.end(), hidden.begin(), hidden.end());
    EXPECT_EQ(icon->rgba, expected);
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, AnUnreadableMaskLeavesAnOldIconOpaque)
{
    api().bits[colorBitmap()] = pixels(4, 10, 20, 30, 0); // The mask's GetDIBits fails
    const auto icon = m_Source.loadIcon("C:\\App\\old.exe", "old.exe", 16);
    ASSERT_TRUE(icon.has_value());
    EXPECT_EQ(icon->rgba, pixels(4, 30, 20, 10, 255));
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, ReadsAMonochromeIcon)
{
    api().monochrome = true;
    std::vector<std::uint8_t> mask = pixels(4, 0, 0, 0, 0);                // AND: all shown
    const std::vector<std::uint8_t> xorMask = pixels(4, 255, 255, 255, 0); // XOR: all white
    mask.insert(mask.end(), xorMask.begin(), xorMask.end());
    api().bits[maskBitmap()] = mask;
    const auto icon = m_Source.loadIcon("C:\\App\\mono.exe", "mono.exe", 16);
    ASSERT_TRUE(icon.has_value());
    EXPECT_EQ(icon->height, 2);
    EXPECT_EQ(icon->rgba, pixels(4, 255, 255, 255, 255));
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, AFileWithoutAnIconHasNone)
{
    api().extractResult = S_FALSE;
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\noicon.exe", "noicon.exe", 16).has_value());
    expectEverythingReleased(false);
}

TEST_F(WindowsProcessIconSourceTest, AFailedGetIconInfoStillDestroysTheIcon)
{
    api().getIconInfoSucceeds = false;
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 16).has_value());
    EXPECT_EQ(api().iconsDestroyed.size(), 1U);
    expectEverythingReleased(false);
}

TEST_F(WindowsProcessIconSourceTest, AFailedDCReleasesTheIconAndItsBitmaps)
{
    api().dcSucceeds = false;
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 16).has_value());
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, AFailedColorReadReleasesEverything)
{
    api().bits[maskBitmap()] = pixels(4, 0, 0, 0, 0); // The colour bitmap's GetDIBits fails
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 16).has_value());
    expectEverythingReleased(true);
    EXPECT_EQ(api().dcsDeleted, 1);
}

TEST_F(WindowsProcessIconSourceTest, AnOversizedBitmapIsRefused)
{
    api().width = 512;
    api().height = 512;
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 16).has_value());
    expectEverythingReleased(true);
}

TEST_F(WindowsProcessIconSourceTest, RefusesAnEmptyPathOrAnUnreasonableSizeWithoutCallingWindows)
{
    EXPECT_FALSE(m_Source.loadIcon("", "System", 16).has_value());
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 0).has_value());
    EXPECT_FALSE(m_Source.loadIcon("C:\\App\\app.exe", "app.exe", 257).has_value());
    EXPECT_EQ(api().requestedSize, 0U);
}

TEST_F(WindowsProcessIconSourceTest, ReadsAProcesssExecutableAndClosesIt)
{
    EXPECT_EQ(m_Source.executablePath(1234), "C:\\Program Files\\App\\app.exe");
    EXPECT_EQ(api().processesOpened, 1);
    EXPECT_EQ(api().processesClosed, 1);
}

TEST_F(WindowsProcessIconSourceTest, RetriesALongPathWithTheLongPathBuffer)
{
    api().imagePathNeeds = 1000;
    EXPECT_EQ(m_Source.executablePath(1234), "C:\\Program Files\\App\\app.exe");
    EXPECT_EQ(api().pathBufferSizes, (std::vector<DWORD>{MAX_PATH, 32767}));
    EXPECT_EQ(api().processesClosed, 1);
}

TEST_F(WindowsProcessIconSourceTest, AProcessThatCantBeOpenedOrReadHasNoPath)
{
    api().openSucceeds = false;
    EXPECT_EQ(m_Source.executablePath(1234), "");
    EXPECT_EQ(m_Source.executablePath(0), ""); // The Idle "process": not even opened
    EXPECT_EQ(api().processesOpened, 0);

    api().openSucceeds = true;
    api().imagePathNeeds = 40000; // Longer than any buffer offered
    EXPECT_EQ(m_Source.executablePath(1234), "");
    EXPECT_EQ(api().processesClosed, 1);
}

TEST_F(WindowsProcessIconSourceTest, FoldsCase)
{
    EXPECT_TRUE(m_Source.caseInsensitivePaths());
}

} // namespace
} // namespace Platform
