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

// ScopedGpuMockLibraries is a no-op: the mock libraries are made available by CTest setting
// LD_LIBRARY_PATH=${TASKSMACK_TEST_GPU_MOCK_DIR} before the test process starts, so dlopen()
// finds them without any runtime env-var manipulation. The guard is kept for API compatibility
// with existing tests that hold an envGuard to document their dependency on the mock.
struct ScopedGpuMockLibraries
{};

[[nodiscard]] inline ScopedGpuMockLibraries useMockGpuLibrariesWithOverrides()
{
    return ScopedGpuMockLibraries{};
}

} // namespace Platform::TestSupport
