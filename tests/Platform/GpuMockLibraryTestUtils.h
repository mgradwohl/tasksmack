#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

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

        [[maybe_unused]] const int setResult = setenv(m_Name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_PreviousValue.has_value())
        {
            [[maybe_unused]] const int setResult = setenv(m_Name.c_str(), m_PreviousValue->c_str(), 1);
            return;
        }

        [[maybe_unused]] const int unsetResult = unsetenv(m_Name.c_str());
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

class ScopedGpuMockLibraries
{
  public:
    ScopedGpuMockLibraries()
        : m_NvmlPath("TASKSMACK_NVML_LIB_PATH", std::string(TASKSMACK_TEST_GPU_MOCK_DIR) + "/libnvidia-ml.so.1"),
          m_RocmPath("TASKSMACK_ROCM_LIB_PATH", std::string(TASKSMACK_TEST_GPU_MOCK_DIR) + "/librocm_smi64.so.6"),
          m_LibraryPath(useMockGpuLibraries())
    {}

  private:
    ScopedEnvironmentVariable m_NvmlPath;
    ScopedEnvironmentVariable m_RocmPath;
    ScopedEnvironmentVariable m_LibraryPath;
};

[[nodiscard]] inline ScopedGpuMockLibraries useMockGpuLibrariesWithOverrides()
{
    return ScopedGpuMockLibraries();
}

} // namespace Platform::TestSupport
