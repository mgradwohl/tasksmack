#pragma once

// System Information's root-only facts through the privileged helper (#2310; #2181): an ISystemInfoProbe
// decorator over the in-process probe and IPrivilegedBroker. Every read is the inner probe's. When it
// reports a fact readable by root only (Linux: the serials, the UUID, the SMBIOS entry point, the SMBIOS
// table behind Memory modules) and the helper is running, the decorator fetches that file through it and
// fills the fact with the parsers the in-process read uses. authorize() asks for the password and starts
// the helper, for the page's "Authenticate to read". The helper reads only the files Broker::FirmwareFile
// names, never a path the client sends.

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace Platform
{

class BrokeredSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    BrokeredSystemInfoProbe(std::unique_ptr<ISystemInfoProbe> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] SystemInfoCapabilities capabilities() const override;
    [[nodiscard]] OsInfo readOs() override;
    [[nodiscard]] FirmwareInfo readFirmware() override;
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override;
    [[nodiscard]] CommitPagingInfo readCommitPaging() override;
    [[nodiscard]] StorageInfo readStorage() override;
    [[nodiscard]] GraphicsInfo readGraphics() override;
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override;
    [[nodiscard]] SensorsInfo readSensors() override;
    [[nodiscard]] DevicesInfo readDevices() override;
    [[nodiscard]] DriversInfo readDrivers() override;
    [[nodiscard]] CrashesInfo readCrashes() override;
    [[nodiscard]] NetworkAdaptersInfo readNetworkAdapters(bool includeWifiDetails) override;
    [[nodiscard]] PublicIpInfo readPublicIp() override;
    [[nodiscard]] BootPerformanceInfo readBootPerformance() override;
    [[nodiscard]] bool canReadAuthorized() const override;
    bool authorize(const std::stop_token& stopToken) override;

  private:
    /// The file's reply through the running helper, or nullopt when there is no answer
    [[nodiscard]] std::optional<Broker::FirmwareFileReply> fetch(Broker::FirmwareFile file) const;

    std::unique_ptr<ISystemInfoProbe> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// @p probe decorated with @p broker, or @p probe as it is when there is no broker.
[[nodiscard]] std::unique_ptr<ISystemInfoProbe> withPrivilegedBroker(std::unique_ptr<ISystemInfoProbe> probe,
                                                                     std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
