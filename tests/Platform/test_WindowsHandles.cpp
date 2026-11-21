/// @file test_WindowsHandles.cpp
/// @brief Unit tests for the shared Win32 handle owners (WindowsHandles.h) and the shared
/// NtQuerySystemInformation(Ex) resolver (WindowsNtQuery.h), #1183; and the owners #4367 added, there
/// and in WindowsCoTaskMem.h, WindowsDevInfo.h, WindowsEventLogHandle.h and WindowsPdhQuery.h.
///
/// The ownership rules are checked against a counting close with the real kernel-handle validity
/// test, so "closed exactly once" is observed directly; real event handles check the CloseHandle
/// owner end to end. Each #4367 owner is checked the same way: its traits' validity with a counting close,
/// its free function swapped for a counting one through FreeWith, or its injected release a counting fake;
/// then a real handle or allocation where one is harmless to make.

#ifdef _WIN32

#include "Platform/Windows/WindowsCoTaskMem.h"
#include "Platform/Windows/WindowsDevInfo.h"
#include "Platform/Windows/WindowsEventLogHandle.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsPdhQuery.h"
#include "Platform/Windows/WindowsProcAddress.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <winevt.h>
// clang-format on

#include <array>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// KernelHandleTraits with a close that records each handle it is given instead of closing it.
struct CountingTraits
{
    using Type = HANDLE;

    static std::vector<HANDLE>& closed()
    {
        static std::vector<HANDLE> handles;
        return handles;
    }

    [[nodiscard]] static Type invalid() noexcept
    {
        return KernelHandleTraits::invalid();
    }

    [[nodiscard]] static bool isValid(Type handle) noexcept
    {
        return KernelHandleTraits::isValid(handle);
    }

    static void close(Type handle) noexcept
    {
        try
        {
            closed().push_back(handle);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a test fake; the count check reports the loss
        {}
    }
};

using CountingHandle = UniqueResource<CountingTraits>;

class UniqueResourceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        CountingTraits::closed().clear();
    }

    // Distinct, non-sentinel addresses stand in for kernel handles; they are never dereferenced.
    std::array<int, 2> m_Objects{};
    HANDLE m_First = m_Objects.data();
    HANDLE m_Second = &m_Objects[1];
};

TEST_F(UniqueResourceTest, ClosesOnceOnDestruction)
{
    {
        const CountingHandle handle(m_First);
        EXPECT_TRUE(handle.valid());
        EXPECT_EQ(handle.get(), m_First);
    }
    EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_First});
}

TEST_F(UniqueResourceTest, NeitherSentinelIsClosed)
{
    {
        const CountingHandle nullHandle(nullptr);
        const CountingHandle invalidHandle(INVALID_HANDLE_VALUE);
        const CountingHandle defaulted;
        EXPECT_FALSE(nullHandle.valid());
        EXPECT_FALSE(invalidHandle.valid());
        EXPECT_FALSE(static_cast<bool>(invalidHandle));
        EXPECT_FALSE(defaulted.valid());
        EXPECT_EQ(defaulted.get(), nullptr);
    }
    EXPECT_TRUE(CountingTraits::closed().empty());
}

TEST_F(UniqueResourceTest, MoveConstructionTransfersOwnership)
{
    {
        CountingHandle source(m_First);
        const CountingHandle target(std::move(source));
        EXPECT_FALSE(source.valid()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
        EXPECT_EQ(target.get(), m_First);
        EXPECT_TRUE(CountingTraits::closed().empty());
    }
    EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_First});
}

TEST_F(UniqueResourceTest, MoveAssignmentClosesTheOldHandleOnce)
{
    {
        CountingHandle source(m_First);
        CountingHandle target(m_Second);
        target = std::move(source);
        EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_Second});
        EXPECT_EQ(target.get(), m_First);
    }
    EXPECT_EQ(CountingTraits::closed(), (std::vector<HANDLE>{m_Second, m_First}));
}

TEST_F(UniqueResourceTest, SelfMoveAssignmentKeepsTheHandle)
{
    {
        CountingHandle handle(m_First);
        auto& alias = handle;
        handle = std::move(alias);
        EXPECT_EQ(handle.get(), m_First);
        EXPECT_TRUE(CountingTraits::closed().empty());
    }
    EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_First});
}

