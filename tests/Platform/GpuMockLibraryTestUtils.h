#pragma once

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Platform::TestSupport
{

class ScopedEnvironmentVariable
{
  public:
    ScopedEnvironmentVariable(std::string name, std::string value) : m_Name(std::move(name))
    {
        if (const char* previous = std::getenv(m_Name.c_str()))
        {
            m_PreviousValue = previous;
        }

        if (setenv(m_Name.c_str(), value.c_str(), 1) != 0)
        {
            throw std::runtime_error("Failed to set environment variable: " + m_Name);
        }
    }

    ~ScopedEnvironmentVariable()
    {
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
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable&&) = delete;

  private:
    std::string m_Name;
    std::optional<std::string> m_PreviousValue;
};

[[nodiscard]] inline std::string prependToLibrarySearchPath(std::string_view directory)
{
    std::string value(directory);
    if (const char* previous = std::getenv("LD_LIBRARY_PATH"))
    {
        if (previous[0] != '\0')
        {
            value += ":";
            value += previous;
        }
    }

    return value;
}

[[nodiscard]] inline ScopedEnvironmentVariable useMockGpuLibraries()
{
    return ScopedEnvironmentVariable("LD_LIBRARY_PATH", prependToLibrarySearchPath(TASKSMACK_TEST_GPU_MOCK_DIR));
}

// ScopedGpuMockLibraries detects at construction whether the mock library directory was
// already added to LD_LIBRARY_PATH by CTest (ENVIRONMENT_MODIFICATION) or coverage.sh
// before process start. Call mocksPreloaded() and GTEST_SKIP() when false: the mocks
// directory is not on the dynamic linker's search path, so dlopen() will not find the mock
// GPU libraries regardless of any runtime env-var manipulation.
struct ScopedGpuMockLibraries
{
    ScopedGpuMockLibraries()
    {
        // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        if (const char* ldPath = std::getenv("LD_LIBRARY_PATH"))
        {
            const std::string_view searchPath(ldPath);
            m_MocksPreloaded = searchPath.contains(TASKSMACK_TEST_GPU_MOCK_DIR);
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

[[nodiscard]] inline ScopedGpuMockLibraries useMockGpuLibrariesWithOverrides()
{
    return ScopedGpuMockLibraries{};
}

} // namespace Platform::TestSupport
