#include "PathService.h"

#include "Platform/Factory.h"

#include <system_error>

namespace Core
{

namespace
{

// Normalize a raw path from a platform provider to an absolute, cleaned path.
// Uses the error_code overload to avoid throwing.
// Fallback order when absolute() fails:
//   1. If raw is relative, try (current_path() / raw).lexically_normal().
//   2. Otherwise (or if current_path() also fails), return raw.lexically_normal()
//      so callers at least get a cleaned path even if it may still be relative.
std::filesystem::path toAbsolute(const std::filesystem::path& raw)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(raw, ec);
    if (!ec)
    {
        return abs.lexically_normal();
    }

    // absolute() failed — try composing with current_path() for relative inputs.
    if (raw.is_relative())
    {
        std::error_code cwdEc;
        std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
        if (!cwdEc)
        {
            return (cwd / raw).lexically_normal();
        }
    }

    // All fallbacks failed; return a lexically-normalized copy so the path is
    // at least cleaned even though it may not be absolute.
    return raw.lexically_normal();
}

} // namespace

PathService::PathService()
{
    auto provider = Platform::makePathProvider();
    m_ExecutableDir = toAbsolute(provider->getExecutableDir());
    m_UserConfigDir = toAbsolute(provider->getUserConfigDir());
}

const std::filesystem::path& PathService::executableDir() const noexcept
{
    return m_ExecutableDir;
}

const std::filesystem::path& PathService::userConfigDir() const noexcept
{
    return m_UserConfigDir;
}

} // namespace Core
