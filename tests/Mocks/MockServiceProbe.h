/// @file MockServiceProbe.h
/// @brief Mock implementation of IServiceProbe for testing (#800)

#pragma once

#include "Platform/IServiceProbe.h"

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

    [[nodiscard]] std::vector<Platform::ServiceInfo> enumerate() override
    {
        ++m_EnumerateCount;
        return m_Services;
    }

    void setServices(std::vector<Platform::ServiceInfo> services)
    {
        m_Services = std::move(services);
    }

    [[nodiscard]] int enumerateCount() const noexcept
    {
        return m_EnumerateCount;
    }

  private:
    Platform::ServiceCapabilities m_Capabilities;
    std::vector<Platform::ServiceInfo> m_Services;
    int m_EnumerateCount = 0;
};

} // namespace Mocks