TEST_F(UniqueResourceTest, ResetClosesAndAdopts)
{
    {
        CountingHandle handle(m_First);
        handle.reset(m_Second);
        EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_First});
        handle.reset();
        EXPECT_FALSE(handle.valid());
        EXPECT_EQ(CountingTraits::closed(), (std::vector<HANDLE>{m_First, m_Second}));
        handle.reset(INVALID_HANDLE_VALUE); // Adopting a failure sentinel never closes it
    }
    EXPECT_EQ(CountingTraits::closed(), (std::vector<HANDLE>{m_First, m_Second}));
}

TEST_F(UniqueResourceTest, ReleaseGivesUpOwnership)
{
    {
        CountingHandle handle(m_First);
        EXPECT_EQ(handle.release(), m_First);
        EXPECT_FALSE(handle.valid());
    }
    EXPECT_TRUE(CountingTraits::closed().empty());
}

TEST_F(UniqueResourceTest, PutClosesTheHeldHandleAndTakesTheWrittenOne)
{
    {
        CountingHandle handle(m_First);
        HANDLE* slot = handle.put();
        EXPECT_EQ(CountingTraits::closed(), std::vector<HANDLE>{m_First});
        EXPECT_EQ(*slot, nullptr);
        *slot = m_Second;
        EXPECT_EQ(handle.get(), m_Second);
    }
    EXPECT_EQ(CountingTraits::closed(), (std::vector<HANDLE>{m_First, m_Second}));
}

TEST(KernelHandleTraitsTest, BothFailureSentinelsAreInvalid)
{
    EXPECT_FALSE(KernelHandleTraits::isValid(nullptr));
    EXPECT_FALSE(KernelHandleTraits::isValid(INVALID_HANDLE_VALUE));
    EXPECT_EQ(KernelHandleTraits::invalid(), nullptr);
}

TEST(UniqueHandleTest, OwnsARealEventHandle)
{
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event.valid());
    ASSERT_NE(SetEvent(event.get()), FALSE);
    EXPECT_EQ(WaitForSingleObject(event.get(), 0), WAIT_OBJECT_0);

    const UniqueHandle moved(std::move(event));
    EXPECT_FALSE(event.valid()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
    DWORD flags = 0;
    EXPECT_NE(GetHandleInformation(moved.get(), &flags), FALSE);
}

TEST(UniqueHandleTest, ReleasedHandleIsLeftOpen)
{
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event.valid());
    HANDLE raw = event.release();
    DWORD flags = 0;
    EXPECT_NE(GetHandleInformation(raw, &flags), FALSE); // Still open: release() did not close it
    EXPECT_NE(CloseHandle(raw), FALSE);
}

TEST(UniqueModuleTest, FreesItsReference)
{
    // GetModuleHandleExW without GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT adds a reference,
    // which UniqueModule's FreeLibrary drops again; kernel32 stays loaded either way.
    HMODULE raw = nullptr;
    ASSERT_NE(GetModuleHandleExW(0, L"kernel32.dll", &raw), FALSE);
    {
        const UniqueModule module(raw);
        EXPECT_EQ(module.get(), raw);
    }
    EXPECT_NE(GetModuleHandleW(L"kernel32.dll"), nullptr);
}

// --- #4367: the traits-based owners (UniqueDevInfo, UniqueEvtHandle, UniquePdhQuery) ---

