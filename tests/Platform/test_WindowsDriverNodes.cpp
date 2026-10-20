/// @file test_WindowsDriverNodes.cpp
/// @brief WindowsGraphics::listDriverNodes() (#2565), the one SetupAPI lister behind the GPU probe's
/// driver versions (#1799) and System Information's Graphics section (#1519): against fake SetupAPI
/// calls (the filter passed through, each property read or missing, a wrong property type, an
/// over-long hardware id, the list destroyed exactly once), and against this machine's SetupAPI.

#include "Platform/Windows/WindowsDriverNodes.h"
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
#include <setupapi.h>
#include <devpropdef.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace Platform::WindowsGraphics
{
namespace
{

/// One fake device node: each property is reported only when it has a value.
struct FakeNode
{
    std::optional<std::wstring> hardwareIds; ///< REG_MULTI_SZ: strings separated by '\0'
    std::optional<DWORD> bus;
    std::optional<DWORD> address;
    std::optional<std::wstring> version;
    DEVPROPTYPE versionType = DEVPROP_TYPE_STRING;
    std::optional<FILETIME> date;
    DEVPROPTYPE dateType = DEVPROP_TYPE_FILETIME;
};

/// The fakes' answers and what they were asked, reset per test.
struct FakeSetupApi
{
    bool failOpen = false;
    std::vector<FakeNode> nodes;
    // The last SetupDiGetClassDevsW call
    const GUID* classGuid = nullptr;
    std::wstring enumerator;
    bool enumeratorNull = false;
    DWORD flags = 0;
    int opens = 0;
    int destroys = 0;
    HDEVINFO destroyed = nullptr;
    std::vector<DWORD> textBufferBytes; ///< The buffer size of every text property read
};

FakeSetupApi& fake()
{
    static FakeSetupApi instance;
    return instance;
}

// NOLINTNEXTLINE(performance-no-int-to-ptr) - a token handle the fakes compare, never dereferenced
const auto FAKE_LIST = reinterpret_cast<HDEVINFO>(static_cast<std::uintptr_t>(0x2565));

HDEVINFO WINAPI fakeGetClassDevs(const GUID* classGuid, PCWSTR enumerator, HWND /*parent*/, DWORD flags)
{
    ++fake().opens;
    fake().classGuid = classGuid;
    fake().enumeratorNull = enumerator == nullptr;
    fake().enumerator = enumerator != nullptr ? enumerator : L"";
    fake().flags = flags;
    return fake().failOpen ? INVALID_HANDLE_VALUE : FAKE_LIST;
}

BOOL WINAPI fakeDestroy(HDEVINFO list)
{
    ++fake().destroys;
    fake().destroyed = list;
    return TRUE;
}

BOOL WINAPI fakeEnum(HDEVINFO list, DWORD index, PSP_DEVINFO_DATA device)
{
    if (list != FAKE_LIST || device == nullptr || device->cbSize != sizeof(SP_DEVINFO_DATA) || index >= fake().nodes.size())
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    device->DevInst = index; // which node the property reads are for
    return TRUE;
}

/// Copies @p text into @p buffer as SetupAPI does: fails with ERROR_INSUFFICIENT_BUFFER when it doesn't fit.
BOOL copyText(const std::wstring& text, PBYTE buffer, DWORD size)
{
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (bytes > size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(buffer, text.c_str(), bytes);
    return TRUE;
}

BOOL WINAPI fakeRegistryProperty(
    HDEVINFO /*list*/, PSP_DEVINFO_DATA device, DWORD property, PDWORD /*type*/, PBYTE buffer, DWORD size, PDWORD /*needed*/)
{
    const FakeNode& node = fake().nodes.at(device->DevInst);
    const auto copyDword = [&](const std::optional<DWORD>& value) -> BOOL
    {
        if (!value.has_value() || size < sizeof(DWORD))
        {
            return FALSE;
        }
        std::memcpy(buffer, &*value, sizeof(DWORD));
        return TRUE;
    };
    switch (property)
    {
    case SPDRP_HARDWAREID:
        fake().textBufferBytes.push_back(size);
        return node.hardwareIds.has_value() ? copyText(*node.hardwareIds, buffer, size) : FALSE;
    case SPDRP_BUSNUMBER:
        return copyDword(node.bus);
    case SPDRP_ADDRESS:
        return copyDword(node.address);
    default:
        return FALSE;
    }
}

bool sameKey(const DEVPROPKEY& a, const DEVPROPKEY& b)
{
    return a.pid == b.pid && std::memcmp(&a.fmtid, &b.fmtid, sizeof(GUID)) == 0;
}

BOOL WINAPI fakeProperty(HDEVINFO /*list*/,
                         PSP_DEVINFO_DATA device,
                         const DEVPROPKEY* key,
                         DEVPROPTYPE* type,
                         PBYTE buffer,
                         DWORD size,
                         PDWORD /*needed*/,
                         DWORD /*flags*/)
{
    const FakeNode& node = fake().nodes.at(device->DevInst);
    if (sameKey(*key, DRIVER_VERSION_KEY))
    {
        fake().textBufferBytes.push_back(size);
        if (!node.version.has_value())
        {
            return FALSE;
        }
        *type = node.versionType;
        return copyText(*node.version, buffer, size);
    }
    if (sameKey(*key, DRIVER_DATE_KEY))
    {
        if (!node.date.has_value() || size < sizeof(FILETIME))
        {
            return FALSE;
        }
        *type = node.dateType;
        std::memcpy(buffer, &*node.date, sizeof(FILETIME));
        return TRUE;
    }
    return FALSE;
}

SetupApiFunctions fakeApi()
{
    return {
        .getClassDevs = &fakeGetClassDevs,
        .destroyDeviceInfoList = &fakeDestroy,
        .enumDeviceInfo = &fakeEnum,
        .getRegistryProperty = &fakeRegistryProperty,
        .getProperty = &fakeProperty,
    };
}

class WindowsDriverNodesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeSetupApi{};
    }
};

FakeNode arcNode()
{
    return {
        .hardwareIds = std::wstring(L"PCI\\VEN_8086&DEV_7D51&SUBSYS_0C9F1028&REV_08") + L'\0' + L"PCI\\VEN_8086&DEV_7D51&SUBSYS_0C9F1028",
        .bus = 0,
        .address = 0x20000,
        .version = L"32.0.101.8247",
        .date = FILETIME{.dwLowDateTime = 0x5A9F8000, .dwHighDateTime = 0x01DB1F2C},
    };
}

} // namespace

