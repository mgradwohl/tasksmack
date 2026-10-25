#pragma once

// COM on the calling thread for the length of one scope: CoInitializeEx in the constructor, a matching
// CoUninitialize in the destructor only when that call succeeded (S_OK or S_FALSE). A thread that already
// has the other apartment (RPC_E_CHANGED_MODE) can still use COM; its own initialisation is left alone.

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

namespace Platform
{

class ComScope
{
  public:
    /// @param flags CoInitializeEx's: the apartment model and options.
    explicit ComScope(DWORD flags = static_cast<DWORD>(COINIT_APARTMENTTHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)) noexcept
        : m_Result(CoInitializeEx(nullptr, flags))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(m_Result))
        {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    /// Whether COM can be used on this thread.
    [[nodiscard]] bool usable() const noexcept
    {
        return SUCCEEDED(m_Result) || m_Result == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT m_Result;
};

} // namespace Platform
