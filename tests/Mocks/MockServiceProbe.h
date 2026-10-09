/// @file MockServiceProbe.h
/// @brief Mock implementation of IServiceProbe for testing (#800)

#pragma once

#include "Platform/IServiceProbe.h"

#include <string>
#include <utility>
#include <vector>

namespace Mocks
{

/// Returns the services and capabilities a test sets, and counts enumerations.
class MockServiceProbe : public Platform::IServiceProbe
{
  public:
    MockServiceProbe() : m_Capabilities(enumerable())
    {}

    explicit MockServiceProbe(Platform::ServiceCapabilities capabilities) : m_Capabilities(std::move(capabilities))
    {}

    [[nodiscard]] Platform::ServiceCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] Platform::ServiceEnumeration enumerate() override
    {
        ++m_EnumerateCount;
        if (!m_FailureReason.empty())
        {
            return Platform::ServiceEnumeration::failed(m_FailureReason);
        }
        return Platform::ServiceEnumeration::succeeded(m_Services);
    }

    /// The next enumerations succeed with these services.
    void setServices(std::vector<Platform::ServiceInfo> services)
    {
        m_Services = std::move(services);
        m_FailureReason.clear();
    }

    /// The next enumerations fail with this reason, until setServices().
    void setFailure(std::string reason)
    {
        m_FailureReason = std::move(reason);
    }

    [[nodiscard]] int enumerateCount() const noexcept
    {
        return m_EnumerateCount;
    }

    void forgetCachedConfig() override
    {
        ++m_ForgetCount;
    }

    [[nodiscard]] int forgetCount() const noexcept
    {
        return m_ForgetCount;
    }

  private:
    /// A probe that can list services (every other capability left false).
    [[nodiscard]] static Platform::ServiceCapabilities enumerable()
    {
        Platform::ServiceCapabilities capabilities;
        capabilities.canEnumerate = true;
        return capabilities;
    }

    Platform::ServiceCapabilities m_Capabilities;
    std::vector<Platform::ServiceInfo> m_Services;
    std::string m_FailureReason;
    int m_EnumerateCount = 0;
    int m_ForgetCount = 0;
};

} // namespace Mocks