TEST_F(WindowsDriverNodesTest, PassesTheFilterThroughAndDestroysTheListOnce)
{
    fake().nodes = {arcNode()};
    const std::vector<DriverNode> nodes = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES, fakeApi());
    EXPECT_EQ(fake().classGuid, nullptr);
    EXPECT_FALSE(fake().enumeratorNull);
    EXPECT_EQ(fake().enumerator, L"PCI");
    EXPECT_EQ(fake().flags, static_cast<DWORD>(DIGCF_PRESENT | DIGCF_ALLCLASSES));
    EXPECT_EQ(nodes.size(), 1U);
    EXPECT_EQ(fake().opens, 1);
    EXPECT_EQ(fake().destroys, 1);
    EXPECT_EQ(fake().destroyed, FAKE_LIST);

    (void) listDriverNodes(&DISPLAY_CLASS_GUID, nullptr, DIGCF_PRESENT, fakeApi());
    EXPECT_EQ(fake().classGuid, &DISPLAY_CLASS_GUID);
    EXPECT_TRUE(fake().enumeratorNull);
    EXPECT_EQ(fake().flags, static_cast<DWORD>(DIGCF_PRESENT));
    EXPECT_EQ(fake().destroys, 2);
}

TEST_F(WindowsDriverNodesTest, ReadsEveryPropertyOfEveryNode)
{
    const FakeNode npu{
        .hardwareIds = std::wstring(L"PCI\\VEN_8086&DEV_B03E&SUBSYS_0C9F1028&REV_04") + L'\0',
        .bus = 0,
        .address = 0xB0000,
        .version = L"32.0.100.4297",
        .date = FILETIME{.dwLowDateTime = 1, .dwHighDateTime = 2},
    };
    fake().nodes = {arcNode(), npu};
    const std::vector<DriverNode> nodes = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES, fakeApi());
    ASSERT_EQ(nodes.size(), 2U);
    // The first string of the REG_MULTI_SZ only
    EXPECT_EQ(nodes[0].hardwareId, "PCI\\VEN_8086&DEV_7D51&SUBSYS_0C9F1028&REV_08");
    EXPECT_EQ(nodes[0].bus, 0U);
    EXPECT_EQ(nodes[0].address, 0x20000U);
    EXPECT_EQ(nodes[0].driverVersion, "32.0.101.8247");
    ASSERT_TRUE(nodes[0].driverDate.has_value());
    EXPECT_EQ(nodes[0].driverDate.value_or(FILETIME{}).dwLowDateTime, 0x5A9F8000U);
    EXPECT_EQ(nodes[0].driverDate.value_or(FILETIME{}).dwHighDateTime, 0x01DB1F2CU);
    EXPECT_EQ(nodes[1].hardwareId, "PCI\\VEN_8086&DEV_B03E&SUBSYS_0C9F1028&REV_04");
    EXPECT_EQ(nodes[1].address, 0xB0000U);
    EXPECT_EQ(nodes[1].driverVersion, "32.0.100.4297");
    ASSERT_TRUE(nodes[1].driverDate.has_value());
    EXPECT_EQ(nodes[1].driverDate.value_or(FILETIME{}).dwLowDateTime, 1U);
}

