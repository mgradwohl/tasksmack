#include "PathService.h"

#include "PathServiceMath.h"
#include "Platform/Factory.h"
#include "Platform/IPathProvider.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace Core
{

namespace
{

// Normalize a raw path from a platform provider to an absolute, cleaned path, delegating the
// fallback decision logic to resolveAbsolutePath() (PathServiceMath.h) against the real
// std::filesystem primitives.
std::filesystem::path toAbsolute(const std::filesystem::path& raw)
{
    return resolveAbsolutePath(
        raw,
        [](const std::filesystem::path& p, std::error_code& ec) { return std::filesystem::absolute(p, ec); },
        [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

} // namespace

PathService::PathService(std::unique_ptr<Platform::IPathProvider> provider)
{
    if (!provider)
    {
        throw std::invalid_argument("PathService: provider must not be null");
    }
    m_ExecutableDir = toAbsolute(provider->getExecutableDir());
    m_UserConfigDir = toAbsolute(provider->getUserConfigDir());
}

PathService::PathService() : PathService(Platform::makePathProvider())
{}

const std::filesystem::path& PathService::executableDir() const noexcept
{
    return m_ExecutableDir;
}

const std::filesystem::path& PathService::userConfigDir() const noexcept
{
    return m_UserConfigDir;
}

} // namespace Core
