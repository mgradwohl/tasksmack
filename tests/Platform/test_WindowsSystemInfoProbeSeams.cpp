/// @file test_WindowsSystemInfoProbeSeams.cpp
/// @brief WindowsSystemInfoProbe's OS, session and firmware reads against fake Win32 calls (#1722): every
/// value read, then each call failing on its own, which leaves only its own field empty, and the
/// grow-and-retry paths of the sized reads. The fakes read nothing from the system, except the SMBIOS
/// table, which the working fake passes through from the real (read-only) GetSystemFirmwareTable.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsDevices.h"
#include "Platform/Windows/WindowsDrivers.h"
#include "Platform/Windows/WindowsGraphics.h"
#include "Platform/Windows/WindowsSystemInfoProbe.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <lm.h>
// clang-format on

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

/// What the fakes answer. Reset per test to a machine where everything reads.
struct FakeOs
{
    std::map<std::wstring, std::variant<std::wstring, DWORD>> versionValues{
        {L"CurrentBuild", std::wstring(L"26100")},
        {L"ProductName", std::wstring(L"Windows 10 Pro")}, // made "Windows 11" from the build
        {L"EditionID", std::wstring(L"Professional")},
        {L"DisplayVersion", std::wstring(L"24H2")},
        {L"ReleaseId", std::wstring(L"2009")},
        {L"UBR", DWORD{4652}},
        {L"InstallDate", DWORD{1700000000}},
    };
    bool secondStringReadFails = false; ///< The value changed between RegGetValueW's size and data calls
    bool wow64Fails = false;
    std::wstring computerName = L"BUILD-BOX";
    bool computerNameFails = false;
    std::wstring userName = L"BUILD-BOX\\Tester";
    LONG userNameMoreData = 0; ///< Calls left that ask for more room than the buffer has (each then fails)
    NET_API_STATUS joinResult = NERR_Success;
    NETSETUP_JOIN_STATUS joinStatus = NetSetupWorkgroupName;
    std::wstring joinName = L"WORKGROUP";
    int joinBuffersFreed = 0;
    int localeLength = 6; ///< "en-US" and its terminator; 0 for a failure
    DWORD zoneId = TIME_ZONE_ID_DAYLIGHT;
    bool zoneHasKeyName = true;
    UINT directoryNeeds = 0; ///< A directory length past MAX_PATH: the buffer must grow first
    bool directoryStaysTooLong = false;
    bool directoryFails = false;
    BOOL firmwareTypeRead = TRUE;
    FIRMWARE_TYPE firmwareType = FirmwareTypeUefi;
    UINT smbiosSize = 0;            ///< 0: pass the real table through
    bool smbiosAnswersOnce = false; ///< The size call answers; the data call then fails
    bool smbiosAbsent = false;      ///< No table at all: the size call fails
    POWER_PLATFORM_ROLE role = PlatformRoleMobile;
};

FakeOs& fake()
{
    static FakeOs instance;
    return instance;
}

