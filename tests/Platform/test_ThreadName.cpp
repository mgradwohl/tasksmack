/// @file test_ThreadName.cpp
/// @brief Tests for Platform::truncateThreadName/setCurrentThreadName (#843 measurement kit): the
/// Linux 15-byte limit, cutting only on a UTF-8 code point boundary, every TaskSmack thread name
/// fitting the limit, and the name actually reaching the OS (Linux: the kernel's thread name;
/// Windows: the thread description).

#include "Platform/ThreadName.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <processthreadsapi.h>
// clang-format on
#else
#include <pthread.h>
#endif

namespace
{

TEST(ThreadNameTest, ShortNameIsUnchanged)
{
    EXPECT_EQ(Platform::truncateThreadName("ts-sampler"), "ts-sampler");
}

TEST(ThreadNameTest, NameAtLimitIsUnchanged)
{
    const std::string fifteen(Platform::MAX_LINUX_THREAD_NAME_LENGTH, 'x');
    EXPECT_EQ(Platform::truncateThreadName(fifteen), fifteen);
}

TEST(ThreadNameTest, LongNameIsCutToLimit)
{
    EXPECT_EQ(Platform::truncateThreadName("tasksmack-background-sampler"), "tasksmack-backg");
}

TEST(ThreadNameTest, CutNeverSplitsUtf8Sequence)
{
    // 14 ASCII bytes then U+00E9 (2 bytes): byte 15 would be the middle of the sequence.
    const std::string name = std::string(14, 'a') + "\xC3\xA9" + "zz";
    EXPECT_EQ(Platform::truncateThreadName(name), std::string(14, 'a'));
}

TEST(ThreadNameTest, EveryTaskSmackThreadNameFitsLinuxLimit)
{
    constexpr std::array NAMES{Platform::UI_THREAD_NAME,
                               Platform::SAMPLER_THREAD_NAME,
                               Platform::PROCESS_SAMPLER_THREAD_NAME,
                               Platform::SYSTEM_SAMPLER_THREAD_NAME};
    for (const std::string_view name : NAMES)
    {
        EXPECT_LE(name.size(), Platform::MAX_LINUX_THREAD_NAME_LENGTH) << name;
    }
}

#if !defined(_WIN32)
/// The calling thread's kernel name (/proc/self/task/<tid>/comm).
[[nodiscard]] std::string currentThreadName()
{
    std::array<char, 16> buffer{};
    if (pthread_getname_np(pthread_self(), buffer.data(), buffer.size()) != 0)
    {
        return {};
    }
    return {buffer.data()};
}

TEST(ThreadNameTest, SetCurrentThreadNameReachesKernelTruncated)
{
    bool named = false;
    std::string observed;
    std::thread worker(
        [&]
        {
            named = Platform::setCurrentThreadName("ts-a-very-long-thread-name");
            observed = currentThreadName();
        });
    worker.join();
    EXPECT_TRUE(named);
    EXPECT_EQ(observed, "ts-a-very-long-");
}

TEST(ThreadNameTest, MainThreadNameLeavesProcessNameAlone)
{
    const std::string before = currentThreadName();
    EXPECT_TRUE(Platform::setMainThreadName(Platform::UI_THREAD_NAME));
    EXPECT_EQ(currentThreadName(), before);
}
#else
/// The calling thread's description (GetThreadDescription), or "<error>" if it can't be read.
[[nodiscard]] std::wstring currentThreadDescription()
{
    PWSTR description = nullptr;
    const HRESULT result = GetThreadDescription(GetCurrentThread(), &description);
    std::wstring text = (SUCCEEDED(result) && description != nullptr) ? std::wstring(description) : std::wstring(L"<error>");
    // The description is allocated by the OS and must be released with LocalFree.
    LocalFree(description);
    return text;
}

TEST(ThreadNameTest, SetCurrentThreadNameSetsWindowsDescriptionUntruncated)
{
    bool named = false;
    std::wstring observed;
    std::thread worker(
        [&]
        {
            named = Platform::setCurrentThreadName("ts-a-very-long-thread-name");
            observed = currentThreadDescription();
        });
    worker.join();
    EXPECT_TRUE(named);
    // Windows has no 15-byte limit, so the whole name is kept.
    EXPECT_EQ(observed, L"ts-a-very-long-thread-name");
}

TEST(ThreadNameTest, MainThreadNameSetsWindowsDescription)
{
    // Run on a worker so the test runner's own main thread keeps its description.
    bool named = false;
    std::wstring observed;
    std::thread worker(
        [&]
        {
            named = Platform::setMainThreadName(Platform::UI_THREAD_NAME);
            observed = currentThreadDescription();
        });
    worker.join();
    EXPECT_TRUE(named);
    EXPECT_EQ(observed, L"tasksmack-ui");
}
#endif

} // namespace
