#pragma once

// The Windows side of NativeId (#4304): each probe snapshot's exact native identities, kept here in UTF-16 under
// opaque ids, so an action resolves the id back to the name the probe read rather than rebuilding it from lossy
// display UTF-8. This is the one place the wide names are kept between a probe and its actions: Domain and the UI
// hold only the id (R1, #4299). The startup and service probes publish into a table their actions share
// (sharedStartupIds(), sharedServiceIds()); a test gives both its own.

#include "Platform/IStartupProbe.h"
#include "Platform/NativeId.h"
#include "WinString.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

/// A new id, never issued before by any table in this process: an id from one table names nothing in another.
[[nodiscard]] inline NativeId nextNativeId() noexcept
{
    static std::atomic<std::uint64_t> last{0};
    return static_cast<NativeId>(last.fetch_add(1, std::memory_order_relaxed) + 1);
}

/// The native identities one probe listed last, each under its id. publish() runs on the probe's sampler thread and
/// resolve() on an action's worker, so both lock.
template<typename Key> class NativeIdTable
{
  public:
    /// Makes @p keys, one probe snapshot's identities, the listed ones, and gives each its id, in order. A key the last
    /// snapshot listed keeps its id, so a row's id stays the same while its entry is listed; a new one gets a fresh
    /// id. The ids of keys no longer listed are retired: resolve() refuses them from now on, and they are never
    /// issued again. Equal keys get the same id.
    [[nodiscard]] std::vector<NativeId> publish(std::span<const Key> keys)
    {
        std::vector<NativeId> ids;
        ids.reserve(keys.size());
        std::map<Key, NativeId> listed;
        const std::scoped_lock lock(m_Mutex);
        for (const Key& key : keys)
        {
            if (const auto known = listed.find(key); known != listed.end())
            {
                ids.push_back(known->second);
                continue;
            }
            const auto previous = m_Ids.find(key);
            const NativeId id = (previous != m_Ids.end()) ? previous->second : nextNativeId();
            listed.emplace(key, id);
            ids.push_back(id);
        }
        m_Ids = std::move(listed);
        m_Keys.clear();
        for (const auto& [key, id] : m_Ids)
        {
            m_Keys.emplace(id, key);
        }
        return ids;
    }

    /// The identity @p id was issued for, while its entry is still listed; nullopt for None, a retired id, or one this
    /// table never issued.
    [[nodiscard]] std::optional<Key> resolve(NativeId id) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Keys.find(id);
        return (found != m_Keys.end()) ? std::optional<Key>(found->second) : std::nullopt;
    }

  private:
    mutable std::mutex m_Mutex;
    std::map<Key, NativeId> m_Ids;
    std::map<NativeId, Key> m_Keys;
};

/// A startup entry's native identity: where it is registered, and the exact UTF-16 name it is registered under there.
/// Wide strings, against R1's rule, because this is the native form D1 keeps for actions; it never leaves Platform.
struct StartupNativeKey
{
    StartupLocation location = StartupLocation::RunUser;
    /// The Run or RunOnce value name, the Startup folder file's name ("Tool.lnk"), a packaged startup task's TaskId,
    /// or a scheduled task's full path ("\Vendor\Updater").
    std::wstring name;
    /// A packaged startup task's package family name; empty for any other entry.
    std::wstring packageFamily;

    [[nodiscard]] auto operator<=>(const StartupNativeKey&) const = default;
};

using StartupIdTable = NativeIdTable<StartupNativeKey>;
/// A service's exact UTF-16 short name, the name OpenServiceW takes.
using ServiceIdTable = NativeIdTable<std::wstring>;

/// The table the Startup tab's probe and actions share (Factory: makeStartupProbe(), makeStartupActions()).
[[nodiscard]] inline std::shared_ptr<StartupIdTable> sharedStartupIds()
{
    static const auto table = std::make_shared<StartupIdTable>();
    return table;
}

/// The table the Services tab's probe and actions share (Factory: makeServiceProbe(), makeServiceActions()).
[[nodiscard]] inline std::shared_ptr<ServiceIdTable> sharedServiceIds()
{
    static const auto table = std::make_shared<ServiceIdTable>();
    return table;
}

/// Whether @p name holds a NUL: a Win32 call taking it as a NUL-terminated string (RegSetValueExW, OpenServiceW,
/// RegOpenKeyExW) would act on the part before it, another name, so an action on it is refused.
[[nodiscard]] constexpr bool hasEmbeddedNul(std::wstring_view name) noexcept
{
    return name.contains(L'\0');
}

/// For a caller that holds only a UTF-8 name, no id (a test, or a name typed in): the name's UTF-16 form, when the
/// UTF-8 is valid and can be the exact name. Nullopt when it isn't valid, or when it holds U+FFFD or U+2400, which
/// display text puts in place of what it couldn't show (a lone surrogate, a NUL): then the name is lossy, and
/// converting it back would name another entry or none.
[[nodiscard]] inline std::optional<std::wstring> exactWideName(std::string_view utf8)
{
    constexpr std::string_view REPLACEMENT = "\xEF\xBF\xBD"; // U+FFFD
    constexpr std::string_view NUL_SYMBOL = "\xE2\x90\x80";  // U+2400
    if (utf8.empty() || utf8.contains(REPLACEMENT) || utf8.contains(NUL_SYMBOL))
    {
        return std::nullopt;
    }
    std::wstring wide = WinString::utf8ToWide(utf8);
    if (wide.empty() || hasEmbeddedNul(wide))
    {
        return std::nullopt;
    }
    return wide;
}

} // namespace Platform::Windows
