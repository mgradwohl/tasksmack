// TaskSmackCiStub.exe (#3489): the idle program the CI real-system tests start, open, elevate and show in
// Explorer instead of a real one. A Windows (not console) program, so it opens no window. It does nothing but
// wait: for the milliseconds its one argument gives (at most two minutes, a minute by default), or until it is
// ended, which every test does in a scope guard. Exit status 0.
//
//   TaskSmackCiStub.exe [<milliseconds>]
//   TaskSmackCiStub.exe --hung-window [<milliseconds>]
//
// Phase 2 (#3490):
//   - With TASKSMACK_CI_STUB_RECORD set to a file's path, it first writes its own command line there (UTF-8):
//     what the Replace Task Manager test reads back, from the copy Windows starts in taskmgr.exe's place.
//   - --hung-window shows a small window, answers its messages for a moment, then stops answering them, its
//     main thread blocked on a mutex another of its threads holds: a window Windows calls Not responding
//     (IsHungAppWindow), and a wait chain that leads from that thread to the other.

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
#include <string>
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

/// The variable that names the file the stub writes its command line to.
constexpr const wchar_t* RECORD_VARIABLE = L"TASKSMACK_CI_STUB_RECORD";
/// The argument that asks for the hung window.
constexpr std::wstring_view HUNG_WINDOW_ARGUMENT = L"--hung-window";
/// The hung window's title, which the test finds it by.
constexpr const wchar_t* HUNG_WINDOW_TITLE = L"TaskSmackCiStub hung window";
/// How long the hung window answers its messages before it stops: long enough that Windows has seen it pump.
constexpr DWORD PUMP_MS = 500;

/// Writes this process's command line, as UTF-8, to the file TASKSMACK_CI_STUB_RECORD names, if it names one.
void recordCommandLine()
{
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetEnvironmentVariableW(RECORD_VARIABLE, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        return;
    }
    path.resize(length);
    const wchar_t* commandLine = GetCommandLineW();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, commandLine, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return;
    }
    std::string text(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, commandLine, -1, text.data(), bytes, nullptr, nullptr);
    text.pop_back(); // the NUL
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

/// What the mutex-holding thread is given: the mutex, an event it sets once it holds it, and how long to hold it.
struct HolderContext
{
    HANDLE mutex = nullptr;
    HANDLE held = nullptr;
    DWORD wait = DEFAULT_WAIT_MS;
};

DWORD WINAPI holdMutex(LPVOID param)
{
    const auto* context = static_cast<const HolderContext*>(param);
    WaitForSingleObject(context->mutex, INFINITE);
    SetEvent(context->held);
    Sleep(context->wait);
    ReleaseMutex(context->mutex);
    return 0;
}

/// --hung-window: the window, a moment of answering it, then the main thread blocked for @p wait.
int runHungWindow(HINSTANCE instance, DWORD wait)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"TaskSmackCiStubHungWindow";
    if (RegisterClassW(&windowClass) == 0)
    {
        return 1;
    }
    constexpr int SIZE = 200;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE,
                                  windowClass.lpszClassName,
                                  HUNG_WINDOW_TITLE,
                                  WS_OVERLAPPEDWINDOW,
                                  0,
                                  0,
                                  SIZE,
                                  SIZE,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  nullptr);
    if (window == nullptr)
    {
        return 1;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);

    HolderContext context{
        .mutex = CreateMutexW(nullptr, FALSE, nullptr), .held = CreateEventW(nullptr, TRUE, FALSE, nullptr), .wait = wait};
    if (context.mutex == nullptr || context.held == nullptr)
    {
        return 1;
    }
    HANDLE holder = CreateThread(nullptr, 0, &holdMutex, &context, 0, nullptr);
    if (holder == nullptr)
    {
        return 1;
    }
    WaitForSingleObject(context.held, INFINITE);

    // Answer the window for a moment, so Windows has seen this thread pump (a window that never has is still
    // starting up, and isn't called hung), then stop.
    const ULONGLONG pumpUntil = GetTickCount64() + PUMP_MS;
    while (GetTickCount64() < pumpUntil)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
    WaitForSingleObject(context.mutex, wait); // blocked: the window's messages go unanswered
    ReleaseMutex(context.mutex);
    WaitForSingleObject(holder, INFINITE);
    CloseHandle(holder);
    CloseHandle(context.held);
    CloseHandle(context.mutex);
    DestroyWindow(window);
    return 0;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage, readability-identifier-naming) - the Windows entry point
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, LPWSTR /*commandLine*/, int /*show*/)
{
    recordCommandLine();
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    DWORD wait = DEFAULT_WAIT_MS;
    bool hungWindow = false;
    if (argv != nullptr && count >= 2)
    {
        const std::span<LPWSTR> arguments(argv, static_cast<std::size_t>(count));
        hungWindow = std::wstring_view(arguments[1]) == HUNG_WINDOW_ARGUMENT;
        if (!hungWindow)
        {
            wait = waitOf(arguments[1]);
        }
        else if (count >= 3)
        {
            wait = waitOf(arguments[2]);
        }
    }
    LocalFree(static_cast<HLOCAL>(argv));
    if (hungWindow)
    {
        return runHungWindow(instance, wait);
    }
    Sleep(wait);
    return 0;
}
