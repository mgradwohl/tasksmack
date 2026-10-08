#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// A service's run state, as the service manager reports it (#800).
enum class ServiceState : std::uint8_t
{
    Unknown,
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
};

/// When a service is started.
enum class ServiceStartType : std::uint8_t
{
    Unknown,
    Automatic,
    AutomaticDelayed,
    Manual,
    Disabled,
    Boot,
    System,
};

/// One service, raw as the service manager reports it. A field the probe could not read (its
/// capability is false, or reading this service's configuration was denied) is left empty, 0 or
/// Unknown.
struct ServiceInfo
{
    std::string name;        ///< The key: the service's short name (Windows) or unit name.
    std::string displayName; ///< Friendly name.
    std::string description;
    ServiceState state = ServiceState::Unknown;
    ServiceStartType startType = ServiceStartType::Unknown;
    std::string serviceType; ///< e.g. "Own process", "Shared process".
    std::uint32_t pid = 0;   ///< 0 when not running.
    std::string binaryPath;  ///< Command line the service is started with.
    std::string account;     ///< Account the service logs on as.
    std::string group;       ///< svchost group (Windows "-k <group>"), when there is one.
};

/// Which ServiceInfo fields this platform can fill at all, following ProcessCapabilities.
struct ServiceCapabilities
{
    bool canEnumerate = false; ///< False: this platform has no service list (the UI says so).
    bool hasDisplayName = false;
    bool hasDescription = false;
    bool hasStartType = false;
    bool hasServiceType = false;
    bool hasPid = false;
    bool hasBinaryPath = false;
    bool hasAccount = false;
    bool hasGroup = false;
};

/// Reads the system's services (read-only; #800 phase 1). Called from one thread at a time.
class IServiceProbe
{
  public:
    virtual ~IServiceProbe() = default;

    IServiceProbe() = default;
    IServiceProbe(const IServiceProbe&) = default;
    IServiceProbe& operator=(const IServiceProbe&) = default;
    IServiceProbe(IServiceProbe&&) = default;
    IServiceProbe& operator=(IServiceProbe&&) = default;

    [[nodiscard]] virtual ServiceCapabilities capabilities() const = 0;

    /// Every service with its current state. Empty when the list cannot be read.
    [[nodiscard]] virtual std::vector<ServiceInfo> enumerate() = 0;
};

/// The probe for a platform without a service implementation yet (Linux until systemd support
/// lands, synthetic runs): no services, and canEnumerate false so the UI can say so.
class UnsupportedServiceProbe final : public IServiceProbe
{
  public:
    [[nodiscard]] ServiceCapabilities capabilities() const override
    {
        return {};
    }

    [[nodiscard]] std::vector<ServiceInfo> enumerate() override
    {
        return {};
    }
};

} // namespace Platform
