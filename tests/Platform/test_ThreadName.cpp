/// @file test_ThreadName.cpp
/// @brief Tests for Platform::truncateThreadName/setCurrentThreadName (#843 measurement kit): the
/// Linux 15-byte limit, cutting only on a UTF-8 code point boundary, every TaskSmack thread name
/// fitting the limit, and (on Linux) the name actually reaching the kernel.

#include "Platform/ThreadName.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <thread>

#if !defined(_WIN32)
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
#endif

} // namespace