/// @p Real's validity, with a close that records each handle it is given instead of closing it.
template<typename Real> struct CountingTraitsOf
{
    using Type = Real::Type;

    static std::vector<Type>& closed()
    {
        static std::vector<Type> handles;
        return handles;
    }

    [[nodiscard]] static Type invalid() noexcept
    {
        return Real::invalid();
    }

    [[nodiscard]] static bool isValid(Type handle) noexcept
    {
        return Real::isValid(handle);
    }

    static void close(Type handle) noexcept
    {
        try
        {
            closed().push_back(handle);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a test fake; the count check reports the loss
        {}
    }
};

template<typename Real> class TraitsOwnerTest : public ::testing::Test
{
  protected:
    using Traits = CountingTraitsOf<Real>;
    using Owner = UniqueResource<Traits>;
    using Type = Traits::Type;

    void SetUp() override
    {
        Traits::closed().clear();
    }

    // Distinct, non-sentinel addresses stand in for the handles; they are never dereferenced.
    std::array<int, 2> m_Objects{};
    Type m_First = m_Objects.data();
    Type m_Second = &m_Objects[1];
};

using OwnerTraits = ::testing::Types<DevInfoTraits, EvtHandleTraits, PdhQueryTraits>;
TYPED_TEST_SUITE(TraitsOwnerTest, OwnerTraits);

TYPED_TEST(TraitsOwnerTest, ReleasedExactlyOnce)
{
    using Owner = TestFixture::Owner;
    using Traits = TestFixture::Traits;
    {
        const Owner owner(this->m_First);
        EXPECT_TRUE(owner.valid());
        EXPECT_EQ(owner.get(), this->m_First);
    }
    EXPECT_EQ(Traits::closed(), std::vector<typename TestFixture::Type>{this->m_First});
}

TYPED_TEST(TraitsOwnerTest, NotReleasedWhenEmpty)
{
    using Owner = TestFixture::Owner;
    using Traits = TestFixture::Traits;
    {
        const Owner defaulted;
        const Owner fromNull(nullptr);
        EXPECT_FALSE(defaulted.valid());
        EXPECT_FALSE(fromNull.valid());
        Owner moved(this->m_First);
        const Owner target(std::move(moved));
        EXPECT_FALSE(moved.valid()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
    }
    EXPECT_EQ(Traits::closed(), std::vector<typename TestFixture::Type>{this->m_First}) << "only the moved-to owner closes";
}

TYPED_TEST(TraitsOwnerTest, ReleasedOnMoveAssign)
{
    using Owner = TestFixture::Owner;
    using Traits = TestFixture::Traits;
    {
        Owner source(this->m_First);
        Owner target(this->m_Second);
        target = std::move(source);
        EXPECT_EQ(Traits::closed(), std::vector<typename TestFixture::Type>{this->m_Second});
        EXPECT_EQ(target.get(), this->m_First);
    }
    EXPECT_EQ(Traits::closed(), (std::vector<typename TestFixture::Type>{this->m_Second, this->m_First}));
}

TYPED_TEST(TraitsOwnerTest, ResetReleaseAndPut)
{
    using Owner = TestFixture::Owner;
    using Traits = TestFixture::Traits;
    {
        Owner owner(this->m_First);
        owner.reset();
        EXPECT_FALSE(owner.valid());
        owner.reset(); // nothing left to close
        EXPECT_EQ(Traits::closed(), std::vector<typename TestFixture::Type>{this->m_First});

        auto* slot = owner.put(); // an out-parameter, as EvtNext and PdhOpenQueryW fill one
        EXPECT_EQ(*slot, nullptr);
        *slot = this->m_Second;
        EXPECT_EQ(owner.release(), this->m_Second);
        EXPECT_FALSE(owner.valid());
    }
    EXPECT_EQ(Traits::closed(), std::vector<typename TestFixture::Type>{this->m_First}) << "a released handle isn't closed";
}

TEST(DevInfoTraitsTest, BothFailureSentinelsAreInvalid)
{
    // SetupDiGetClassDevsW fails with INVALID_HANDLE_VALUE: an owner made from that never destroys it.
    EXPECT_FALSE(DevInfoTraits::isValid(nullptr));
    EXPECT_FALSE(DevInfoTraits::isValid(INVALID_HANDLE_VALUE));
    EXPECT_EQ(DevInfoTraits::invalid(), nullptr);
    CountingTraitsOf<DevInfoTraits>::closed().clear();
    {
        const UniqueResource<CountingTraitsOf<DevInfoTraits>> failed(INVALID_HANDLE_VALUE);
        EXPECT_FALSE(failed.valid());
    }
    EXPECT_TRUE(CountingTraitsOf<DevInfoTraits>::closed().empty());
}

TEST(UniqueDevInfoTest, OwnsARealEmptySet)
{
    // An empty device information set: no device is enumerated or changed.
    UniqueDevInfo devices(SetupDiCreateDeviceInfoList(nullptr, nullptr));
    ASSERT_TRUE(devices.valid()) << GetLastError();
    const UniqueDevInfo moved(std::move(devices));
    EXPECT_FALSE(devices.valid()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    EXPECT_EQ(SetupDiEnumDeviceInfo(moved.get(), 0, &device), FALSE);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_NO_MORE_ITEMS));
}

TEST(UniqueEvtHandleTest, OwnsARealRenderContext)
{
    // A render context: an EVT_HANDLE that reads no log.
    const UniqueEvtHandle context(EvtCreateRenderContext(0, nullptr, static_cast<DWORD>(EvtRenderContextSystem)));
    EXPECT_TRUE(context.valid()) << GetLastError();
}

// --- #4367: the FreeWith owners (UniqueLocal, UniqueEnvironmentStrings, UniqueCoTaskMem) ---

std::vector<void*>& freedMemory()
{
    static std::vector<void*> freed;
    return freed;
}

void countingFree(void* memory) noexcept
{
    try
    {
        freedMemory().push_back(memory);
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a test fake; the count check reports the loss
    {}
}

/// The owners' shape with the free swapped for countingFree: what UniqueLocal, UniqueEnvironmentStrings
/// and UniqueCoTaskMem are, the static_asserts below say, with their own free function in its place.
template<typename T> using CountingMemory = std::unique_ptr<T, FreeWith<&countingFree>>;

static_assert(std::is_same_v<UniqueLocal<LPWSTR>, std::unique_ptr<LPWSTR, FreeWith<&::LocalFree>>>);
static_assert(std::is_same_v<UniqueEnvironmentStrings, std::unique_ptr<wchar_t, FreeWith<&::FreeEnvironmentStringsW>>>);
static_assert(std::is_same_v<UniqueCoTaskMem<wchar_t>, std::unique_ptr<wchar_t, FreeWith<&::CoTaskMemFree>>>);
static_assert(sizeof(UniqueLocal<LPWSTR>) == sizeof(LPWSTR*), "the deleter takes no room");

class FreeWithTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        freedMemory().clear();
    }

    std::array<int, 2> m_Objects{};
};

