#pragma once

#include <cstdint>

namespace Platform
{

/// IOCTL_DISK_PERFORMANCE's counter fields (BytesRead/BytesWritten/ReadTime/WriteTime) come
/// from the signed LARGE_INTEGER::QuadPart member. A buggy or virtualized disk driver
/// (VM/WSL passthrough, some RAID controllers) reporting a negative value would otherwise
/// wrap to near UINT64_MAX when cast directly to uint64_t, producing a spurious
/// multi-exabyte rate spike in StorageModel's next delta-based computation. Treat a
/// negative value as "no data this sample" rather than reinterpreting the sign bit as
/// magnitude.
[[nodiscard]] inline uint64_t clampNonNegativeQuadPart(int64_t value)
{
    return value < 0 ? 0ULL : static_cast<uint64_t>(value);
}

} // namespace Platform
