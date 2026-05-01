#include "PathService.h"

#include "Platform/Factory.h"

namespace Core
{

PathService::PathService()
{
    auto provider = Platform::makePathProvider();
    m_ExecutableDir = provider->getExecutableDir();
    m_UserConfigDir = provider->getUserConfigDir();
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
