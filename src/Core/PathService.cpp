#include "PathService.h"

#include "Platform/Factory.h"

#include <system_error>

namespace Core
{

namespace
{

// Normalize a raw path from a platform provider to an absolute, cleaned path.
// Uses the error_code overload to avoid throwing; falls back to the original
// path if std::filesystem::absolute() fails (e.g., the CWD is unavailable).
std::filesystem::path toAbsolute(const std::filesystem::path& raw)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(raw, ec);
    if (ec)
    {
        return raw;
    }
    return abs.lexically_normal();
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
