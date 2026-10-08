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
    explicit MockServiceProbe(Platform::ServiceCapabilities capabilities = {.canEnumerate = true}) : m_Capabilities(capabilities)
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
            return {.failureReason = m_FailureReason};
        }
        return {.ok = true, .failureReason = {}, .services = m_Services};
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

  private:
    Platform::ServiceCapabilities m_Capabilities;
    std::vector<Platform::ServiceInfo> m_Services;
    std::string m_FailureReason;
    int m_EnumerateCount = 0;
};

} // namespace Mocks
