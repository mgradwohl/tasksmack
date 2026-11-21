/// @file test_WindowsFinalPath.cpp
/// @brief The final-path reader the elevated checks share (#4368): each \\?\ prefix policy over each kind of path, and
/// each buffer sizing over a stand-in GetFinalPathNameByHandleW that records the sizes it is asked with. Windows only.

#include "Platform/Windows/WindowsFinalPath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// One path, and what each prefix policy makes of it.
struct PrefixRow
{
    std::wstring_view path;
    std::wstring_view kept;
    std::wstring_view keptForUnc;
    std::wstring_view removed;
};

TEST(WindowsFinalPathTest, EachPrefixPolicyKeepsOrDropsOnlyItsOwnPrefix)
{
    const std::vector<PrefixRow> rows{
        {.path = LR"(\\?\C:\dir)", .kept = LR"(\\?\C:\dir)", .keptForUnc = LR"(C:\dir)", .removed = LR"(C:\dir)"},
        {
            .path = LR"(\\?\UNC\server\share\dir)",
            .kept = LR"(\\?\UNC\server\share\dir)",
            .keptForUnc = LR"(\\?\UNC\server\share\dir)",
            .removed = LR"(\\server\share\dir)",
        },
        {.path = LR"(C:\dir)", .kept = LR"(C:\dir)", .keptForUnc = LR"(C:\dir)", .removed = LR"(C:\dir)"},
        // An NT path (VOLUME_NAME_NT) has no \\?\ to drop.
        {
            .path = LR"(\Device\HarddiskVolume3\dir)",
            .kept = LR"(\Device\HarddiskVolume3\dir)",
            .keptForUnc = LR"(\Device\HarddiskVolume3\dir)",
            .removed = LR"(\Device\HarddiskVolume3\dir)",
        },
        // The prefixes match exactly, case included, as Windows writes them: a lower-case "unc" isn't the UNC prefix.
        {
            .path = LR"(\\?\unc\server\share)",
            .kept = LR"(\\?\unc\server\share)",
            .keptForUnc = LR"(unc\server\share)",
            .removed = LR"(unc\server\share)",
        },
        {.path = L"", .kept = L"", .keptForUnc = L"", .removed = L""},
    };
    for (const PrefixRow& row : rows)
    {
        const std::wstring path(row.path);
        EXPECT_EQ(withFinalPathPrefix(path, FinalPathPrefix::Kept), std::wstring(row.kept));
        EXPECT_EQ(withFinalPathPrefix(path, FinalPathPrefix::KeptForUnc), std::wstring(row.keptForUnc));
        EXPECT_EQ(withFinalPathPrefix(path, FinalPathPrefix::Removed), std::wstring(row.removed));
    }
}

/// What the stand-in answers, and the buffer sizes it was asked with.
struct FinalPathFake
{
    std::wstring path{};        // NOLINT(readability-redundant-member-init) - designated initializers leave it out; written when it fits
    bool neverFits = false;     ///< Answer one more than the size given, every time
    bool fails = false;         ///< Answer 0
    std::vector<DWORD> sizes{}; // NOLINT(readability-redundant-member-init) - designated initializers leave it out
};

FinalPathFake g_Fake; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - the stand-in's, reset per test

/// GetFinalPathNameByHandleW as g_Fake says: the length written when it fits, else the length needed with the NUL.
DWORD WINAPI fakeFinalPath(HANDLE /*handle*/, LPWSTR buffer, DWORD size, DWORD /*flags*/)
{
    g_Fake.sizes.push_back(size);
    if (g_Fake.fails)
    {
        return 0;
    }
    if (g_Fake.neverFits)
    {
        return size + 1;
    }
    if (size <= g_Fake.path.size())
    {
        return static_cast<DWORD>(g_Fake.path.size() + 1);
    }
    const std::span<wchar_t> out(buffer, size);
    std::ranges::copy(g_Fake.path, out.begin());
    out[g_Fake.path.size()] = L'\0';
    return static_cast<DWORD>(g_Fake.path.size());
}

constexpr DWORD DOS = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;

[[nodiscard]] std::optional<std::wstring> read(FinalPathBuffer buffer, FinalPathPrefix prefix = FinalPathPrefix::Kept)
{
    return readFinalPath(nullptr, &fakeFinalPath, DOS, buffer, prefix);
}

