// SystemModel's history append is a transaction (#1412): a std::bad_alloc anywhere in a sample --
// including one that introduces a new interface and a new core -- must leave every history series
// aligned with the timestamps and the published generation unchanged.
//
// The failure is real, not simulated: this file replaces the global operator new with one that can be
// told to throw on the Nth allocation made by the current thread from now. It is disarmed (the
// default) for every other test in the binary and every other thread, and then only counts down a
// thread_local, so it changes nothing else.

#include "Domain/SystemModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

// Where the replacement is compiled out and the test skipped (TASKSMACK_NO_ALLOCATOR_HOOK names why):
// - sanitizer builds (ASan, MSan, TSan): their runtimes define the global operator new themselves,
//   so a replacement would not link;
// - Windows: the MSVC/clang-cl runtime (debug CRT heap, CRT-internal allocations) does not tolerate a
//   replaced operator new failing on demand -- the test aborted there with no output (#1454 CI). The
//   logic under test is platform-independent and runs in the Linux debug and release builds.
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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{

/// Allocations by this thread before the one that fails; negative when disarmed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the allocator hook's per-thread switch
thread_local std::int64_t t_AllocationsBeforeFailure = -1;

/// Makes the @p count-th allocation from now on this thread (0 = the next one) throw std::bad_alloc,
/// for the guard's lifetime.
class FailAllocationAfter
{
  public:
    explicit FailAllocationAfter(std::int64_t count) noexcept
    {
        t_AllocationsBeforeFailure = count;
    }
    ~FailAllocationAfter()
    {
        t_AllocationsBeforeFailure = -1;
    }
    FailAllocationAfter(const FailAllocationAfter&) = delete;
    FailAllocationAfter& operator=(const FailAllocationAfter&) = delete;
    FailAllocationAfter(FailAllocationAfter&&) = delete;
    FailAllocationAfter& operator=(FailAllocationAfter&&) = delete;
};

} // namespace

#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
// The replacement allocation functions (see the file comment), for the whole test binary. Disarmed,
// they are a plain malloc/free pair. Only operator new(size_t) and the two unaligned operator deletes
// are replaced; the standard's default operator new[] and nothrow forms call operator new(size_t),
// and its default operator delete[] and sized forms call operator delete(void*), so every unaligned
// form allocates with malloc and frees with free. The aligned forms are not replaced at all, so they
// keep the library's own matching new/delete pair.
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