LSTATUS WINAPI fakeRegGetValue(HKEY root, LPCWSTR subkey, LPCWSTR name, DWORD flags, LPDWORD /*type*/, PVOID data, LPDWORD bytes)
{
    EXPECT_EQ(root, HKEY_LOCAL_MACHINE);
    EXPECT_STREQ(subkey, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    const auto it = fake().versionValues.find(name);
    if (it == fake().versionValues.end())
    {
        return ERROR_FILE_NOT_FOUND;
    }
    if (const auto* text = std::get_if<std::wstring>(&it->second); text != nullptr && (flags & RRF_RT_REG_SZ) != 0)
    {
        const auto needed = static_cast<DWORD>((text->size() + 1) * sizeof(wchar_t));
        if (data != nullptr)
        {
            if (fake().secondStringReadFails || *bytes < needed)
            {
                return ERROR_MORE_DATA;
            }
            std::memcpy(data, text->c_str(), needed);
        }
        *bytes = text->empty() ? 0 : needed; // an empty value reads as no bytes
        return ERROR_SUCCESS;
    }
    if (const auto* number = std::get_if<DWORD>(&it->second); number != nullptr && (flags & RRF_RT_REG_DWORD) != 0)
    {
        std::memcpy(data, number, sizeof(DWORD));
        *bytes = sizeof(DWORD);
        return ERROR_SUCCESS;
    }
    return ERROR_UNSUPPORTED_TYPE;
}

BOOL WINAPI fakeWow64(HANDLE /*process*/, USHORT* processMachine, USHORT* nativeMachine)
{
    if (fake().wow64Fails)
    {
        return FALSE;
    }
    *processMachine = 0;     // IMAGE_FILE_MACHINE_UNKNOWN: not under WOW64
    *nativeMachine = 0xAA64; // IMAGE_FILE_MACHINE_ARM64
    return TRUE;
}

/// GetComputerNameExW's and GetUserNameExW's contract: on ERROR_MORE_DATA, `size` is the room needed.
BOOL fillName(const std::wstring& text, LPWSTR buffer, PULONG size)
{
    if (*size <= text.size())
    {
        *size = static_cast<ULONG>(text.size() + 1);
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    std::memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    *size = static_cast<ULONG>(text.size());
    return TRUE;
}

BOOL fakeComputerName(LPWSTR buffer, PULONG size)
{
    if (fake().computerNameFails)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return fillName(fake().computerName, buffer, size);
}

BOOL fakeUserName(LPWSTR buffer, PULONG size)
{
    if (fake().userNameMoreData > 0)
    {
        --fake().userNameMoreData;
        *size += MAX_PATH; // always more than offered
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    return fillName(fake().userName, buffer, size);
}

/// @p text and a terminator into @p destination, which must have room for both.
void copyText(std::span<wchar_t> destination, std::wstring_view text)
{
    ASSERT_GT(destination.size(), text.size());
    text.copy(destination.data(), text.size());
    destination[text.size()] = L'\0';
}

DWORD fakeJoin(LPCWSTR server, LPWSTR* name, DWORD* status)
{
    EXPECT_EQ(server, nullptr);
    if (fake().joinResult != NERR_Success)
    {
        return fake().joinResult;
    }
    *name = fake().joinName.data();
    *status = static_cast<DWORD>(fake().joinStatus);
    return NERR_Success;
}

NET_API_STATUS NET_API_FUNCTION fakeBufferFree(LPVOID buffer)
{
    EXPECT_EQ(buffer, static_cast<LPVOID>(fake().joinName.data()));
    ++fake().joinBuffersFreed;
    return NERR_Success;
}

int WINAPI fakeLocale(LPWSTR buffer, int chars)
{
    if (fake().localeLength == 0)
    {
        return 0;
    }
    EXPECT_GE(chars, fake().localeLength);
    copyText(std::span(buffer, static_cast<std::size_t>(chars)), L"en-US");
    return fake().localeLength;
}

DWORD WINAPI fakeTimeZone(PDYNAMIC_TIME_ZONE_INFORMATION zone)
{
    zone->Bias = 480; // UTC-8
    zone->StandardBias = 0;
    zone->DaylightBias = -60;
    copyText(zone->StandardName, L"Pacific Standard Time");
    copyText(zone->TimeZoneKeyName, fake().zoneHasKeyName ? L"Pacific Standard Time" : L"");
    return fake().zoneId;
}

UINT directory(const wchar_t* path, LPWSTR buffer, UINT size)
{
    if (fake().directoryFails)
    {
        return 0;
    }
    const std::wstring text = fake().directoryNeeds > 0 ? std::wstring(fake().directoryNeeds - 1, L'd') : std::wstring(path);
    if (fake().directoryStaysTooLong)
    {
        return size + 1; // whatever room it's given, it needs more
    }
    if (size <= text.size())
    {
        return static_cast<UINT>(text.size() + 1); // the room needed, terminator included
    }
    std::memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    return static_cast<UINT>(text.size());
}

UINT WINAPI fakeSystemDirectory(LPWSTR buffer, UINT size)
{
    return directory(L"C:\\Windows\\system32", buffer, size);
}

UINT WINAPI fakeWindowsDirectory(LPWSTR buffer, UINT size)
{
    return directory(L"C:\\Windows", buffer, size);
}

BOOL WINAPI fakeFirmwareType(PFIRMWARE_TYPE type)
{
    *type = fake().firmwareType;
    return fake().firmwareTypeRead;
}

UINT WINAPI fakeFirmwareTable(DWORD provider, DWORD id, PVOID buffer, DWORD size)
{
    if (fake().smbiosAbsent)
    {
        return 0;
    }
    if (fake().smbiosSize == 0)
    {
        return GetSystemFirmwareTable(provider, id, buffer, size); // read-only
    }
    if (buffer == nullptr)
    {
        return fake().smbiosSize;
    }
    return fake().smbiosAnswersOnce ? 0 : size + 1; // failed, or the table grew
}

POWER_PLATFORM_ROLE WINAPI fakeRole(ULONG /*version*/)
{
    return fake().role;
}

[[nodiscard]] SystemInfoFunctions fakeFunctions()
{
    return {
        .regGetValue = &fakeRegGetValue,
        .isWow64Process2 = &fakeWow64,
        .computerName = &fakeComputerName,
        .userName = &fakeUserName,
        .netGetJoinInformation = &fakeJoin,
        .netApiBufferFree = &fakeBufferFree,
        .getUserDefaultLocaleName = &fakeLocale,
        .getDynamicTimeZoneInformation = &fakeTimeZone,
        .getSystemDirectory = &fakeSystemDirectory,
        .getSystemWindowsDirectory = &fakeWindowsDirectory,
        .getFirmwareType = &fakeFirmwareType,
        .getSystemFirmwareTable = &fakeFirmwareTable,
        .powerDeterminePlatformRoleEx = &fakeRole,
    };
}

class WindowsSystemInfoProbeSeamsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeOs{};
    }
    [[nodiscard]] static OsInfo readOs()
    {
        WindowsSystemInfoProbe probe(fakeFunctions());
        return probe.readOs();
    }
};

TEST_F(WindowsSystemInfoProbeSeamsTest, ReadsEveryOsField)
{
    const OsInfo info = readOs();
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_EQ(info.name, "Windows 11 Pro");
    EXPECT_EQ(info.version, "24H2");
    EXPECT_EQ(info.build, "26100.4652");
    EXPECT_EQ(info.installUnixSeconds, 1700000000U);
    EXPECT_EQ(info.architecture, "ARM64");
    EXPECT_GT(info.bootUnixSeconds, 0U);
    EXPECT_EQ(info.computerName, "BUILD-BOX");
    EXPECT_EQ(info.domainOrWorkgroup, "WORKGROUP");
    EXPECT_FALSE(info.joinedToDomain);
    EXPECT_EQ(fake().joinBuffersFreed, 1);
    EXPECT_EQ(info.userName, "BUILD-BOX\\Tester");
    EXPECT_EQ(info.locale, "en-US");
    EXPECT_EQ(info.timeZone, "Pacific Standard Time");
    EXPECT_EQ(info.utcOffsetMinutes, std::optional(-420)); // daylight time: UTC-7
    EXPECT_EQ(info.systemDirectory, "C:\\Windows\\system32");
    EXPECT_EQ(info.windowsDirectory, "C:\\Windows");
}

TEST_F(WindowsSystemInfoProbeSeamsTest, EachFailedReadLeavesOnlyItsFieldEmpty)
{
    fake().versionValues.clear();
    fake().wow64Fails = true;
    fake().computerNameFails = true;
    fake().joinResult = ERROR_ACCESS_DENIED;
    fake().userNameMoreData = 2; // still too small after the one retry
    fake().localeLength = 0;
    fake().zoneId = TIME_ZONE_ID_INVALID;
    fake().directoryFails = true;

    const OsInfo info = readOs();
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_GT(info.bootUnixSeconds, 0U) << "the boot time needs none of them";
    EXPECT_TRUE(info.name.empty());
    EXPECT_TRUE(info.version.empty());
    EXPECT_TRUE(info.build.empty());
    EXPECT_EQ(info.installUnixSeconds, 0U);
    EXPECT_TRUE(info.architecture.empty());
    EXPECT_TRUE(info.computerName.empty());
    EXPECT_TRUE(info.domainOrWorkgroup.empty());
    EXPECT_EQ(fake().joinBuffersFreed, 0) << "no buffer to free when the call fails";
    EXPECT_TRUE(info.userName.empty());
    EXPECT_TRUE(info.locale.empty());
    EXPECT_TRUE(info.timeZone.empty());
    EXPECT_FALSE(info.utcOffsetMinutes.has_value());
    EXPECT_TRUE(info.systemDirectory.empty());
    EXPECT_TRUE(info.windowsDirectory.empty());
}

TEST_F(WindowsSystemInfoProbeSeamsTest, VersionValuesFallBackAndFailAlone)
{
    // No DisplayVersion: ReleaseId. No ProductName: built from EditionID. No UBR: the build alone.
    fake().versionValues.erase(L"DisplayVersion");
    fake().versionValues.erase(L"ProductName");
    fake().versionValues.erase(L"UBR");
    fake().versionValues[L"ReleaseId"] = std::wstring{}; // present but empty
    OsInfo info = readOs();
    EXPECT_EQ(info.name, "Windows 11 Professional");
    EXPECT_TRUE(info.version.empty());
    EXPECT_EQ(info.build, "26100");

    // A string that changes between the size and data reads is left empty; DWORDs still read.
    fake().secondStringReadFails = true;
    info = readOs();
    EXPECT_TRUE(info.build.empty());
    EXPECT_EQ(info.installUnixSeconds, 1700000000U);
}

TEST_F(WindowsSystemInfoProbeSeamsTest, SizedReadsGrowTheirBuffersOnce)
{
    fake().computerName = std::wstring(MAX_PATH + 40, L'n'); // too long for the first buffer
    fake().userNameMoreData = 1;                             // asks for more once, then fits
    fake().directoryNeeds = MAX_PATH + 20;
    const OsInfo info = readOs();
    EXPECT_EQ(info.computerName, std::string(MAX_PATH + 40, 'n'));
    EXPECT_EQ(info.userName, "BUILD-BOX\\Tester");
    EXPECT_EQ(info.systemDirectory.size(), MAX_PATH + 19U);
    EXPECT_EQ(info.windowsDirectory.size(), MAX_PATH + 19U);

    // A directory still too long for the grown buffer is left empty.
    fake().directoryStaysTooLong = true;
    const OsInfo stillTooLong = readOs();
    EXPECT_TRUE(stillTooLong.systemDirectory.empty());
    EXPECT_TRUE(stillTooLong.windowsDirectory.empty());
}

TEST_F(WindowsSystemInfoProbeSeamsTest, JoinStatesAndTimeZones)
{
    fake().joinStatus = NetSetupDomainName;
    fake().zoneId = TIME_ZONE_ID_STANDARD;
    OsInfo info = readOs();
    EXPECT_EQ(info.domainOrWorkgroup, "WORKGROUP");
    EXPECT_TRUE(info.joinedToDomain);
    EXPECT_EQ(info.utcOffsetMinutes, std::optional(-480));

    // Not joined: no name shown, the buffer still freed. No daylight rules: the bias alone; no key
    // name: the standard name.
    fake().joinStatus = NetSetupUnjoined;
    fake().zoneId = TIME_ZONE_ID_UNKNOWN;
    fake().zoneHasKeyName = false;
    fake().joinBuffersFreed = 0;
    info = readOs();
    EXPECT_TRUE(info.domainOrWorkgroup.empty());
    EXPECT_FALSE(info.joinedToDomain);
    EXPECT_EQ(fake().joinBuffersFreed, 1);
    EXPECT_EQ(info.utcOffsetMinutes, std::optional(-480));
    EXPECT_EQ(info.timeZone, "Pacific Standard Time");
}

TEST_F(WindowsSystemInfoProbeSeamsTest, FirmwareReadsAndFailures)
{
    WindowsSystemInfoProbe probe(fakeFunctions());
    FirmwareInfo info = probe.readFirmware();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.firmwareMode, FirmwareMode::Uefi);
    EXPECT_EQ(info.platformRole, "Mobile");

    fake().firmwareType = FirmwareTypeBios;
    EXPECT_EQ(probe.readFirmware().firmwareMode, FirmwareMode::Legacy);

    // Nothing read: the section is still there, with nothing in it.
    fake().firmwareTypeRead = FALSE;
    fake().smbiosSize = 64;
    fake().smbiosAnswersOnce = true;
    fake().role = PlatformRoleUnspecified;
    info = probe.readFirmware();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.firmwareMode, FirmwareMode::Unknown);
    EXPECT_TRUE(info.systemManufacturer.empty());
    EXPECT_TRUE(info.biosVersion.empty());
    EXPECT_TRUE(info.smbiosVersion.empty());
    EXPECT_TRUE(info.chassisType.empty());
    EXPECT_TRUE(info.platformRole.empty());

    // A table that grew between the two calls is not read either.
    fake().smbiosAnswersOnce = false;
    EXPECT_TRUE(probe.readFirmware().smbiosVersion.empty());
}

