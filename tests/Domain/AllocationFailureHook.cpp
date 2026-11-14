// The test binary's one replacement of the global operator new (see AllocationFailureHook.h).

#include "AllocationFailureHook.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{

/// Allocations by this thread before the one that fails; negative when disarmed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the allocator hook's per-thread switch
thread_local std::int64_t t_AllocationsBeforeFailure = -1;

/// Allocations this thread has made through the replaced operator new (allocationsOnThisThread()).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the allocator hook's per-thread count
thread_local std::uint64_t t_AllocationCount = 0;

#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
/// The test noted last, so a sweep that arms the hook thousands of times writes its name once.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the log's one-line-per-test memory
std::atomic<const ::testing::TestInfo*> g_LastNotedTest{nullptr};

/// Appends "Suite.Case" to the file TASKSMACK_ALLOCATION_HOOK_LOG names, once per test, so CI can say which
/// tests ran with the hook armed (#3342). It runs before the countdown is set and makes no allocation through
/// operator new (a stack buffer, open/write/close), so it can't spend or trip the allocation it arms.
void noteArmedTest() noexcept
{
    const char* const path = std::getenv("TASKSMACK_ALLOCATION_HOOK_LOG"); // NOLINT(concurrency-mt-unsafe) - set before the tests start
    if (path == nullptr || *path == '\0')
    {
        return;
    }
    const ::testing::TestInfo* const test = ::testing::UnitTest::GetInstance()->current_test_info();
    if (test == nullptr || g_LastNotedTest.exchange(test) == test)
    {
        return;
    }
    std::array<char, 512> line{};
    const int length = std::snprintf(line.data(), line.size(), "%s.%s\n", test->test_suite_name(), test->name());
    if (length <= 0)
    {
        return;
    }
    const int file = ::open(
        path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg,hicpp-vararg) - POSIX open
    if (file < 0)
    {
        return;
    }
    // One write per line: O_APPEND keeps lines whole when ctest runs suites in parallel processes.
    const auto size = std::min(static_cast<std::size_t>(length), line.size() - 1);
    [[maybe_unused]] const auto written = ::write(file, line.data(), size);
    ::close(file);
}
#endif

} // namespace

namespace TestSupport
{

void armAllocationFailure(std::int64_t count) noexcept
{
#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    if (count >= 0)
    {
        noteArmedTest(); // before the countdown starts: nothing it does may count
    }
#endif
    t_AllocationsBeforeFailure = count;
}

bool allocationFailurePending() noexcept
{
    return t_AllocationsBeforeFailure >= 0;
}

std::uint64_t allocationsOnThisThread() noexcept
{
    return t_AllocationCount;
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
    ++t_AllocationCount;
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
