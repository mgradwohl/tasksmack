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
/// - Windows debug builds (_ITERATOR_DEBUG_LEVEL 2, the MSVC STL's default with _DEBUG, or 1): every
///   container, even an empty std::string or std::vector, allocates a _Container_proxy through operator new, and does
///   so in noexcept constructors and moves. A failure armed there calls std::terminate -- the test binary
///   exited with code 3 and no output (#1454 CI, #3846) -- and every container built counts as an
///   allocation, so the "allocates nothing" checks can't hold. The hook can't tell a proxy allocation from
///   any other, and the proxy is built into every translation unit, so it stays compiled out there.
///
/// Windows release builds (_ITERATOR_DEBUG_LEVEL 0, /MT) compile it in (#3846). Under the MSVC ABI a
/// replacement in the executable serves the whole program: the static CRT's default operator new[] and
/// nothrow forms call operator new(size_t), and its operator delete[] and sized forms call operator
/// delete(void*), as on Linux. What it can't fail: allocations the CRT or Windows make with malloc,
/// HeapAlloc or their own heaps (the UCRT's internals, the loader, COM, ETW and the other system APIs),
/// which never call operator new.

#include <cstdint>
// The MSVC STL defines _ITERATOR_DEBUG_LEVEL in <yvals.h>, which <cstdint> doesn't include and <new> does
// (through <exception>). Without it, a translation unit built without the precompiled header would see no
// level, compile the hook in a debug build and disagree with the rest of the binary.
#include <new> // NOLINT(misc-include-cleaner) - for _ITERATOR_DEBUG_LEVEL, see above
#if defined(_MSVC_STL_VERSION) && !defined(_ITERATOR_DEBUG_LEVEL)
#error "AllocationFailureHook.h: the MSVC STL's _ITERATOR_DEBUG_LEVEL isn't defined here"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - tested with #ifdef, which a constexpr can't be
#define TASKSMACK_NO_ALLOCATOR_HOOK "the sanitizer runtime owns operator new"
#endif
#endif
#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - tested with #ifdef, which a constexpr can't be
#define TASKSMACK_NO_ALLOCATOR_HOOK "the sanitizer runtime owns operator new"
#endif
#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK) && defined(_ITERATOR_DEBUG_LEVEL)
#if _ITERATOR_DEBUG_LEVEL != 0
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - tested with #ifdef, which a constexpr can't be
#define TASKSMACK_NO_ALLOCATOR_HOOK                                                                                                        \
    "the checked MSVC STL (_ITERATOR_DEBUG_LEVEL 1 or 2) allocates a container proxy through operator new in noexcept "                    \
    "constructors, so a failed allocation there terminates; the Windows release build runs this case"
#endif
#endif

namespace TestSupport
{

/// Make the @p count-th allocation from now on this thread (0 = the next one) throw std::bad_alloc;
/// a negative count disarms. One failure per arming: the hook disarms itself when it fires.
/// When TASKSMACK_ALLOCATION_HOOK_LOG names a file, the first arming in each test appends "Suite.Case"
/// to it, and CI's tools/allocation-hook-summary.py reports which tests ran with the hook (#3342).
void armAllocationFailure(std::int64_t count) noexcept;

/// Whether this thread's armed failure has yet to fire: false once it has thrown (or when disarmed).
/// Lets a test that cannot see the std::bad_alloc -- a model that catches and logs it -- tell whether
/// the call it armed reached the failing allocation.
[[nodiscard]] bool allocationFailurePending() noexcept;

/// How many allocations this thread has made through operator new since it started, so a test can
/// show that building something allocates nothing (#843): read it before and after. Always 0 where the
/// replacement is compiled out (TASKSMACK_NO_ALLOCATOR_HOOK), so such a test skips there.
[[nodiscard]] std::uint64_t allocationsOnThisThread() noexcept;

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
