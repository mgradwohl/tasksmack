#include "WindowsHeldFileVersion.h"

#include "WindowsHandles.h"
#include "WindowsVersionResourceMath.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

// Structured exception handling (__try/__except) is a Microsoft extension, which -Wpedantic warns of: it is what catches
// an in-page error reading a mapped file, so it is allowed here, and only here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"

/// The VS_VERSIONINFO block of the mapped file @p view, copied into @p out; its length, or 0 when it has none or it
/// doesn't fit. Never inlined, so an in-page error raised while it reads the mapping reaches copyVersionResource()'s
/// handler through this call. Not noexcept, so nothing stands between that exception and the handler.
[[nodiscard, clang::noinline]] std::size_t
copyVersionResourceUnguarded(const std::byte* view, std::size_t size, BYTE* out, std::size_t room)
{
    const std::optional<std::span<const std::byte>> block = VersionResourceMath::findVersionResource(std::span(view, size));
    if (!block.has_value() || block->size() > room)
    {
        return 0;
    }
    std::memcpy(out, block->data(), block->size());
    return block->size();
}

/// copyVersionResourceUnguarded(), where an in-page error reading the mapping (a network file that went away, a read
/// error on the disk) is no block rather than a crash. Nothing here needs unwinding, as __try requires.
[[nodiscard]] std::size_t copyVersionResource(const std::byte* view, std::size_t size, BYTE* out, std::size_t room)
{
    std::size_t copied = 0;
    __try
    {
        copied = copyVersionResourceUnguarded(view, size, out, room);
    }
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        copied = 0;
    }
    return copied;
}

#pragma clang diagnostic pop

/// Unmaps a view of a file mapping.
struct ViewUnmapper
{
    void operator()(const void* view) const noexcept
    {
        UnmapViewOfFile(view);
    }
};

} // namespace

HeldVersionBlock heldVersionBlock(HANDLE file)
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 || std::cmp_greater(size.QuadPart, SIZE_MAX))
    {
        return {}; // an empty file can't be mapped, and has no version info anyway
    }
    // A data mapping, read-only: nothing in it is run or relocated, and it is the held file's, whatever its name is now.
    const UniqueHandle mapping(CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping)
    {
        return {};
    }
    const std::unique_ptr<const void, ViewUnmapper> view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr)
    {
        return {};
    }
    // Kept at twice the largest block's room whatever the block's own length, so a value that claims to run past the
    // block still ends inside this buffer.
    constexpr std::size_t MAX_BLOCK = 0xFFFF; // VS_VERSIONINFO's wLength is a WORD
    HeldVersionBlock held{.buffer = std::vector<BYTE>((2 * MAX_BLOCK) + 16, BYTE{0}), .length = 0};
    held.length = copyVersionResource(
        static_cast<const std::byte*>(view.get()), static_cast<std::size_t>(size.QuadPart), held.buffer.data(), MAX_BLOCK);
    if (held.length == 0)
    {
        return {};
    }
    return held;
}

std::optional<VersionResourceMath::FixedFileVersion> heldFileVersion(HANDLE file)
{
    const HeldVersionBlock held = heldVersionBlock(file);
    if (held.length == 0)
    {
        return std::nullopt;
    }
    return VersionResourceMath::fixedFileVersion(std::as_bytes(std::span(held.buffer).first(held.length)));
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
