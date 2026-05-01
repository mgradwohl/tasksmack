#include "PathService.h"

#include "Platform/Factory.h"

namespace Core
{

PathService::PathService()
    : m_Provider(Platform::makePathProvider()),
      m_ExecutableDir(m_Provider->getExecutableDir()),
      m_UserConfigDir(m_Provider->getUserConfigDir())
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
