#pragma once

// TaskSmack's own process identity, for the Processes table's batch actions (#804): a batch that
// includes TaskSmack itself says so in its confirmation and acts on it last.

#include <cstdint>

namespace Platform
{

/// The PID of the calling process (getpid() / GetCurrentProcessId()).
[[nodiscard]] std::int32_t currentProcessId() noexcept;

} // namespace Platform
