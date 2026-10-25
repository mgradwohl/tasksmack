#include "PerfProviderApartment.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
// clang-format on

#pragma comment(lib, "ole32.lib")

namespace Platform::Windows
{

void keepComForPerfProviders() noexcept
{
    // Once per process; the cookie is never passed to CoDecrementMTAUsage.
    [[maybe_unused]] static const bool keptMta = []
    {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));
    }();

    // Once per thread that is in an STA; the reference is never released.
    thread_local bool keptSta = false;
    if (keptSta)
    {
        return;
    }
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    if (FAILED(CoGetApartmentType(&type, &qualifier)) || (type != APTTYPE_STA && type != APTTYPE_MAINSTA))
    {
        return;
    }
    // S_FALSE: the thread was already in an STA, as checked, and now holds one more reference.
    keptSta = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

} // namespace Platform::Windows