TEST_F(WindowsDriverNodesTest, AMissingPropertyLeavesOnlyItsFieldEmpty)
{
    fake().nodes = {FakeNode{}, arcNode()};
    fake().nodes[1].bus.reset();
    fake().nodes[1].date.reset();
    const std::vector<DriverNode> nodes = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES, fakeApi());
    ASSERT_EQ(nodes.size(), 2U); // a node with nothing readable is still listed
    EXPECT_TRUE(nodes[0].hardwareId.empty());
    EXPECT_FALSE(nodes[0].bus.has_value());
    EXPECT_FALSE(nodes[0].address.has_value());
    EXPECT_TRUE(nodes[0].driverVersion.empty());
    EXPECT_FALSE(nodes[0].driverDate.has_value());
    EXPECT_FALSE(nodes[1].bus.has_value());
    EXPECT_EQ(nodes[1].address, 0x20000U);
    EXPECT_EQ(nodes[1].driverVersion, "32.0.101.8247");
    EXPECT_FALSE(nodes[1].driverDate.has_value());
}

TEST_F(WindowsDriverNodesTest, APropertyOfTheWrongTypeIsIgnored)
{
    fake().nodes = {arcNode()};
    fake().nodes[0].versionType = DEVPROP_TYPE_STRING_LIST;
    fake().nodes[0].dateType = DEVPROP_TYPE_UINT64;
    const std::vector<DriverNode> nodes = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES, fakeApi());
    ASSERT_EQ(nodes.size(), 1U);
    EXPECT_TRUE(nodes[0].driverVersion.empty());
    EXPECT_FALSE(nodes[0].driverDate.has_value());
    EXPECT_EQ(nodes[0].hardwareId, "PCI\\VEN_8086&DEV_7D51&SUBSYS_0C9F1028&REV_08");
}

TEST_F(WindowsDriverNodesTest, TextLongerThanTheBufferIsNotRead)
{
    // The buffer offers all but its last element, which stays 0: 510 characters and a terminator
    // fit, 511 do not.
    fake().nodes = {arcNode(), arcNode()};
    fake().nodes[0].hardwareIds = std::wstring(DRIVER_NODE_TEXT_CHARS - 2, L'A');
    fake().nodes[1].hardwareIds = std::wstring(DRIVER_NODE_TEXT_CHARS - 1, L'B');
    fake().nodes[1].version = std::wstring(DRIVER_NODE_TEXT_CHARS - 1, L'9');
    const std::vector<DriverNode> nodes = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES, fakeApi());
    ASSERT_EQ(nodes.size(), 2U);
    EXPECT_EQ(nodes[0].hardwareId, std::string(DRIVER_NODE_TEXT_CHARS - 2, 'A'));
    EXPECT_TRUE(nodes[1].hardwareId.empty());
    EXPECT_TRUE(nodes[1].driverVersion.empty());
    // Every text read offers the buffer less its last element.
    ASSERT_FALSE(fake().textBufferBytes.empty());
    for (const DWORD bytes : fake().textBufferBytes)
    {
        EXPECT_EQ(bytes, (DRIVER_NODE_TEXT_CHARS - 1) * sizeof(wchar_t));
    }
}

