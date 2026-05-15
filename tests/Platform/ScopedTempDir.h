#pragma once

/// @file ScopedTempDir.h
/// @brief RAII helper for unique temporary directories in Linux test fixtures.

#if defined(__linux__) && __has_include(<unistd.h>)

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace Platform::Test
{

/// RAII temporary directory. The directory name is formed from a caller-supplied
/// base string, the process ID, and a per-process monotonic counter, guaranteeing
/// uniqueness even when many tests run in the same process (e.g. a coverage run).
/// The directory is created in the constructor and removed (best-effort) in the
/// destructor so cleanup happens even if an assertion fires.
struct ScopedTempDir
{
    std::filesystem::path path;

    explicit ScopedTempDir(std::string_view name) : path(std::filesystem::temp_directory_path() / uniqueName(name))
    {
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDir() noexcept
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // must not throw from destructor
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;

  private:
    [[nodiscard]] static std::string uniqueName(std::string_view base)
    {
        static std::atomic<std::uint32_t> counter{0};
        return std::format("{}_{}_{}", base, getpid(), counter.fetch_add(1, std::memory_order_relaxed));
    }
};

} // namespace Platform::Test

#endif // defined(__linux__) && __has_include(<unistd.h>)