namespace
{

constexpr std::uint64_t WARM_UP_SAMPLES = 30;

/// Counters for sample @p step: cores 0..cores-1 and the named interfaces, all advancing.
Platform::SystemCounters countersAt(std::uint64_t step, std::size_t cores, const std::vector<std::string>& interfaces)
{
    const auto cpu = [step](std::uint64_t salt)
    {
        const std::uint64_t busy = 1 + ((step + salt) % 5);
        return TestMocks::makeCpuCounters(step * busy, 0, step, step * (10 - busy));
    };
    std::vector<Platform::CpuCounters> perCore;
    perCore.reserve(cores);
    for (std::size_t core = 0; core < cores; ++core)
    {
        perCore.push_back(cpu(core));
        perCore.back().coreId = core;
    }
    std::vector<Platform::SystemCounters::InterfaceCounters> ifaces;
    ifaces.reserve(interfaces.size());
    for (const auto& name : interfaces)
    {
        ifaces.push_back(TestMocks::makeInterfaceCounters(name, step * 1000, step * 500));
    }
    return TestMocks::makeSystemCounters(
        cpu(99), TestMocks::makeMemoryCounters(1024, 512), step, std::move(perCore), 0, 0, std::move(ifaces));
}

/// Every history series the model holds is as long as its timestamps, and every series of its
/// publication as long as the publication's timestamps.
void expectAligned(const Domain::SystemModel& model, const std::vector<std::string>& interfaces)
{
    const std::size_t samples = model.timestamps().size();
    for (const auto& series : {model.cpuHistory(),
                               model.cpuUserHistory(),
                               model.cpuSystemHistory(),
                               model.cpuIowaitHistory(),
                               model.cpuIdleHistory(),
                               model.memoryHistory(),
                               model.memoryCachedHistory(),
                               model.swapHistory(),
                               model.powerHistory(),
                               model.batteryChargeHistory(),
                               model.netRxHistory(),
                               model.netTxHistory()})
    {
        EXPECT_EQ(series.size(), samples);
    }
    for (const auto& core : model.perCoreHistory())
    {
        EXPECT_EQ(core.size(), samples);
    }
    for (const auto& name : interfaces)
    {
        const auto rx = model.netRxHistoryForInterface(name);
        const auto tx = model.netTxHistoryForInterface(name);
        EXPECT_EQ(rx.size(), tx.size()) << name; // both or neither
        EXPECT_TRUE(rx.empty() || rx.size() == samples) << name;
    }

    const auto publication = model.publication();
    const std::size_t published = publication->timestamps.size();
    EXPECT_EQ(publication->cpuHistory.size(), published);
    EXPECT_EQ(publication->netRxHistory.size(), published);
    EXPECT_EQ(publication->perInterfaceRxHistory.size(), publication->perInterfaceTxHistory.size());
    for (const auto& core : publication->perCoreHistory)
    {
        EXPECT_EQ(core.size(), published);
    }
    for (const auto& [name, rx] : publication->perInterfaceRxHistory)
    {
        EXPECT_EQ(rx.size(), published) << name;
        EXPECT_EQ(publication->perInterfaceTxHistory.at(name).size(), published) << name;
    }
}

// Fails each allocation of one sample in turn -- the sample that adds core 2 and interface wlan0 to a
// two-core, eth0-only history -- until the sample needs no more. After every failure the series are
// still aligned, the publication and its version are untouched, and the next sample applies cleanly.
TEST(SystemModelAllocationFailureTest, AFailedAllocationOnASampleThatAddsAnInterfaceAndACoreKeepsTheSeriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    const std::vector<std::string> before{"eth0"};
    const std::vector<std::string> after{"eth0", "wlan0"};
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one sample's

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        Domain::SystemModel model(std::make_unique<TestMocks::MockSystemProbe>());
        std::uint64_t step = 0;
        for (; step < WARM_UP_SAMPLES; ++step)
        {
            model.updateFromCounters(countersAt(step, 2, before), static_cast<double>(step));
        }
        const auto publicationBefore = model.publication();
        const std::uint64_t versionBefore = model.publicationVersion();
        const std::size_t samplesBefore = model.timestamps().size();
        const auto newSample = countersAt(step, 3, after);

        bool threw = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                model.updateFromCounters(newSample, static_cast<double>(step));
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
        }
        if (!threw)
        {
            break; // the sample made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        expectAligned(model, after);
        EXPECT_EQ(model.publicationVersion(), versionBefore);
        EXPECT_EQ(model.publication(), publicationBefore);
        // The history either did not move, or (a failure in publish(), after the append) moved by
        // exactly one sample in every series together.
        const std::size_t samplesAfter = model.timestamps().size();
        EXPECT_TRUE(samplesAfter == samplesBefore || samplesAfter == samplesBefore + 1) << samplesAfter;

        // The model carries on: the next sample applies and publishes, with the new core and interface.
        model.updateFromCounters(countersAt(step + 1, 3, after), static_cast<double>(step + 1));
        expectAligned(model, after);
        EXPECT_GT(model.publicationVersion(), versionBefore);
        EXPECT_EQ(model.publication()->perCoreHistory.size(), 3U);
        EXPECT_EQ(model.publication()->perInterfaceRxHistory.count("wlan0"), 1U);
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 10); // the sample did allocate, and those allocations were exercised
}

} // namespace