// A path that fits MAX_PATH is read once, whichever the sizing, with its prefix as asked.
TEST(WindowsFinalPathTest, AShortPathIsReadOnce)
{
    for (const FinalPathBuffer buffer : {FinalPathBuffer::AsAnswered, FinalPathBuffer::Longest})
    {
        g_Fake = {.path = LR"(\\?\C:\TaskSmack)"};
        EXPECT_EQ(read(buffer, FinalPathPrefix::KeptForUnc), std::optional<std::wstring>(LR"(C:\TaskSmack)"));
        EXPECT_EQ(g_Fake.sizes, std::vector<DWORD>{MAX_PATH});
    }
}

// A longer path: AsAnswered asks again at the size the first answer said, Longest at LONGEST_PATH.
TEST(WindowsFinalPathTest, ALongPathIsReadAgainAtEachSizingsOwnSize)
{
    const std::wstring longPath = LR"(\\?\C:\)" + std::wstring(300, L'x');
    g_Fake = {.path = longPath};
    EXPECT_EQ(read(FinalPathBuffer::AsAnswered), std::optional(longPath));
    EXPECT_EQ(g_Fake.sizes, (std::vector<DWORD>{MAX_PATH, static_cast<DWORD>(longPath.size() + 1)}));

    g_Fake = {.path = longPath};
    EXPECT_EQ(read(FinalPathBuffer::Longest), std::optional(longPath));
    EXPECT_EQ(g_Fake.sizes, (std::vector<DWORD>{MAX_PATH, LONGEST_PATH}));
}

// A path that never fits, or a call that fails, is no path: never a guess, never a loop.
TEST(WindowsFinalPathTest, APathThatNeverFitsOrCantBeReadIsNoPath)
{
    g_Fake = {.neverFits = true};
    EXPECT_EQ(read(FinalPathBuffer::AsAnswered), std::nullopt);
    EXPECT_EQ(g_Fake.sizes, (std::vector<DWORD>{MAX_PATH, MAX_PATH + 1}));

    g_Fake = {.neverFits = true};
    EXPECT_EQ(read(FinalPathBuffer::Longest), std::nullopt);
    EXPECT_EQ(g_Fake.sizes, (std::vector<DWORD>{MAX_PATH, LONGEST_PATH}));

    for (const FinalPathBuffer buffer : {FinalPathBuffer::AsAnswered, FinalPathBuffer::Longest})
    {
        g_Fake = {.fails = true};
        EXPECT_EQ(read(buffer), std::nullopt);
        EXPECT_EQ(g_Fake.sizes, std::vector<DWORD>{MAX_PATH}) << "a failure isn't asked again";
    }
}

/// GetModuleFileNameW as g_Fake says: a path that doesn't fit comes back truncated, with the size given.
DWORD WINAPI fakeModuleFileName(HMODULE /*module*/, LPWSTR buffer, DWORD size)
{
    g_Fake.sizes.push_back(size);
    if (g_Fake.fails)
    {
        return 0;
    }
    const std::span<wchar_t> out(buffer, size);
    const std::size_t written = std::min<std::size_t>(g_Fake.path.size(), size - 1);
    std::ranges::copy(std::wstring_view(g_Fake.path).substr(0, written), out.begin());
    out[written] = L'\0';
    return g_Fake.path.size() < size ? static_cast<DWORD>(written) : size;
}

// This process's image: MAX_PATH first, then the longest path, as BrokerHelper and Start at sign-in both read it.
TEST(WindowsFinalPathTest, TheImagePathIsReadAtMaxPathThenTheLongest)
{
    g_Fake = {.path = LR"(C:\Program Files\TaskSmack\TaskSmack.exe)"};
    EXPECT_EQ(moduleImagePath(&fakeModuleFileName), std::optional(g_Fake.path));
    EXPECT_EQ(g_Fake.sizes, std::vector<DWORD>{MAX_PATH});

    const std::wstring longPath = LR"(C:\)" + std::wstring(400, L'x') + L"\\TaskSmack.exe";
    g_Fake = {.path = longPath};
    EXPECT_EQ(moduleImagePath(&fakeModuleFileName), std::optional(longPath));
    EXPECT_EQ(g_Fake.sizes, (std::vector<DWORD>{MAX_PATH, LONGEST_PATH}));

    g_Fake = {.fails = true};
    EXPECT_EQ(moduleImagePath(&fakeModuleFileName), std::nullopt);
}

} // namespace
} // namespace Platform::Windows
