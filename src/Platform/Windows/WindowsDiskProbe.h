#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

#if defined(_WIN32)
// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
// clang-format on
#endif

#include <string>
#include <vector>

namespace Platform
{

/// Windows implementation of IDiskProbe using Performance Data Helper (PDH).
/// Reads disk I/O metrics from Windows Performance Counters.
class WindowsDiskProbe : public IDiskProbe
{
  public:
    WindowsDiskProbe();
    ~WindowsDiskProbe() override;

    WindowsDiskProbe(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe& operator=(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe(WindowsDiskProbe&&) = delete;
    WindowsDiskProbe& operator=(WindowsDiskProbe&&) = delete;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

  private:
#if defined(_WIN32)
    struct DiskCounterSet
    {
        std::string instanceName;
        PDH_HCOUNTER readBytesCounter = nullptr;
        PDH_HCOUNTER writeBytesCounter = nullptr;
        PDH_HCOUNTER readsCounter = nullptr;
        PDH_HCOUNTER writesCounter = nullptr;
        PDH_HCOUNTER idleTimeCounter = nullptr;
    };

    PDH_HQUERY m_Query = nullptr;
    std::vector<DiskCounterSet> m_DiskCounters;
#endif
};

} // namespace Platform
