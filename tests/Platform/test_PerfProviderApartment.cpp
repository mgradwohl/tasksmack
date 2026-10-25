/// @file test_PerfProviderApartment.cpp
/// @brief Tests for Platform::Windows::keepComForPerfProviders (#1898)
///
/// PDH's perf providers (bitsperf.dll) hold COM objects made in the apartment of the thread that first
/// connected PDH. When that apartment ended, COM unloaded their proxy DLL, and the next PDH reconnect
/// crashed in bitsperf's CPerfMon::~CPerfMon. Its own ctest entry (one process per suite) means no other
/// test's COM reference keeps the apartment alive here.

#include <gtest/gtest.h>

#if defined(_WIN32)

#include "Platform/Windows/PerfProviderApartment.h"
#include "Platform/Windows/WindowsDiskProbe.h"

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

#include <thread>

namespace Platform
{
namespace
{

// The sequence two shells in one process made: the UI thread's STA (SDL's) is where PDH first connects,
// the apartment's own CoUninitialize (SDL_Quit) follows, then a new disk probe reconnects PDH.
TEST(PerfProviderApartmentTest, ADiskProbeAfterItsThreadsApartmentEndsDoesNotCrash)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ASSERT_TRUE(SUCCEEDED(init)) << "the test thread must be able to enter an STA";
    {
        const WindowsDiskProbe first; // PdhEnumObjects connects PDH, loading the providers, in this STA
    }
    CoUninitialize();

    // The probe kept the apartment: the thread is still in its STA.
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    EXPECT_TRUE(SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA))
        << "the disk probe kept its thread's apartment";

    // PdhEnumObjects with bRefresh = TRUE closes the providers, which release their COM objects: this
    // crashed when the apartment, and with it BitsProxy.dll, was gone.
    const WindowsDiskProbe second;
    SUCCEED();
}

TEST(PerfProviderApartmentTest, AThreadWithoutComGetsNoApartmentOfItsOwn)
{
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    HRESULT result = E_FAIL;
    std::thread(
        [&]
        {
            Windows::keepComForPerfProviders();
            result = CoGetApartmentType(&type, &qualifier);
        })
        .join();
    // The process's MTA is kept, so the thread is in it implicitly -- not in an STA, and not initialised.
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(type, APTTYPE_MTA);
    EXPECT_EQ(qualifier, APTTYPEQUALIFIER_IMPLICIT_MTA);
}

} // namespace
} // namespace Platform

#endif // _WIN32
