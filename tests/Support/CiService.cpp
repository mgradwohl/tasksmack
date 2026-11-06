// TaskSmackCiService.exe (#3490): the throwaway service the CI real-system service tests install, then start, stop,
// restart and reconfigure through the real WindowsServiceActions. It does nothing but report itself running and wait
// for a stop, at most ten minutes, so a service the test somehow left behind ends on its own. Started any other way
// than by the service manager, StartServiceCtrlDispatcherW fails and it exits 1 at once.

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsvc.h>
// clang-format on

#include <array>

namespace
{

/// The longest it runs before it stops itself.
constexpr DWORD MAX_RUN_MS = 10U * 60U * 1000U;
/// What it tells the service manager a start or a stop may take.
constexpr DWORD PENDING_HINT_MS = 5'000;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) - the service manager's callbacks take no context
SERVICE_STATUS_HANDLE g_Status = nullptr;
HANDLE g_Stop = nullptr;
DWORD g_CheckPoint = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Tells the service manager it is in @p state. A pending state gets a rising check point and a wait hint.
void report(DWORD state)
{
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0U;
    status.dwWin32ExitCode = NO_ERROR;
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    status.dwCheckPoint = pending ? ++g_CheckPoint : 0U;
    status.dwWaitHint = pending ? PENDING_HINT_MS : 0U;
    SetServiceStatus(g_Status, &status);
}

DWORD WINAPI handler(DWORD control, DWORD /*eventType*/, LPVOID /*eventData*/, LPVOID /*context*/)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING);
        SetEvent(g_Stop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    // An own-process service: the name isn't checked, since this process runs only the one.
    g_Status = RegisterServiceCtrlHandlerExW(L"", &handler, nullptr);
    if (g_Status == nullptr)
    {
        return;
    }
    report(SERVICE_START_PENDING);
    g_Stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_Stop == nullptr)
    {
        report(SERVICE_STOPPED);
        return;
    }
    report(SERVICE_RUNNING);
    WaitForSingleObject(g_Stop, MAX_RUN_MS);
    CloseHandle(g_Stop);
    report(SERVICE_STOPPED);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage, readability-identifier-naming) - the Windows entry point
int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, LPWSTR /*commandLine*/, int /*show*/)
{
    std::array<wchar_t, 1> name{};
    // The service, then the all-null entry that ends the table.
    std::array<SERVICE_TABLE_ENTRYW, 2> table{};
    table[0] = {.lpServiceName = name.data(), .lpServiceProc = &serviceMain};
    return StartServiceCtrlDispatcherW(table.data()) != FALSE ? 0 : 1;
}
