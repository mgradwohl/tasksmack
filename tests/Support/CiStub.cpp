// TaskSmackCiStub.exe (#3489): the idle program the CI real-system tests start, open, elevate and show in
// Explorer instead of a real one. A Windows (not console) program, so it opens no window. It does nothing but
// wait: for the milliseconds its one argument gives (at most two minutes, a minute by default), or until it is
// ended, which every test does in a scope guard. Exit status 0.
//
//   TaskSmackCiStub.exe [<milliseconds>]

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace
{

/// How long it waits when no argument says, and the most any argument may make it wait.
constexpr DWORD DEFAULT_WAIT_MS = 60'000;
constexpr DWORD MAX_WAIT_MS = 120'000;

/// @p text as milliseconds, or DEFAULT_WAIT_MS when it isn't a plain decimal number.
[[nodiscard]] DWORD waitOf(std::wstring_view text)
{
    if (text.empty() || text.size() > 9)
    {
        return DEFAULT_WAIT_MS;
    }
    DWORD value = 0;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            return DEFAULT_WAIT_MS;
        }
        value = (value * 10U) + static_cast<DWORD>(c - L'0');
    }
    return std::min(value, MAX_WAIT_MS);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage, readability-identifier-naming) - the Windows entry point
int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, LPWSTR /*commandLine*/, int /*show*/)
{
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    DWORD wait = DEFAULT_WAIT_MS;
    if (argv != nullptr && count >= 2)
    {
        const std::span<LPWSTR> arguments(argv, static_cast<std::size_t>(count));
        wait = waitOf(arguments[1]);
    }
    LocalFree(static_cast<HLOCAL>(argv));
    Sleep(wait);
    return 0;
}
