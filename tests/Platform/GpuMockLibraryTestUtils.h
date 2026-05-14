#pragma once

#include <cstdlib>
#include <string_view>

namespace Platform::TestSupport
{

// MockGpuLibraryStatus detects at construction whether the mock library directory was
// already added to LD_LIBRARY_PATH by CTest (ENVIRONMENT_MODIFICATION) or coverage.sh
// before process start. Call mocksPreloaded() and GTEST_SKIP() when false: the mocks
// directory is not on the dynamic linker's search path, so dlopen() will not find the mock
// GPU libraries regardless of any runtime env-var manipulation.
struct MockGpuLibraryStatus
{
    MockGpuLibraryStatus()
    {
        // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        if (const char* ldPath = std::getenv("LD_LIBRARY_PATH"))
        {
            // Parse the colon-separated path list and compare complete entries to avoid
            // false positives when another entry shares TASKSMACK_TEST_GPU_MOCK_DIR as a
            // prefix or substring.
            const std::string_view mockDir(TASKSMACK_TEST_GPU_MOCK_DIR);
            std::string_view remaining(ldPath);
            while (!remaining.empty())
            {
                const auto colonPos = remaining.find(':');
                const std::string_view entry = (colonPos == std::string_view::npos) ? remaining : remaining.substr(0, colonPos);
                if (entry == mockDir)
                {
                    m_MocksPreloaded = true;
                    break;
                }
                remaining = (colonPos == std::string_view::npos) ? std::string_view{} : remaining.substr(colonPos + 1);
            }
        }
        // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    }

    [[nodiscard]] bool mocksPreloaded() const noexcept
    {
        return m_MocksPreloaded;
    }

  private:
    bool m_MocksPreloaded = false;
};

// Check whether the GPU mock library directory is already on the dynamic linker's search
// path. The name reflects that this is a detector: it checks state set up by CTest or
// coverage.sh before process start; it does NOT inject mock libraries at runtime (runtime
// LD_LIBRARY_PATH mutations cannot affect dlopen() search paths).
[[nodiscard]] inline MockGpuLibraryStatus checkMockGpuLibrariesPreloaded()
{
    return MockGpuLibraryStatus{};
}

} // namespace Platform::TestSupport
