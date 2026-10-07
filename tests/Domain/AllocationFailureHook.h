#pragma once

/// @file AllocationFailureHook.h
/// @brief Fail a chosen allocation on the current thread, for the models' transaction tests (#1412).
///
/// AllocationFailureHook.cpp replaces the global operator new for the whole test binary with one that
/// can be told to throw std::bad_alloc on the Nth allocation the current thread makes from now. It is
/// disarmed (the default) for every other test and every other thread, and then only counts down a
/// thread_local, so it changes nothing else. A binary can hold only one replacement, so every test
/// that injects allocation failures shares this one.
///
/// Where the replacement is compiled out and those tests skip (TASKSMACK_NO_ALLOCATOR_HOOK names why):
/// - sanitizer builds (ASan, MSan, TSan): their runtimes define the global operator new themselves,
///   so a replacement would not link;
/// - Windows: the MSVC/clang-cl runtime (debug CRT heap, CRT-internal allocations) does not tolerate a
///   replaced operator new failing on demand -- the test aborted there with no output (#1454 CI). The
///   logic under test is platform-independent and runs in the Linux debug and release builds.

#include <cstdint>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define TASKSMACK_NO_ALLOCATOR_HOOK "the sanitizer runtime owns operator new"
#endif
#endif
#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define TASKSMACK_NO_ALLOCATOR_HOOK "the sanitizer runtime owns operator new"
#endif
#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK) && defined(_WIN32)
#define TASKSMACK_NO_ALLOCATOR_HOOK "the Windows C runtime does not support failing a replaced operator new on demand"
#endif

namespace TestSupport
{

/// Make the @p count-th allocation from now on this thread (0 = the next one) throw std::bad_alloc;
/// a negative count disarms. One failure per arming: the hook disarms itself when it fires.
void armAllocationFailure(std::int64_t count) noexcept;

/// Whether this thread's armed failure has yet to fire: false once it has thrown (or when disarmed).
/// Lets a test that cannot see the std::bad_alloc -- a model that catches and logs it -- tell whether
/// the call it armed reached the failing allocation.
[[nodiscard]] bool allocationFailurePending() noexcept;

/// Makes the @p count-th allocation from now on this thread throw, for the guard's lifetime.
class FailAllocationAfter
{
  public:
    explicit FailAllocationAfter(std::int64_t count) noexcept
    {
        armAllocationFailure(count);
    }
    ~FailAllocationAfter()
    {
        armAllocationFailure(-1);
    }
    FailAllocationAfter(const FailAllocationAfter&) = delete;
    FailAllocationAfter& operator=(const FailAllocationAfter&) = delete;
    FailAllocationAfter(FailAllocationAfter&&) = delete;
    FailAllocationAfter& operator=(FailAllocationAfter&&) = delete;
};

} // namespace TestSupport