TEST_F(WindowsDriverNodesTest, AListThatCannotBeOpenedGivesNoNodesAndIsNotDestroyed)
{
    fake().failOpen = true;
    fake().nodes = {arcNode()};
    EXPECT_TRUE(listDriverNodes(&DISPLAY_CLASS_GUID, nullptr, DIGCF_PRESENT, fakeApi()).empty());
    EXPECT_EQ(fake().opens, 1);
    EXPECT_EQ(fake().destroys, 0);
}

TEST_F(WindowsDriverNodesTest, AnEmptyListIsStillDestroyed)
{
    EXPECT_TRUE(listDriverNodes(&DISPLAY_CLASS_GUID, nullptr, DIGCF_PRESENT, fakeApi()).empty());
    EXPECT_EQ(fake().destroys, 1);
}

TEST(WindowsDriverNodesKeysTest, AreDevpkeyDeviceDriverDateAndVersion)
{
    // DEVPKEY_Device_DriverDate / _DriverVersion: {a8b865dd-2e3d-4094-ad97-e593a70c75d6}, 2 and 3
    for (const DEVPROPKEY& key : {DRIVER_DATE_KEY, DRIVER_VERSION_KEY})
    {
        EXPECT_EQ(key.fmtid.Data1, 0xa8b865ddU);
        EXPECT_EQ(key.fmtid.Data2, 0x2e3dU);
        EXPECT_EQ(key.fmtid.Data3, 0x4094U);
        EXPECT_EQ(key.fmtid.Data4[0], 0xadU);
        EXPECT_EQ(key.fmtid.Data4[7], 0xd6U);
    }
    EXPECT_EQ(DRIVER_DATE_KEY.pid, 2U);
    EXPECT_EQ(DRIVER_VERSION_KEY.pid, 3U);
    // GUID_DEVCLASS_DISPLAY: {4d36e968-e325-11ce-bfc1-08002be10318}
    EXPECT_EQ(DISPLAY_CLASS_GUID.Data1, 0x4d36e968U);
    EXPECT_EQ(DISPLAY_CLASS_GUID.Data4[7], 0x18U);
}

// ========== This machine's SetupAPI ==========

TEST(WindowsDriverNodesLiveTest, DisplayNodesOnPciMatchThePciList)
{
    // System Information's lister and the GPU probe's read the same nodes: every display-class node on
    // PCI is in the all-classes PCI list with the same facts.
    const std::vector<DriverNode> display = WindowsSystemInfoProbe::graphicsFunctions().listDisplayDrivers();
    const std::vector<DriverNode> pci = listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES);
    for (const DriverNode& node : display)
    {
        if (!node.hardwareId.starts_with("PCI\\"))
        {
            continue; // a software adapter (the Basic Render Driver) or another bus's
        }
        const auto match =
            std::ranges::find_if(pci,
                                 [&node](const DriverNode& other)
                                 { return other.hardwareId == node.hardwareId && other.bus == node.bus && other.address == node.address; });
        ASSERT_NE(match, pci.end()) << node.hardwareId;
        EXPECT_EQ(match->driverVersion, node.driverVersion) << node.hardwareId;
        EXPECT_EQ(match->driverDate.has_value(), node.driverDate.has_value()) << node.hardwareId;
    }
    for (const DriverNode& node : pci)
    {
        EXPECT_TRUE(node.hardwareId.empty() || node.hardwareId.starts_with("PCI\\")) << node.hardwareId;
    }
}

} // namespace Platform::WindowsGraphics
