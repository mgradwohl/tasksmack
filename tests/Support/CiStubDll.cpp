// TaskSmackCiStubDll.dll (#3897): a DLL with no exports whose DllMain refuses to load, which the CI real-system app
// test copies beside a scratch TaskSmack.exe as version.dll, a DLL TaskSmack imports that isn't a KnownDLL. Linked
// with /DEPENDENTLOADFLAG:0x800, TaskSmack.exe takes System32's and never this one; if the loader took this one, the
// import would find none of its functions, and its DllMain would fail the start anyway, so TaskSmack wouldn't run.

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

// NOLINTNEXTLINE(misc-use-internal-linkage, readability-identifier-naming) - the DLL entry point the loader calls
extern "C" BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/)
{
    return reason == DLL_PROCESS_ATTACH ? FALSE : TRUE;
}