TEST_F(FreeWithTest, ReleasedExactlyOnce)
{
    {
        const CountingMemory<int> memory(m_Objects.data());
        EXPECT_EQ(memory.get(), m_Objects.data());
    }
    EXPECT_EQ(freedMemory(), std::vector<void*>{m_Objects.data()});
}

TEST_F(FreeWithTest, NotReleasedWhenEmpty)
{
    {
        const CountingMemory<int> defaulted;
        const CountingMemory<int> fromNull(nullptr);
        CountingMemory<int> moved(m_Objects.data());
        const CountingMemory<int> target(std::move(moved));
        EXPECT_EQ(moved.get(),
                  nullptr); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
    }
    EXPECT_EQ(freedMemory(), std::vector<void*>{m_Objects.data()}) << "only the moved-to owner frees";
}

TEST_F(FreeWithTest, ReleasedOnMoveAssign)
{
    {
        CountingMemory<int> source(m_Objects.data());
        CountingMemory<int> target(&m_Objects[1]);
        target = std::move(source);
        EXPECT_EQ(freedMemory(), std::vector<void*>{&m_Objects[1]});
    }
    EXPECT_EQ(freedMemory(), (std::vector<void*>{&m_Objects[1], m_Objects.data()}));
}

TEST_F(FreeWithTest, ResetReleaseAndOutPtr)
{
    {
        CountingMemory<int> memory(m_Objects.data());
        memory.reset();
        memory.reset();
        EXPECT_EQ(freedMemory(), std::vector<void*>{m_Objects.data()});

        // Filled through std::out_ptr, as SHGetKnownFolderPath and GetDisplayName fill theirs.
        const auto fill = [this](int** out)
        {
            *out = &m_Objects[1];
        };
        fill(std::out_ptr(memory));
        EXPECT_EQ(memory.get(), &m_Objects[1]);
        EXPECT_EQ(memory.release(), &m_Objects[1]);
    }
    EXPECT_EQ(freedMemory(), std::vector<void*>{m_Objects.data()}) << "released memory isn't freed";
}

TEST(UniqueLocalTest, FreesRealLocalMemory)
{
    const UniqueLocal<int> memory(static_cast<int*>(LocalAlloc(LMEM_FIXED, sizeof(int))));
    ASSERT_NE(memory.get(), nullptr);
    *memory = 1;
    EXPECT_EQ(LocalSize(memory.get()), sizeof(int));
}

