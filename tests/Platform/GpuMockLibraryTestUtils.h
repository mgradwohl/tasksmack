#pragma once

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Platform::TestSupport
{

// RAII helper that saves and restores a single environment variable.
// Useful in tests that need to temporarily override env-vars for non-dlopen paths;
// note that LD_LIBRARY_PATH mutations at runtime do NOT affect the dynamic linker's
// dlopen() search path (which is cached at process startup).
class ScopedEnvironmentVariable
{
  public:
    ScopedEnvironmentVariable(std::string name, std::string value) : m_Name(std::move(name))
    {
        // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        // Single-threaded test setup; env-var access is safe here.
        if (const char* previous = std::getenv(m_Name.c_str()))
        {
            m_PreviousValue = previous;
        }
        const int setResult = setenv(m_Name.c_str(), value.c_str(), 1);
        // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        if (setResult != 0)
        {
            throw std::runtime_error("Failed to set environment variable: " + m_Name);
        }
    }

    ~ScopedEnvironmentVariable()
    {
        // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        if (m_PreviousValue.has_value())
        {
            if (setenv(m_Name.c_str(), m_PreviousValue->c_str(), 1) != 0)
            {
                std::abort();
            }
            return;
        }
        if (unsetenv(m_Name.c_str()) != 0)
        {
            std::abort();
        }
        // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable&&) = delete;

  private:
    std::string m_Name;
    std::optional<std::string> m_PreviousValue;
};

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
