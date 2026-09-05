/// @file test_WindowsProcAddress.cpp
/// @brief Unit tests for Platform::Windows::getProcAddress<T>
///
/// getProcAddress<T> is a thin, pure wrapper around GetProcAddress: it needs no fake
/// hardware, only a real, always-loaded module (kernel32.dll) to resolve a well-known
/// symbol against.

#ifdef _WIN32

#include "Platform/Windows/WindowsProcAddress.h"

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

namespace Platform::Windows
{
namespace
{

using GetCurrentProcessIdFn = DWORD(WINAPI*)();

TEST(GetProcAddressTest, NullModuleReturnsNullptr)
{
    const auto fn = getProcAddress<GetCurrentProcessIdFn>(nullptr, "GetCurrentProcessId");
    EXPECT_EQ(fn, nullptr);
}

TEST(GetProcAddressTest, ValidModuleAndSymbolResolves)
{
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr) << "kernel32.dll should always be loaded in the process";

    const auto fn = getProcAddress<GetCurrentProcessIdFn>(kernel32, "GetCurrentProcessId");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), GetCurrentProcessId());
}

TEST(GetProcAddressTest, ValidModuleWithBogusSymbolReturnsNullptr)
{
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);

    const auto fn = getProcAddress<GetCurrentProcessIdFn>(kernel32, "ThisSymbolDoesNotExistInKernel32");
    EXPECT_EQ(fn, nullptr);
}

} // namespace
} // namespace Platform::Windows

#endif // _WIN32