TEST(UniqueEnvironmentStringsTest, OwnsTheRealBlock)
{
    const UniqueEnvironmentStrings strings(GetEnvironmentStringsW());
    ASSERT_NE(strings.get(), nullptr);
    EXPECT_NE(strings.get()[0], L'\0') << "a test process has environment variables";
}

TEST(UniqueCoTaskMemTest, FreesRealTaskMemoryThroughOutPtr)
{
    UniqueCoTaskMem<wchar_t> text;
    const auto fill = [](wchar_t** out)
    {
        *out = static_cast<wchar_t*>(CoTaskMemAlloc(sizeof(L"path")));
        if (*out != nullptr)
        {
            std::memcpy(*out, L"path", sizeof(L"path"));
        }
    };
    fill(std::out_ptr(text));
    ASSERT_NE(text.get(), nullptr);
    EXPECT_STREQ(text.get(), L"path");
}

// --- #4367: the injected-release owners (UniqueWlanHandle, UniqueWlanMemory, UniqueNetApiBuffer) ---

std::vector<HANDLE>& closedWlanHandles()
{
    static std::vector<HANDLE> closed;
    return closed;
}

DWORD WINAPI fakeWlanCloseHandle(HANDLE client, PVOID reserved)
{
    EXPECT_EQ(reserved, nullptr);
    closedWlanHandles().push_back(client);
    return ERROR_SUCCESS;
}

std::vector<PVOID>& freedWlanMemory()
{
    static std::vector<PVOID> freed;
    return freed;
}

VOID WINAPI fakeWlanFreeMemory(PVOID memory)
{
    freedWlanMemory().push_back(memory);
}

std::vector<LPVOID>& freedNetApiBuffers()
{
    static std::vector<LPVOID> freed;
    return freed;
}

DWORD WINAPI fakeNetApiBufferFree(LPVOID buffer)
{
    freedNetApiBuffers().push_back(buffer);
    return ERROR_SUCCESS;
}

class InjectedOwnerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        closedWlanHandles().clear();
        freedWlanMemory().clear();
        freedNetApiBuffers().clear();
    }

    [[nodiscard]] static UniqueWlanHandle wlanHandle(HANDLE client)
    {
        return {client, WlanHandleCloser{.close = &fakeWlanCloseHandle}};
    }

    [[nodiscard]] static UniqueNetApiBuffer<wchar_t> netApiBuffer(wchar_t* buffer)
    {
        return {buffer, &fakeNetApiBufferFree};
    }

    std::array<int, 2> m_Objects{};
    std::array<wchar_t, 2> m_Names{};
    HANDLE m_First = m_Objects.data();
    HANDLE m_Second = &m_Objects[1];
};

TEST_F(InjectedOwnerTest, WlanHandleClosedExactlyOnce)
{
    {
        const UniqueWlanHandle client = wlanHandle(m_First);
        EXPECT_EQ(client.get(), m_First);
    }
    EXPECT_EQ(closedWlanHandles(), std::vector<HANDLE>{m_First});
}

TEST_F(InjectedOwnerTest, WlanHandleNotClosedWhenEmpty)
{
    {
        const UniqueWlanHandle none = wlanHandle(nullptr);
        const UniqueWlanHandle defaulted;
        UniqueWlanHandle moved = wlanHandle(m_First);
        const UniqueWlanHandle target(std::move(moved));
    }
    EXPECT_EQ(closedWlanHandles(), std::vector<HANDLE>{m_First}) << "only the moved-to owner closes";
}

TEST_F(InjectedOwnerTest, WlanHandleClosedOnMoveAssignAndReset)
{
    {
        UniqueWlanHandle source = wlanHandle(m_First);
        UniqueWlanHandle target = wlanHandle(m_Second);
        target = std::move(source);
        EXPECT_EQ(closedWlanHandles(), std::vector<HANDLE>{m_Second});
        target.reset();
        target.reset();
        EXPECT_EQ(closedWlanHandles(), (std::vector<HANDLE>{m_Second, m_First}));
    }
    EXPECT_EQ(closedWlanHandles(), (std::vector<HANDLE>{m_Second, m_First}));
}

