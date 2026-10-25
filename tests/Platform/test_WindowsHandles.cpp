/// @file test_WindowsHandles.cpp
/// @brief Unit tests for the shared Win32 handle owners (WindowsHandles.h) and the shared
/// NtQuerySystemInformation(Ex) resolver (WindowsNtQuery.h), #1183.
///
/// The ownership rules are checked against a counting close with the real kernel-handle validity
/// test, so "closed exactly once" is observed directly; real event handles check the CloseHandle
/// owner end to end.

#ifdef _WIN32

#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
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
// clang-format on

#include <array>
#include <thread>
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