TEST_F(WindowsSystemInfoProbeSeamsTest, MemoryModulesWithoutTheTable)
{
    WindowsSystemInfoProbe probe(fakeFunctions());
    fake().smbiosAbsent = true;
    const MemoryModulesInfo info = probe.readMemoryModules();
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.tableRead);
    EXPECT_TRUE(info.modules.empty());
    EXPECT_GT(info.usableBytes, 0U) << "installed and usable memory don't need the table";
}

TEST_F(WindowsSystemInfoProbeSeamsTest, SystemFunctionsAreAllSet)
{
    const SystemInfoFunctions fns = WindowsSystemInfoProbe::systemFunctions();
    EXPECT_NE(fns.regGetValue, nullptr);
    EXPECT_NE(fns.isWow64Process2, nullptr);
    EXPECT_NE(fns.computerName, nullptr);
    EXPECT_NE(fns.userName, nullptr);
    EXPECT_NE(fns.netGetJoinInformation, nullptr);
    EXPECT_NE(fns.netApiBufferFree, nullptr);
    EXPECT_NE(fns.getUserDefaultLocaleName, nullptr);
    EXPECT_NE(fns.getDynamicTimeZoneInformation, nullptr);
    EXPECT_NE(fns.getSystemDirectory, nullptr);
    EXPECT_NE(fns.getSystemWindowsDirectory, nullptr);
    EXPECT_NE(fns.getFirmwareType, nullptr);
    EXPECT_NE(fns.getSystemFirmwareTable, nullptr);
    EXPECT_NE(fns.powerDeterminePlatformRoleEx, nullptr);
}