TEST_F(InjectedOwnerTest, WlanMemoryFreedExactlyOnce)
{
    {
        UniqueWlanMemory<int> source(m_Objects.data(), &fakeWlanFreeMemory);
        UniqueWlanMemory<int> target(&m_Objects[1], &fakeWlanFreeMemory);
        const UniqueWlanMemory<int> none(nullptr, &fakeWlanFreeMemory);
        target = std::move(source); // frees the second
        EXPECT_EQ(freedWlanMemory(), std::vector<PVOID>{&m_Objects[1]});
    }
    EXPECT_EQ(freedWlanMemory(), (std::vector<PVOID>{&m_Objects[1], m_Objects.data()})) << "null is never freed";
}

TEST_F(InjectedOwnerTest, NetApiBufferFreedExactlyOnce)
{
    {
        const UniqueNetApiBuffer<wchar_t> name = netApiBuffer(m_Names.data());
        EXPECT_TRUE(name.owns());
        EXPECT_EQ(name.get(), m_Names.data());
    }
    EXPECT_EQ(freedNetApiBuffers(), std::vector<LPVOID>{m_Names.data()});
}

TEST_F(InjectedOwnerTest, NetApiBufferFromASuccessFreesNullToo)
{
    // NetGetJoinInformation can succeed with no name; NetApiBufferFree still gets the null, as by hand.
    {
        const UniqueNetApiBuffer<wchar_t> name = netApiBuffer(nullptr);
        EXPECT_TRUE(name.owns());
    }
    EXPECT_EQ(freedNetApiBuffers(), std::vector<LPVOID>{nullptr});
}

TEST_F(InjectedOwnerTest, NetApiBufferNotFreedWhenEmpty)
{
    {
        const UniqueNetApiBuffer<wchar_t> defaulted;
        EXPECT_FALSE(defaulted.owns());
        UniqueNetApiBuffer<wchar_t> moved = netApiBuffer(m_Names.data());
        const UniqueNetApiBuffer<wchar_t> target(std::move(moved));
        EXPECT_FALSE(moved.owns()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) - the moved-from state is under test
        EXPECT_EQ(moved.get(), nullptr);
    }
    EXPECT_EQ(freedNetApiBuffers(), std::vector<LPVOID>{m_Names.data()}) << "only the moved-to owner frees";
}

TEST_F(InjectedOwnerTest, NetApiBufferFreedOnMoveAssignAndReset)
{
    {
        UniqueNetApiBuffer<wchar_t> source = netApiBuffer(m_Names.data());
        UniqueNetApiBuffer<wchar_t> target = netApiBuffer(&m_Names[1]);
        target = std::move(source);
        EXPECT_EQ(freedNetApiBuffers(), std::vector<LPVOID>{&m_Names[1]});
        EXPECT_EQ(target.get(), m_Names.data());
        auto& alias = target;
        target = std::move(alias); // a self-move keeps the buffer
        EXPECT_EQ(target.get(), m_Names.data());
        target.reset();
        EXPECT_FALSE(target.owns());
        target.reset();
        EXPECT_EQ(freedNetApiBuffers(), (std::vector<LPVOID>{&m_Names[1], m_Names.data()}));
    }
    EXPECT_EQ(freedNetApiBuffers(), (std::vector<LPVOID>{&m_Names[1], m_Names.data()}));
}

TEST(NtQueryResolverTest, ResolvesOnceToNtdllsExports)
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    ASSERT_NE(ntdll, nullptr);
    const auto expected = getProcAddress<NtQuerySystemInformationFn>(ntdll, "NtQuerySystemInformation");
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(ntQuerySystemInformation(), expected);
    EXPECT_EQ(ntQuerySystemInformation(), ntQuerySystemInformation());
    EXPECT_EQ(ntQuerySystemInformationEx(), getProcAddress<NtQuerySystemInformationExFn>(ntdll, "NtQuerySystemInformationEx"));
}

TEST(NtQueryResolverTest, ConcurrentFirstCallsAgree)
{
    std::array<NtQuerySystemInformationFn, 8> seen{};
    {
        std::vector<std::jthread> threads;
        threads.reserve(seen.size());
        for (auto& slot : seen)
        {
            threads.emplace_back([&slot] { slot = ntQuerySystemInformation(); });
        }
    }
    for (const auto fn : seen)
    {
        EXPECT_EQ(fn, seen.front());
        EXPECT_NE(fn, nullptr);
    }
}

} // namespace
} // namespace Platform::Windows

#endif // _WIN32
