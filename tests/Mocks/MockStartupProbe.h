/// @file MockStartupProbe.h
/// @brief Mock implementation of IStartupProbe for testing (#801)

#pragma once

#include "Platform/IStartupProbe.h"

#include <utility>
#include <vector>

namespace Mocks
{

/// Returns the entries and capabilities a test sets, and counts enumerations.
class MockStartupProbe : public Platform::IStartupProbe
{
  public:
    explicit MockStartupProbe(Platform::StartupCapabilities capabilities = {.canEnumerate = true,
                                                                            .hasEnabledState = true,
                                                                            .hasDisabledTime = true,
                                                                            .hasPublisher = true,
                                                                            .canResolveShortcuts = true,
                                                                            .unavailableReason = {}})
        : m_Capabilities(std::move(capabilities))
    {}

    [[nodiscard]] Platform::StartupCapabilities capabilities() const override
    {
        return m_Capabilities;
    }

    [[nodiscard]] std::vector<Platform::StartupEntry> enumerate() override
    {
        ++m_EnumerateCount;
        return m_Entries;
    }

    void setEntries(std::vector<Platform::StartupEntry> entries)
    {
        m_Entries = std::move(entries);
    }

    [[nodiscard]] int enumerateCount() const noexcept
    {
        return m_EnumerateCount;
    }

  private:
    Platform::StartupCapabilities m_Capabilities;
    std::vector<Platform::StartupEntry> m_Entries;
    int m_EnumerateCount = 0;
};

} // namespace Mocks