// The real readers the Graphics, Devices and Drivers sections compose, given inputs they can't read:
// each fails to its documented empty answer rather than throwing or reading something else.
TEST(WindowsSystemInfoProbeReadersTest, GraphicsAndDeviceReadersFailEmpty)
{
    const WindowsGraphics::Functions graphics = WindowsSystemInfoProbe::graphicsFunctions();
    EXPECT_TRUE(graphics.readMonitorEdid(L"\\\\.\\TASKSMACK-1722-NO-DISPLAY").empty());
    const LUID noAdapter{.LowPart = 0xFFFFFFFFU, .HighPart = 0x7FFFFFFF};
    EXPECT_FALSE(graphics.adapterType(noAdapter).has_value());
    EXPECT_FALSE(graphics.pciLocation(noAdapter).has_value());

    const WindowsDevices::Functions devices = WindowsSystemInfoProbe::deviceFunctions();
    EXPECT_TRUE(devices.readHubPorts("USB\\TASKSMACK-1722\\NO-SUCH-HUB", {1, 2}).empty());
}

TEST(WindowsSystemInfoProbeReadersTest, DriverReadersFailEmpty)
{
    const WindowsDrivers::Functions drivers = WindowsSystemInfoProbe::driverFunctions();
    const std::string missing = "C:\\TaskSmack-1722-no-such-dir\\no-such-driver.sys";
    EXPECT_FALSE(drivers.fileStamp(missing).has_value());
    const WindowsDrivers::FileVersionRecord version = drivers.readFileVersion(missing);
    EXPECT_TRUE(version.version.empty());
    EXPECT_TRUE(version.company.empty());
    EXPECT_NE(drivers.verifyEmbeddedSignature(missing).result, 0);
    const std::optional<WindowsDrivers::TrustCheck> catalog = drivers.verifyCatalogSignature(missing);
    ASSERT_TRUE(catalog.has_value()) << "a file that can't be opened is an error, not 'in no catalog'";
    if (catalog.has_value())
    {
        EXPECT_EQ(catalog->result, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        EXPECT_TRUE(catalog->signer.empty());
    }

    // Win32 errors read from the system's messages, an NTSTATUS (a driver's exit code) from ntdll's.
    EXPECT_FALSE(drivers.errorMessage(ERROR_ACCESS_DENIED).empty());
    EXPECT_FALSE(drivers.errorMessage(0xC0000034U).empty()); // STATUS_OBJECT_NAME_NOT_FOUND
    EXPECT_TRUE(drivers.errorMessage(0x2FFFFFFFU).empty());  // neither has it
}

TEST(WindowsSystemInfoProbeReadersTest, AnUnsignedFileIsInNoCatalog)
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / std::format("tasksmack-1722-{}-unsigned.sys", GetCurrentProcessId());
    std::ofstream(file, std::ios::binary) << "not a driver";
    const std::string path = WinString::wideToUtf8(file.wstring());

    const WindowsDrivers::Functions drivers = WindowsSystemInfoProbe::driverFunctions();
    EXPECT_TRUE(drivers.fileStamp(path).has_value());
    EXPECT_TRUE(drivers.readFileVersion(path).version.empty()) << "no version resource";
    EXPECT_NE(drivers.verifyEmbeddedSignature(path).result, 0);
    EXPECT_FALSE(drivers.verifyCatalogSignature(path).has_value());

    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

} // namespace
} // namespace Platform
