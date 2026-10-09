#include "WindowsStartupActions.h"

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#pragma comment(lib, "advapi32.lib")

#include "WinString.h"
#include "WindowsStartupProbeMath.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Platform
{

namespace
{

namespace Math = Windows::StartupMath;

/// Closes a key through the injected RegCloseKey: advapi32's, or a test's fake for its fake keys.
struct InjectedKeyCloser
{
    Windows::StartupRegistryFunctions::RegCloseKeyFn close = &::RegCloseKey;

    void operator()(HKEY key) const noexcept
    {
        close(key);
    }
};

using InjectableKey = std::unique_ptr<std::remove_pointer_t<HKEY>, InjectedKeyCloser>;

[[nodiscard]] StartupActionResult failure(LSTATUS status)
{
    return StartupActionResult::failed(Math::approvedErrorText(static_cast<std::uint32_t>(status)));
}

} // namespace

WindowsStartupActions::WindowsStartupActions(bool elevated, Windows::StartupRegistryFunctions api) : m_Elevated(elevated), m_Api(api)
{}

StartupActionCapabilities WindowsStartupActions::capabilities() const
{
    return {.canSetEnabled = true, .elevated = m_Elevated};
}

StartupActionResult WindowsStartupActions::setEnabled(const StartupEntry& entry, bool enabled)
{
    const auto approvedKey = Math::approvedKeyFor(entry.location);
    if (!approvedKey)
    {
        return StartupActionResult::failed("Run-once entries can't be enabled or disabled");
    }
    const std::wstring valueName = WinString::utf8ToWide(Math::approvedValueName(entry));
    if (valueName.empty())
    {
        return StartupActionResult::failed("The entry has no name to record its state under");
    }

    // Created when missing (a profile where nothing was ever disabled has no StartupApproved key yet).
    HKEY rawKey = nullptr;
    const LSTATUS opened = m_Api.createKey(approvedKey->machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                           approvedKey->subkey,
                                           0,
                                           nullptr,
                                           REG_OPTION_NON_VOLATILE,
                                           KEY_QUERY_VALUE | KEY_SET_VALUE,
                                           nullptr,
                                           &rawKey,
                                           nullptr);
    if (opened != ERROR_SUCCESS)
    {
        return failure(opened);
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = m_Api.closeKey});

    // The current value, kept but for the bytes the new state owns. A missing or non-binary value is
    // replaced by a standard one.
    std::vector<std::uint8_t> existing;
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS queried = m_Api.queryValue(key.get(), valueName.c_str(), nullptr, &type, nullptr, &size);
    if (queried == ERROR_SUCCESS && type == REG_BINARY && size > 0)
    {
        existing.resize(size);
        queried = m_Api.queryValue(key.get(), valueName.c_str(), nullptr, &type, existing.data(), &size);
        existing.resize(size);
    }
    if (queried != ERROR_SUCCESS && queried != ERROR_FILE_NOT_FOUND)
    {
        return failure(queried);
    }
    if (type != REG_BINARY)
    {
        existing.clear();
    }

    std::uint64_t disabledAt = 0;
    if (!enabled)
    {
        FILETIME now{};
        m_Api.now(&now);
        disabledAt = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
    }
    const std::vector<std::uint8_t> value = Math::encodeStartupApproved(existing, enabled, disabledAt);
    const LSTATUS written = m_Api.setValue(key.get(), valueName.c_str(), 0, REG_BINARY, value.data(), static_cast<DWORD>(value.size()));
    return written == ERROR_SUCCESS ? StartupActionResult::succeeded() : failure(written);
}

} // namespace Platform
