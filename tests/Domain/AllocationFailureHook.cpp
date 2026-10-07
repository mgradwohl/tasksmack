// The test binary's one replacement of the global operator new (see AllocationFailureHook.h).

#include "AllocationFailureHook.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace
{

/// Allocations by this thread before the one that fails; negative when disarmed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the allocator hook's per-thread switch
thread_local std::int64_t t_AllocationsBeforeFailure = -1;

} // namespace

namespace TestSupport
{

void armAllocationFailure(std::int64_t count) noexcept
{
    t_AllocationsBeforeFailure = count;
}

bool allocationFailurePending() noexcept
{
    return t_AllocationsBeforeFailure >= 0;
}

} // namespace TestSupport

#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
// The replacement allocation functions, for the whole test binary. Disarmed, they are a plain
// malloc/free pair. Only operator new(size_t) and the two unaligned operator deletes are replaced; the
// standard's default operator new[] and nothrow forms call operator new(size_t), and its default
// operator delete[] and sized forms call operator delete(void*), so every unaligned form allocates
// with malloc and frees with free. The aligned forms are not replaced at all, so they keep the
// library's own matching new/delete pair.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads,hicpp-no-malloc)
void* operator new(std::size_t size)
{
    if (t_AllocationsBeforeFailure == 0)
    {
        t_AllocationsBeforeFailure = -1; // one failure per arming
        throw std::bad_alloc();
    }
    if (t_AllocationsBeforeFailure > 0)
    {
        --t_AllocationsBeforeFailure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept
{
    std::free(memory);
}
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads,hicpp-no-malloc)
#endif
