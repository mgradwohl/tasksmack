#include "Platform/BrokeredSystemInfoProbe.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/SmbiosParser.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{

namespace
{

/// A one-line sysfs attribute's bytes without the trailing newline and spaces, as the in-process read
/// (LinuxOsInfo::readLine) gives it.
[[nodiscard]] std::string attributeText(const std::vector<std::uint8_t>& bytes)
{
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    {
        text.pop_back();
    }
    return text;
}

} // namespace

std::unique_ptr<ISystemInfoProbe> withPrivilegedBroker(std::unique_ptr<ISystemInfoProbe> probe, std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || probe == nullptr)
    {
        return probe;
    }
    return std::make_unique<BrokeredSystemInfoProbe>(std::move(probe), std::move(broker));
}

BrokeredSystemInfoProbe::BrokeredSystemInfoProbe(std::unique_ptr<ISystemInfoProbe> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

SystemInfoCapabilities BrokeredSystemInfoProbe::capabilities() const
{
    SystemInfoCapabilities capabilities = m_Inner->capabilities();
    capabilities.canReadAuthorized = canReadAuthorized();
    return capabilities;
}

OsInfo BrokeredSystemInfoProbe::readOs()
{
    return m_Inner->readOs();
}

FirmwareInfo BrokeredSystemInfoProbe::readFirmware()
{
    FirmwareInfo info = m_Inner->readFirmware();
    if (info.identifiersNeedAdmin)
    {
        bool allAnswered = true;
        const auto fill = [&](Broker::FirmwareFile file, std::string& field)
        {
            const std::optional<Broker::FirmwareFileReply> reply = fetch(file);
            if (!reply.has_value() || reply->status == Broker::FirmwareFileStatus::Unreadable)
            {
                allAnswered = false;
            }
            else if (reply->status == Broker::FirmwareFileStatus::Ok && field.empty())
            {
                field = attributeText(reply->bytes);
            }
        };
        fill(Broker::FirmwareFile::ProductSerial, info.systemSerial);
        fill(Broker::FirmwareFile::ProductUuid, info.systemUuid);
        fill(Broker::FirmwareFile::BoardSerial, info.boardSerial);
        info.identifiersNeedAdmin = !allAnswered;
    }
    if (info.smbiosVersionNeedsAdmin)
    {
        const std::optional<Broker::FirmwareFileReply> reply = fetch(Broker::FirmwareFile::SmbiosEntryPoint);
        if (reply.has_value() && reply->status == Broker::FirmwareFileStatus::Ok)
        {
            info.smbiosVersion = Smbios::entryPointVersion(reply->bytes);
            info.smbiosVersionNeedsAdmin = false;
        }
    }
    return info;
}

MemoryModulesInfo BrokeredSystemInfoProbe::readMemoryModules()
{
    MemoryModulesInfo info = m_Inner->readMemoryModules();
    if (info.tableNeedsAdmin)
    {
        const std::optional<Broker::FirmwareFileReply> reply = fetch(Broker::FirmwareFile::SmbiosTable);
        if (reply.has_value() && reply->status == Broker::FirmwareFileStatus::Ok)
        {
            info.tableNeedsAdmin = false;
            Smbios::decodeMemoryTable(reply->bytes, info);
        }
    }
    return info;
}

CommitPagingInfo BrokeredSystemInfoProbe::readCommitPaging()
{
    return m_Inner->readCommitPaging();
}

StorageInfo BrokeredSystemInfoProbe::readStorage()
{
    return m_Inner->readStorage();
}

GraphicsInfo BrokeredSystemInfoProbe::readGraphics()
{
    return m_Inner->readGraphics();
}

PlatformSecurityInfo BrokeredSystemInfoProbe::readPlatformSecurity()
{
    return m_Inner->readPlatformSecurity();
}

SensorsInfo BrokeredSystemInfoProbe::readSensors()
{
    return m_Inner->readSensors();
}

DevicesInfo BrokeredSystemInfoProbe::readDevices()
{
    return m_Inner->readDevices();
}

DriversInfo BrokeredSystemInfoProbe::readDrivers()
{
    return m_Inner->readDrivers();
}

CrashesInfo BrokeredSystemInfoProbe::readCrashes()
{
    CrashesInfo info = m_Inner->readCrashes();
    if (!info.otherUsersHidden || m_Broker == nullptr || !m_Broker->connected())
    {
        return info;
    }
    // Every user's crashes, as the helper reads them as root
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::CrashesRequest{}, std::stop_token{});
    if (response.has_value())
    {
        if (const auto* reply = std::get_if<Broker::CrashesReply>(&*response))
        {
            return reply->info;
        }
    }
    return info;
}

NetworkAdaptersInfo BrokeredSystemInfoProbe::readNetworkAdapters(bool includeWifiDetails)
{
    return m_Inner->readNetworkAdapters(includeWifiDetails);
}

PublicIpInfo BrokeredSystemInfoProbe::readPublicIp()
{
    return m_Inner->readPublicIp();
}

BootPerformanceInfo BrokeredSystemInfoProbe::readBootPerformance()
{
    return m_Inner->readBootPerformance();
}

bool BrokeredSystemInfoProbe::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

bool BrokeredSystemInfoProbe::authorize(const std::stop_token& stopToken)
{
    return canReadAuthorized() && (m_Broker->connected() || m_Broker->connect(stopToken).has_value());
}

std::optional<Broker::FirmwareFileReply> BrokeredSystemInfoProbe::fetch(Broker::FirmwareFile file) const
{
    if (m_Broker == nullptr || !m_Broker->connected())
    {
        return std::nullopt;
    }
    const std::expected<Broker::Response, std::string> response =
        m_Broker->call(Broker::FirmwareFileRequest{.file = file}, std::stop_token{});
    if (response.has_value())
    {
        if (const auto* reply = std::get_if<Broker::FirmwareFileReply>(&*response))
        {
            return *reply;
        }
    }
    return std::nullopt;
}

} // namespace Platform
