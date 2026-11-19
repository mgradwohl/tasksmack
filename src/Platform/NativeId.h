#pragma once

// The opaque id a probe gives a listed entry for its native identity (#4304). Windows names services, Run values,
// Startup folder files, packaged startup tasks and scheduled tasks in UTF-16 that may not be valid Unicode (a lone
// surrogate), or that may hold a NUL; the UTF-8 the UI shows of such a name has U+FFFD in its place (#4299, D1),
// so it can't name the entry back. The probe keeps the exact native name on the Platform side, under this id, and
// the matching actions resolve it there: the UI and Domain see only the UTF-8 display text and this id (R1), pass
// the id back for an action, and never rebuild an identity from the text.

#include <cstdint>

namespace Platform
{

/// One listed entry's native identity, as an opaque value: compared for equality only, never read. Ids are never
/// reused, so a stale one (its entry no longer listed) names nothing rather than another entry. None: the
/// platform gives no id (its names are exact, as Linux's unit names), or the entry wasn't listed by a probe.
// NOLINTNEXTLINE(performance-enum-size): its values are a 64-bit counter, not the one named here
enum class NativeId : std::uint64_t
{
    None = 0,
};

} // namespace Platform
