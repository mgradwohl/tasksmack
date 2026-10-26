/// @file test_WindowsNetworkAdapters.cpp
/// @brief Platform::WindowsNetworkAdapters (#1518) against fake GetAdaptersAddresses and WLAN calls: the
/// adapter list's linked lists (unicast addresses, gateways, DNS servers) walked, address, prefix and MAC
/// formatting, the loopback adapter left out, the default gateway of the lowest metric, DNS servers
/// collected from the up adapters without repeats or Windows' fec0 placeholders, DHCP, a list that grows
/// between the size answer and the copy, the list failing or empty; against a fake GetIpForwardTable2, the
/// default gateway of the cheapest default route, interface plus route metric, for IPv4 and IPv6, several
/// on one interface, none usable, and the table unreadable (#2996); then the Wi-Fi connection decoded
/// (network name, quality, RSSI, PHY, band, channel, rates), the location-access denial, the channel
/// alone, no wlanapi.dll, the service not running, and every WLAN answer freed; without the Wi-Fi details
/// asked for, none of the location-gated calls made (#1782). Plus one read of this machine through the
/// real calls, without the Wi-Fi details.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsConversions.h"
#include "Platform/Windows/WindowsNetworkAdapters.h"

#include <gtest/gtest.h>

// NOLINTBEGIN(misc-include-cleaner) - Winsock and WLAN: include-cleaner lacks mappings for their types
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Platform::WindowsNetworkAdapters
{
namespace
{

/// A fake adapter: the IP_ADAPTER_ADDRESSES and everything its pointers point at, at stable addresses.
class FakeAdapter
{
  public:
    FakeAdapter(const wchar_t* name, const char* guid, IFTYPE type = IF_TYPE_ETHERNET_CSMACD) : m_Name(name), m_Guid(guid)
    {
        m_Raw.Length = sizeof(IP_ADAPTER_ADDRESSES); // NOLINT(cppcoreguidelines-pro-type-union-access) - the API's own layout
        m_Raw.FriendlyName = m_Name.data();
        m_Raw.AdapterName = m_Guid.data();
        m_Raw.IfType = type;
        m_Raw.OperStatus = IfOperStatusUp;
        m_Raw.Mtu = 1500;
    }

    // Its list points into its own members, so it stays where it was made.
    FakeAdapter(const FakeAdapter&) = delete;
    FakeAdapter& operator=(const FakeAdapter&) = delete;
    FakeAdapter(FakeAdapter&&) = delete;
    FakeAdapter& operator=(FakeAdapter&&) = delete;
    ~FakeAdapter() = default;

    IP_ADAPTER_ADDRESSES& raw()
    {
        return m_Raw;
    }

    void description(const wchar_t* text)
    {
        m_Description = text;
        m_Raw.Description = m_Description.data();
    }

    void searchSuffix(const wchar_t* text)
    {
        IP_ADAPTER_DNS_SUFFIX& node = m_SearchList.emplace_back();
        wcsncpy_s(&node.String[0], MAX_DNS_SUFFIX_STRING_LENGTH, text, _TRUNCATE);
        append(m_Raw.FirstDnsSuffix, &node);
    }

    void dnsSuffix(const wchar_t* text)
    {
        m_Suffix = text;
        m_Raw.DnsSuffix = m_Suffix.data();
    }

    void mac(std::initializer_list<BYTE> bytes)
    {
        std::ranges::copy(bytes, std::begin(m_Raw.PhysicalAddress));
        m_Raw.PhysicalAddressLength = static_cast<ULONG>(bytes.size());
    }

    IP_ADAPTER_UNICAST_ADDRESS& unicast(const char* text, UINT8 prefix)
    {
        IP_ADAPTER_UNICAST_ADDRESS& node = m_Unicast.emplace_back();
        node.Address = socketAddress(text);
        node.OnLinkPrefixLength = prefix;
        node.DadState = IpDadStatePreferred;
        append(m_Raw.FirstUnicastAddress, &node);
        return node;
    }

    void gateway(const char* text)
    {
        IP_ADAPTER_GATEWAY_ADDRESS_LH& node = m_Gateways.emplace_back();
        node.Address = socketAddress(text);
        append(m_Raw.FirstGatewayAddress, &node);
    }

    void dns(const char* text)
    {
        IP_ADAPTER_DNS_SERVER_ADDRESS& node = m_Dns.emplace_back();
        node.Address = socketAddress(text);
        append(m_Raw.FirstDnsServerAddress, &node);
    }

    void dhcp(const char* server)
    {
        m_Raw.Flags |= IP_ADAPTER_DHCP_ENABLED; // NOLINT(cppcoreguidelines-pro-type-union-access)
        m_Raw.Dhcpv4Server = socketAddress(server);
    }

    SOCKET_ADDRESS socketAddress(const char* text)
    {
        SOCKADDR_STORAGE& storage = m_Sockaddrs.emplace_back();
        if (std::strchr(text, ':') != nullptr)
        {
            auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            v6->sin6_family = AF_INET6;
            EXPECT_EQ(inet_pton(AF_INET6, text, &v6->sin6_addr), 1) << text;
            return {.lpSockaddr = reinterpret_cast<LPSOCKADDR>(v6), .iSockaddrLength = sizeof(sockaddr_in6)}; // NOLINT
        }
        auto* v4 = reinterpret_cast<sockaddr_in*>(&storage); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        v4->sin_family = AF_INET;
        EXPECT_EQ(inet_pton(AF_INET, text, &v4->sin_addr), 1) << text;
        return {.lpSockaddr = reinterpret_cast<LPSOCKADDR>(v4), .iSockaddrLength = sizeof(sockaddr_in)}; // NOLINT
    }

  private:
    template<typename T> static void append(T*& head, T* node)
    {
        T** link = &head;
        while (*link != nullptr)
        {
            link = &(*link)->Next;
        }
        *link = node;
    }

    IP_ADAPTER_ADDRESSES m_Raw{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed, as the real list's unset fields are
    std::wstring m_Name;
    std::string m_Guid;
    std::wstring m_Description;
    std::wstring m_Suffix;
    std::deque<IP_ADAPTER_UNICAST_ADDRESS> m_Unicast;
    std::deque<IP_ADAPTER_GATEWAY_ADDRESS_LH> m_Gateways;
    std::deque<IP_ADAPTER_DNS_SERVER_ADDRESS> m_Dns;
    std::deque<IP_ADAPTER_DNS_SUFFIX> m_SearchList;
    std::deque<SOCKADDR_STORAGE> m_Sockaddrs;
};

constexpr GUID WIFI_GUID{.Data1 = 0x12345678, .Data2 = 0x9ABC, .Data3 = 0xDEF0, .Data4 = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
constexpr const char* WIFI_GUID_TEXT = "{12345678-9abc-def0-0123-456789abcdef}"; // AdapterName's case varies
constexpr std::array<UCHAR, 6> BSSID{0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

/// What the fakes answer. Reset per test.
struct FakeState
{
    std::vector<FakeAdapter*> adapters; ///< Linked in this order on each list call
    ULONG listStatus = ERROR_SUCCESS;
    int overflowsFirst = 0;          ///< ERROR_BUFFER_OVERFLOW answers before the list
    ULONG overflowBytes = 64 * 1024; ///< The size an overflow answer asks for
    std::vector<ULONG> bufferSizes;  ///< The size each list call was given

    DWORD openStatus = ERROR_SUCCESS;
    WLAN_INTERFACE_STATE interfaceState = wlan_interface_state_connected;
    std::map<WLAN_INTF_OPCODE, DWORD> queryStatus; ///< A status other than success, by opcode
    WLAN_CONNECTION_ATTRIBUTES connection{};       // NOLINT(bugprone-invalid-enum-default-initialization) - filled in SetUp
    LONG rssi = -55;
    ULONG channel = 36;
    ULONG bssFrequencyKhz = 5'180'000;
    bool bssListFails = false;

    std::vector<MIB_IPFORWARD_ROW2> routes; ///< The route table, both families
    DWORD routeStatus = NO_ERROR;           ///< GetIpForwardTable2's answer when not NO_ERROR
    int routeReads = 0;                     ///< GetIpForwardTable2 calls

    std::map<WLAN_INTF_OPCODE, int> queries; ///< WlanQueryInterface calls, by opcode
    int bssLists = 0;                        ///< WlanGetNetworkBssList calls

    int allocations = 0;
    int frees = 0;
    int opens = 0;
    int closes = 0;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, bugprone-throwing-static-initialization) - the fakes are plain function pointers
FakeState g_State;

ULONG WINAPI fakeGetAdaptersAddresses(ULONG /*family*/, ULONG flags, PVOID /*reserved*/, PIP_ADAPTER_ADDRESSES buffer, PULONG size)
{
    EXPECT_NE(flags & GAA_FLAG_INCLUDE_GATEWAYS, 0U);
    g_State.bufferSizes.push_back(*size);
    if (g_State.overflowsFirst > 0)
    {
        --g_State.overflowsFirst;
        *size = g_State.overflowBytes;
        return ERROR_BUFFER_OVERFLOW;
    }
    if (g_State.listStatus != ERROR_SUCCESS)
    {
        return g_State.listStatus;
    }
    if (g_State.adapters.empty())
    {
        return ERROR_NO_DATA;
    }
    // The first entry is copied into the buffer, as the real call lays it out; the rest stay in the fakes.
    std::size_t i = g_State.adapters.size();
    IP_ADAPTER_ADDRESSES* next = nullptr;
    while (i > 1)
    {
        --i;
        g_State.adapters[i]->raw().Next = next;
        next = &g_State.adapters[i]->raw();
    }
    *buffer = g_State.adapters.front()->raw();
    buffer->Next = next;
    return ERROR_SUCCESS;
}

/// A WlanFreeMemory-able copy of @p value.
template<typename T> PVOID allocate(const T& value, std::size_t bytes = sizeof(T))
{
    void* memory = std::calloc(1, bytes); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - freed by fakeFree
    std::memcpy(memory, &value, sizeof(T));
    ++g_State.allocations;
    return memory;
}

DWORD WINAPI fakeOpen(DWORD /*version*/, PVOID /*reserved*/, PDWORD negotiated, PHANDLE client)
{
    if (g_State.openStatus != ERROR_SUCCESS)
    {
        return g_State.openStatus;
    }
    ++g_State.opens;
    *negotiated = 2;
    *client = reinterpret_cast<HANDLE>(0x1234); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    return ERROR_SUCCESS;
}

DWORD WINAPI fakeClose(HANDLE /*client*/, PVOID /*reserved*/)
{
    ++g_State.closes;
    return ERROR_SUCCESS;
}

DWORD WINAPI fakeEnum(HANDLE /*client*/, PVOID /*reserved*/, PWLAN_INTERFACE_INFO_LIST* list)
{
    WLAN_INTERFACE_INFO_LIST value{};
    value.dwNumberOfItems = 1;
    value.InterfaceInfo[0].InterfaceGuid = WIFI_GUID;
    value.InterfaceInfo[0].isState = g_State.interfaceState;
    *list = static_cast<PWLAN_INTERFACE_INFO_LIST>(allocate(value));
    return ERROR_SUCCESS;
}

DWORD WINAPI fakeQuery(HANDLE /*client*/,
                       const GUID* guid,
                       WLAN_INTF_OPCODE opcode,
                       PVOID /*reserved*/,
                       PDWORD size,
                       PVOID* data,
                       PWLAN_OPCODE_VALUE_TYPE /*type*/)
{
    EXPECT_TRUE(IsEqualGUID(*guid, WIFI_GUID));
    ++g_State.queries[opcode];
    if (const auto failed = g_State.queryStatus.find(opcode); failed != g_State.queryStatus.end())
    {
        return failed->second;
    }
    switch (opcode)
    {
    case wlan_intf_opcode_current_connection:
        *data = allocate(g_State.connection);
        *size = sizeof(WLAN_CONNECTION_ATTRIBUTES);
        return ERROR_SUCCESS;
    case wlan_intf_opcode_rssi:
        *data = allocate(g_State.rssi);
        *size = sizeof(LONG);
        return ERROR_SUCCESS;
    case wlan_intf_opcode_channel_number:
        *data = allocate(g_State.channel);
        *size = sizeof(ULONG);
        return ERROR_SUCCESS;
    default:
        return ERROR_NOT_SUPPORTED;
    }
}

DWORD WINAPI fakeBssList(HANDLE /*client*/,
                         const GUID* /*guid*/,
                         PDOT11_SSID /*ssid*/,
                         DOT11_BSS_TYPE /*type*/,
                         BOOL /*security*/,
                         PVOID /*reserved*/,
                         PWLAN_BSS_LIST* list)
{
    ++g_State.bssLists;
    if (g_State.bssListFails)
    {
        return ERROR_ACCESS_DENIED;
    }
    // Two entries: another access point of the same network first, then the connected one.
    const std::size_t bytes = sizeof(WLAN_BSS_LIST) + sizeof(WLAN_BSS_ENTRY);
    WLAN_BSS_LIST head{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed like the real list
    head.dwTotalSize = static_cast<DWORD>(bytes);
    head.dwNumberOfItems = 2;
    auto* value = static_cast<PWLAN_BSS_LIST>(allocate(head, bytes));
    WLAN_BSS_ENTRY* entries = &value->wlanBssEntries[0];
    entries[0].ulChCenterFrequency = 2'412'000;                  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::ranges::copy(BSSID, std::begin(entries[1].dot11Bssid)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    entries[1].ulChCenterFrequency = g_State.bssFrequencyKhz;    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    *list = value;
    return ERROR_SUCCESS;
}

VOID WINAPI fakeFree(PVOID memory)
{
    ++g_State.frees;
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc)
}

/// The routes of @p family as a FreeMibTable-able table, as GetIpForwardTable2 answers: ERROR_NOT_FOUND
/// for none.
DWORD WINAPI fakeGetIpForwardTable2(ADDRESS_FAMILY family, PMIB_IPFORWARD_TABLE2* table)
{
    ++g_State.routeReads;
    if (g_State.routeStatus != NO_ERROR)
    {
        return g_State.routeStatus;
    }
    std::vector<MIB_IPFORWARD_ROW2> rows;
    const auto ofFamily = [family](const MIB_IPFORWARD_ROW2& row)
    {
        return row.DestinationPrefix.Prefix.si_family == family; // NOLINT(cppcoreguidelines-pro-type-union-access) - the shared tag
    };
    std::ranges::copy_if(g_State.routes, std::back_inserter(rows), ofFamily);
    if (rows.empty())
    {
        return ERROR_NOT_FOUND;
    }
    const std::size_t bytes = sizeof(MIB_IPFORWARD_TABLE2) + ((rows.size() - 1) * sizeof(MIB_IPFORWARD_ROW2));
    MIB_IPFORWARD_TABLE2 head{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed like the real table
    head.NumEntries = static_cast<ULONG>(rows.size());
    auto* value = static_cast<PMIB_IPFORWARD_TABLE2>(allocate(head, bytes));
    std::memcpy(&value->Table[0], rows.data(), rows.size() * sizeof(MIB_IPFORWARD_ROW2));
    *table = value;
    return NO_ERROR;
}

/// A route through the interface @p luid to @p nextHop ("0.0.0.0" or "::" for on-link), a default route
/// unless @p prefixLength says otherwise.
[[nodiscard]] MIB_IPFORWARD_ROW2 route(std::uint64_t luid, const char* nextHop, ULONG metric, UINT8 prefixLength = 0)
{
    MIB_IPFORWARD_ROW2 row{}; // NOLINT(bugprone-invalid-enum-default-initialization) - zeroed like the real table
    const bool v6 = std::strchr(nextHop, ':') != nullptr;
    row.InterfaceLuid.Value = luid;
    row.DestinationPrefix.PrefixLength = prefixLength;
    row.Metric = metric;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - si_family says which member is set
    row.DestinationPrefix.Prefix.si_family = v6 ? AF_INET6 : AF_INET;
    row.NextHop.si_family = v6 ? AF_INET6 : AF_INET;
    if (v6)
    {
        EXPECT_EQ(inet_pton(AF_INET6, nextHop, &row.NextHop.Ipv6.sin6_addr), 1) << nextHop;
    }
    else
    {
        EXPECT_EQ(inet_pton(AF_INET, nextHop, &row.NextHop.Ipv4.sin_addr), 1) << nextHop;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    return row;
}

constexpr std::uint64_t FAKE_NOW = 1'760'000'000;

/// The reads that ask for the location-gated Wi-Fi details, as after "Show Wi-Fi details" (#1782).
constexpr bool WITH_DETAILS = true;

std::uint64_t fakeNow()
{
    return FAKE_NOW;
}

[[nodiscard]] Functions fakeFunctions()
{
    return {
        .getAdaptersAddresses = &fakeGetAdaptersAddresses,
        .wlanOpenHandle = &fakeOpen,
        .wlanCloseHandle = &fakeClose,
        .wlanEnumInterfaces = &fakeEnum,
        .wlanQueryInterface = &fakeQuery,
        .wlanGetNetworkBssList = &fakeBssList,
        .wlanFreeMemory = &fakeFree,
        .nowUnix = &fakeNow,
    };
}

/// fakeFunctions() with the fake route table (#2996); fakeFunctions() alone has none, as when it can't be read.
[[nodiscard]] Functions fakeFunctionsWithRoutes()
{
    Functions fns = fakeFunctions();
    fns.getIpForwardTable2 = &fakeGetIpForwardTable2;
    fns.freeMibTable = &fakeFree;
    return fns;
}

[[nodiscard]] const NetworkAdapter* find(const NetworkAdaptersInfo& info, const std::string& name)
{
    for (const NetworkAdapter& adapter : info.adapters)
    {
        if (adapter.name == name)
        {
            return &adapter;
        }
    }
    return nullptr;
}

/// @p adapter's Wi-Fi link, or an empty one. A plain reference keeps clang-tidy's optional
/// dataflow satisfied: bugprone-unchecked-optional-access doesn't model ASSERT_TRUE's early return.
[[nodiscard]] const WifiLink& wifiOf(const NetworkAdapter& adapter)
{
    static const WifiLink none;
    return adapter.wifiLink.has_value() ? *adapter.wifiLink : none;
}

class WindowsNetworkAdaptersTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        g_State = {};
        WLAN_ASSOCIATION_ATTRIBUTES& association = g_State.connection.wlanAssociationAttributes;
        g_State.connection.isState = wlan_interface_state_connected;
        const std::string ssid = "HomeNet";
        association.dot11Ssid.uSSIDLength = static_cast<ULONG>(ssid.size());
        std::ranges::copy(ssid, std::begin(association.dot11Ssid.ucSSID));
        std::ranges::copy(BSSID, std::begin(association.dot11Bssid));
        association.dot11PhyType = dot11_phy_type_he;
        association.wlanSignalQuality = 82;
        association.ulRxRate = 1'201'000;
        association.ulTxRate = 864'500;
    }

    void TearDown() override
    {
        EXPECT_EQ(g_State.frees, g_State.allocations) << "every WLAN answer is freed";
        EXPECT_EQ(g_State.closes, g_State.opens) << "the WLAN handle is closed";
    }
};

TEST_F(WindowsNetworkAdaptersTest, WalksTheAdapterListsAndFormats)
{
    FakeAdapter loopback(L"Loopback Pseudo-Interface 1", "{00000000-0000-0000-0000-000000000001}", IF_TYPE_SOFTWARE_LOOPBACK);
    loopback.unicast("127.0.0.1", 8);
    FakeAdapter ethernet(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    ethernet.description(L"Intel(R) Ethernet Connection I219-V");
    ethernet.mac({0xA4, 0x5E, 0x60, 0x12, 0x34, 0x0B});
    ethernet.unicast("fe80::1c2a:3bff:fe4d:5e6f", 64);
    ethernet.unicast("192.168.0.20", 24).PrefixOrigin = IpPrefixOriginManual;
    ethernet.unicast("169.254.1.2", 16).DadState = IpDadStateTentative; // not in use: left out
    ethernet.unicast("fe80::2", 64).DadState = IpDadStateDeprecated;    // still in use
    ethernet.gateway("192.168.0.1");
    ethernet.gateway("fe80::1");
    ethernet.dns("192.168.0.1");
    ethernet.dns("192.168.0.1"); // listed twice, kept once
    ethernet.dns("fec0:0:0:ffff::1");
    ethernet.dnsSuffix(L"corp.example.com");
    ethernet.searchSuffix(L"corp.example.com"); // repeats the suffix: kept once
    ethernet.searchSuffix(L"example.com");
    ethernet.raw().Flags |= IP_ADAPTER_IPV4_ENABLED; // NOLINT(cppcoreguidelines-pro-type-union-access) - static IPv4
    ethernet.raw().Ipv4Metric = 25;
    ethernet.raw().Ipv6Metric = 25;
    g_State.adapters = {&loopback, &ethernet};

    NetworkAdaptersInfo info;
    Functions fns = fakeFunctions();
    fns.wlanOpenHandle = nullptr; // no wlanapi.dll
    readNetworkAdapters(info, fns, WITH_DETAILS);

    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.listed);
    EXPECT_TRUE(info.dnsRead);
    ASSERT_EQ(info.adapters.size(), 1U) << "the loopback adapter is left out";
    const NetworkAdapter& adapter = info.adapters[0];
    EXPECT_EQ(adapter.name, "Ethernet");
    EXPECT_EQ(adapter.description, "Intel(R) Ethernet Connection I219-V");
    EXPECT_TRUE(adapter.up);
    EXPECT_FALSE(adapter.wireless);
    EXPECT_EQ(adapter.mtu, 1500U);
    EXPECT_EQ(adapter.mac, "A4-5E-60-12-34-0B");
    ASSERT_EQ(adapter.addresses.size(), 3U);
    EXPECT_EQ(adapter.addresses[0], (AdapterAddress{.address = "fe80::1c2a:3bff:fe4d:5e6f", .prefix = 64, .v6 = true}));
    EXPECT_EQ(adapter.addresses[1], (AdapterAddress{.address = "192.168.0.20", .prefix = 24, .v6 = false}));
    EXPECT_EQ(adapter.addresses[2], (AdapterAddress{.address = "fe80::2", .prefix = 64, .v6 = true}));
    EXPECT_EQ(adapter.gateways, (std::vector<std::string>{"192.168.0.1", "fe80::1"}));
    EXPECT_EQ(adapter.linkDnsServers, (std::vector<std::string>{"192.168.0.1"})) << "no repeats, no fec0 placeholders";
    EXPECT_EQ(adapter.linkSearchDomains, (std::vector<std::string>{"corp.example.com", "example.com"}));
    EXPECT_EQ(adapter.dhcpEnabled, std::optional<bool>(false));
    EXPECT_FALSE(adapter.dhcpLease.has_value()) << "static IPv4: no lease";

    EXPECT_EQ(info.gatewayV4, "192.168.0.1");
    EXPECT_EQ(info.gatewayV4Adapter, "Ethernet");
    EXPECT_EQ(info.gatewayV6, "fe80::1");
    EXPECT_EQ(info.dnsServers, (std::vector<std::string>{"192.168.0.1"}));
    EXPECT_EQ(info.searchDomains, (std::vector<std::string>{"corp.example.com", "example.com"}));
}

TEST_F(WindowsNetworkAdaptersTest, LowestMetricGatewayDnsFromUpAdaptersAndDhcp)
{
    FakeAdapter vpn(L"VPN", "{BBBBBBBB-0000-0000-0000-000000000003}");
    vpn.gateway("10.8.0.1");
    vpn.dns("10.8.0.53");
    vpn.raw().Ipv4Metric = 5;
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.gateway("192.168.0.1");
    lan.dns("192.168.0.1");
    lan.dns("10.8.0.53");
    lan.raw().Ipv4Metric = 25;
    lan.raw().Flags |= IP_ADAPTER_IPV4_ENABLED; // NOLINT(cppcoreguidelines-pro-type-union-access)
    lan.dhcp("192.168.0.1");
    IP_ADAPTER_UNICAST_ADDRESS& leased = lan.unicast("192.168.0.20", 24);
    leased.PrefixOrigin = IpPrefixOriginDhcp;
    leased.LeaseLifetime = 86'000;
    FakeAdapter down(L"Bluetooth Network Connection", "{CCCCCCCC-0000-0000-0000-000000000004}");
    down.raw().OperStatus = IfOperStatusDown;
    down.gateway("172.16.0.1");
    down.dns("172.16.0.53");
    down.raw().Ipv4Metric = 1;
    g_State.adapters = {&lan, &down, &vpn};

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);

    EXPECT_EQ(info.gatewayV4, "10.8.0.1") << "the up adapter with the lowest metric";
    EXPECT_EQ(info.gatewayV4Adapter, "VPN");
    EXPECT_TRUE(info.gatewayV6.empty());
    EXPECT_EQ(info.dnsServers, (std::vector<std::string>{"10.8.0.53", "192.168.0.1"}))
        << "lowest metric first, no repeats, no down adapter";
    ASSERT_EQ(info.adapters.size(), 3U);
    EXPECT_EQ(info.adapters[0].name, "Bluetooth Network Connection") << "sorted by name";
    EXPECT_FALSE(info.adapters[0].up);
    const NetworkAdapter* ethernet = find(info, "Ethernet");
    ASSERT_NE(ethernet, nullptr);
    EXPECT_EQ(ethernet->dhcpEnabled, std::optional<bool>(true));
    ASSERT_TRUE(ethernet->dhcpLease.has_value());
    const DhcpLease lease = ethernet->dhcpLease.value_or(DhcpLease{});
    EXPECT_EQ(lease.server, "192.168.0.1");
    EXPECT_EQ(lease.leaseSeconds, 0U) << "the length isn't reported";
    EXPECT_EQ(lease.expiresUnix, FAKE_NOW + 86'000) << "the read's time plus the time left";
    EXPECT_FALSE(find(info, "VPN")->dhcpEnabled.has_value()) << "IPv4 not enabled: DHCP isn't known";
}

TEST_F(WindowsNetworkAdaptersTest, RouteMetricsReverseTheInterfaceOrderV4)
{
    FakeAdapter vpn(L"VPN", "{BBBBBBBB-0000-0000-0000-000000000003}");
    vpn.raw().Luid.Value = 3;
    vpn.raw().Ipv4Metric = 5;
    vpn.gateway("10.8.0.1");
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.raw().Luid.Value = 2;
    lan.raw().Ipv4Metric = 25;
    lan.gateway("192.168.0.1");
    FakeAdapter down(L"Bluetooth Network Connection", "{CCCCCCCC-0000-0000-0000-000000000004}");
    down.raw().Luid.Value = 4;
    down.raw().OperStatus = IfOperStatusDown;
    down.raw().Ipv4Metric = 1;
    down.gateway("172.16.0.1");
    g_State.adapters = {&lan, &down, &vpn};
    g_State.routes = {
        route(3, "10.8.0.1", 100),  // 5 + 100 = 105
        route(3, "10.8.0.1", 0, 8), // 10.0.0.0/8: not a default route
        route(2, "192.168.0.1", 0), // 25 + 0 = 25: the cheapest
        route(4, "172.16.0.1", 0),  // through a down adapter
        route(99, "192.0.2.1", 0),  // through an interface not listed
    };

    NetworkAdaptersInfo byInterface;
    readNetworkAdapters(byInterface, fakeFunctions(), WITH_DETAILS);
    EXPECT_EQ(byInterface.gatewayV4Adapter, "VPN") << "the interface metric alone picks the VPN";

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctionsWithRoutes(), WITH_DETAILS);
    EXPECT_EQ(info.gatewayV4, "192.168.0.1") << "the route's metric plus the interface's: 25 beats 105";
    EXPECT_EQ(info.gatewayV4Adapter, "Ethernet");
    EXPECT_TRUE(info.gatewayV6.empty()) << "no IPv6 route at all";
    EXPECT_TRUE(info.gatewayV6Adapter.empty());
    EXPECT_EQ(g_State.routeReads, 2) << "one table per family";
    const NetworkAdapter* tunnel = find(info, "VPN");
    ASSERT_NE(tunnel, nullptr);
    EXPECT_EQ(tunnel->gateways, (std::vector<std::string>{"10.8.0.1"})) << "each adapter's own gateways are unchanged";
}

TEST_F(WindowsNetworkAdaptersTest, RouteMetricsReverseTheInterfaceOrderV6)
{
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.raw().Luid.Value = 2;
    lan.raw().Ipv4Metric = 15;
    lan.raw().Ipv6Metric = 15;
    lan.gateway("192.168.0.1");
    lan.gateway("fe80::1");
    FakeAdapter wifi(L"Wi-Fi", "{DDDDDDDD-0000-0000-0000-000000000005}");
    wifi.raw().Luid.Value = 5;
    wifi.raw().Ipv6Metric = 45;
    wifi.gateway("fe80::2");
    g_State.adapters = {&lan, &wifi};
    g_State.routes = {
        route(2, "192.168.0.1", 0),
        route(2, "fe80::1", 256), // 15 + 256 = 271
        route(5, "::", 0),        // on-link: no gateway to show
        route(5, "fe80::2", 0),   // 45 + 0 = 45: the cheapest
    };

    NetworkAdaptersInfo byInterface;
    readNetworkAdapters(byInterface, fakeFunctions(), WITH_DETAILS);
    EXPECT_EQ(byInterface.gatewayV6, "fe80::1") << "the interface metric alone picks the Ethernet";

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctionsWithRoutes(), WITH_DETAILS);
    EXPECT_EQ(info.gatewayV6, "fe80::2") << "45 beats 271";
    EXPECT_EQ(info.gatewayV6Adapter, "Wi-Fi");
    EXPECT_EQ(info.gatewayV4, "192.168.0.1");
    EXPECT_EQ(info.gatewayV4Adapter, "Ethernet");
    const NetworkAdapter* ethernet = find(info, "Ethernet");
    ASSERT_NE(ethernet, nullptr);
    EXPECT_EQ(ethernet->gateways, (std::vector<std::string>{"192.168.0.1", "fe80::1"}));
}

TEST_F(WindowsNetworkAdaptersTest, TheCheapestOfSeveralGatewaysOnOneInterface)
{
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.raw().Luid.Value = 2;
    lan.raw().Ipv4Metric = 25;
    lan.gateway("192.168.0.1");
    lan.gateway("192.168.0.254");
    g_State.adapters = {&lan};
    g_State.routes = {
        route(2, "192.168.0.1", 50),
        route(2, "0.0.0.0", 0),        // on-link: no gateway to show
        route(2, "192.168.0.254", 10), // the cheapest
        route(2, "192.168.0.253", 10), // as cheap, listed later
    };

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctionsWithRoutes(), WITH_DETAILS);
    EXPECT_EQ(info.gatewayV4, "192.168.0.254") << "the cheapest route's next hop, not the adapter's first gateway";
    EXPECT_EQ(info.gatewayV4Adapter, "Ethernet");
    ASSERT_EQ(info.adapters.size(), 1U);
    EXPECT_EQ(info.adapters[0].gateways, (std::vector<std::string>{"192.168.0.1", "192.168.0.254"}));
}

TEST_F(WindowsNetworkAdaptersTest, NoUsableDefaultRouteMeansNoDefaultGateway)
{
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.raw().Luid.Value = 2;
    lan.gateway("192.168.0.1");
    lan.gateway("fe80::1");
    FakeAdapter down(L"Bluetooth Network Connection", "{CCCCCCCC-0000-0000-0000-000000000004}");
    down.raw().Luid.Value = 4;
    down.raw().OperStatus = IfOperStatusDown;
    down.gateway("172.16.0.1");
    g_State.adapters = {&lan, &down};
    // No IPv6 route at all, and no IPv4 default route with a next hop through an up adapter.
    g_State.routes = {
        route(2, "192.168.0.1", 0, 24), // 192.168.0.0/24: not a default route
        route(2, "0.0.0.0", 0),         // on-link
        route(4, "172.16.0.1", 0),      // through a down adapter
    };

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctionsWithRoutes(), WITH_DETAILS);
    EXPECT_TRUE(info.gatewayV4.empty()) << "the table was read: no gateway is made up from the adapters'";
    EXPECT_TRUE(info.gatewayV4Adapter.empty());
    EXPECT_TRUE(info.gatewayV6.empty());
    EXPECT_TRUE(info.gatewayV6Adapter.empty());
    const NetworkAdapter* ethernet = find(info, "Ethernet");
    ASSERT_NE(ethernet, nullptr);
    EXPECT_EQ(ethernet->gateways, (std::vector<std::string>{"192.168.0.1", "fe80::1"}));
}

TEST_F(WindowsNetworkAdaptersTest, RouteTableReadFailureFallsBackToTheInterfaceMetric)
{
    FakeAdapter vpn(L"VPN", "{BBBBBBBB-0000-0000-0000-000000000003}");
    vpn.raw().Luid.Value = 3;
    vpn.raw().Ipv4Metric = 5;
    vpn.raw().Ipv6Metric = 5;
    vpn.gateway("10.8.0.1");
    vpn.gateway("fd00::1");
    FakeAdapter lan(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    lan.raw().Luid.Value = 2;
    lan.raw().Ipv4Metric = 25;
    lan.raw().Ipv6Metric = 25;
    lan.gateway("192.168.0.1");
    lan.gateway("fe80::1");
    g_State.adapters = {&lan, &vpn};
    g_State.routes = {route(3, "10.8.0.1", 100), route(2, "192.168.0.1", 0)}; // would pick the Ethernet
    g_State.routeStatus = ERROR_NOT_SUPPORTED;

    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctionsWithRoutes(), WITH_DETAILS);
    EXPECT_EQ(g_State.routeReads, 2);
    EXPECT_EQ(info.gatewayV4, "10.8.0.1") << "the up adapter with the lowest interface metric";
    EXPECT_EQ(info.gatewayV4Adapter, "VPN");
    EXPECT_EQ(info.gatewayV6, "fd00::1");
    EXPECT_EQ(info.gatewayV6Adapter, "VPN");

    // Without FreeMibTable the table isn't asked for at all.
    g_State.routeReads = 0;
    g_State.routeStatus = NO_ERROR;
    Functions noFree = fakeFunctionsWithRoutes();
    noFree.freeMibTable = nullptr;
    NetworkAdaptersInfo unfreeable;
    readNetworkAdapters(unfreeable, noFree, WITH_DETAILS);
    EXPECT_EQ(g_State.routeReads, 0);
    EXPECT_EQ(unfreeable.gatewayV4Adapter, "VPN");
}
TEST_F(WindowsNetworkAdaptersTest, ListGrowsThenFailsOrIsEmpty)
{
    FakeAdapter ethernet(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    g_State.adapters = {&ethernet};
    g_State.overflowsFirst = 1;
    NetworkAdaptersInfo grown;
    readNetworkAdapters(grown, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(grown.listed);
    EXPECT_EQ(grown.adapters.size(), 1U);
    ASSERT_EQ(g_State.bufferSizes.size(), 2U);
    EXPECT_GE(g_State.bufferSizes[1], g_State.overflowBytes) << "retried with the size asked for";

    g_State.bufferSizes.clear();
    g_State.overflowsFirst = 100; // never settles
    NetworkAdaptersInfo unsettled;
    readNetworkAdapters(unsettled, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(unsettled.available);
    EXPECT_FALSE(unsettled.listed);
    EXPECT_FALSE(unsettled.dnsRead);
    EXPECT_EQ(g_State.bufferSizes.size(), 4U) << "a bounded number of tries";

    g_State.overflowsFirst = 0;
    g_State.listStatus = ERROR_NOT_ENOUGH_MEMORY;
    NetworkAdaptersInfo failed;
    readNetworkAdapters(failed, fakeFunctions(), WITH_DETAILS);
    EXPECT_FALSE(failed.listed);
    EXPECT_TRUE(failed.adapters.empty());

    g_State.listStatus = ERROR_SUCCESS;
    g_State.adapters.clear();
    NetworkAdaptersInfo none;
    readNetworkAdapters(none, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(none.listed) << "ERROR_NO_DATA: listed, and there are none";
    EXPECT_TRUE(none.adapters.empty());
}

TEST_F(WindowsNetworkAdaptersTest, DecodesTheWifiLink)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);

    ASSERT_EQ(info.adapters.size(), 1U);
    const NetworkAdapter& adapter = info.adapters[0];
    EXPECT_TRUE(adapter.wireless);
    EXPECT_EQ(adapter.wifiSignalDbm, std::optional<int>(-55));
    ASSERT_TRUE(adapter.wifiLink.has_value());
    EXPECT_EQ(wifiOf(adapter).ssid, "HomeNet");
    EXPECT_EQ(wifiOf(adapter).signalQuality, std::optional<std::uint32_t>(82));
    EXPECT_EQ(wifiOf(adapter).phy, "802.11ax (Wi-Fi 6)");
    EXPECT_EQ(wifiOf(adapter).frequencyMhz, 5180U) << "the connected BSS's, not the other access point's";
    EXPECT_EQ(wifiOf(adapter).channel, 0U) << "not asked for: the frequency gives it";
    EXPECT_EQ(wifiOf(adapter).rxBitrateKbps, 1'201'000U);
    EXPECT_EQ(wifiOf(adapter).bitrateKbps, 864'500U);
    EXPECT_TRUE(adapter.wifiLinkUnavailable.empty());
}

TEST_F(WindowsNetworkAdaptersTest, SsidBytesAreEscapedForDisplay)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    DOT11_SSID& ssid = g_State.connection.wlanAssociationAttributes.dot11Ssid;
    const std::array<UCHAR, 6> bytes{'C', 'a', 'f', 'e', ' ', 0xFF}; // not UTF-8
    std::ranges::copy(bytes, std::begin(ssid.ucSSID));
    ssid.uSSIDLength = static_cast<ULONG>(bytes.size());
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);
    EXPECT_EQ(wifiOf(info.adapters[0]).ssid, "Cafe \\xFF");
}

TEST_F(WindowsNetworkAdaptersTest, SixGigahertzIsWifi6E)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    g_State.bssFrequencyKhz = 6'135'000;
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);
    ASSERT_TRUE(info.adapters[0].wifiLink.has_value());
    EXPECT_EQ(wifiOf(info.adapters[0]).phy, "802.11ax (Wi-Fi 6E)");
    EXPECT_EQ(wifiOf(info.adapters[0]).frequencyMhz, 6135U);
}

TEST_F(WindowsNetworkAdaptersTest, LocationDeniedKeepsTheRssiAndChannel)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    g_State.queryStatus[wlan_intf_opcode_current_connection] = ERROR_ACCESS_DENIED;
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);

    const NetworkAdapter& adapter = info.adapters[0];
    EXPECT_TRUE(adapter.wifiLinkUnavailable.contains("location")) << adapter.wifiLinkUnavailable;
    EXPECT_EQ(adapter.wifiSignalDbm, std::optional<int>(-55));
    ASSERT_TRUE(adapter.wifiLink.has_value());
    EXPECT_TRUE(wifiOf(adapter).ssid.empty());
    EXPECT_FALSE(wifiOf(adapter).signalQuality.has_value());
    EXPECT_EQ(wifiOf(adapter).channel, 36U) << "from wlan_intf_opcode_channel_number";
    EXPECT_EQ(wifiOf(adapter).frequencyMhz, 0U);
}

TEST_F(WindowsNetworkAdaptersTest, BssListMissingFallsBackToTheChannelNumber)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    g_State.bssListFails = true;
    g_State.channel = 6;
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);
    ASSERT_TRUE(info.adapters[0].wifiLink.has_value());
    EXPECT_EQ(wifiOf(info.adapters[0]).ssid, "HomeNet");
    EXPECT_EQ(wifiOf(info.adapters[0]).channel, 6U);
    EXPECT_EQ(wifiOf(info.adapters[0]).frequencyMhz, 0U);
    EXPECT_EQ(wifiOf(info.adapters[0]).phy, "802.11ax (Wi-Fi 6)");
}

TEST_F(WindowsNetworkAdaptersTest, DisconnectedOrNoServiceOrNoDll)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    g_State.interfaceState = wlan_interface_state_disconnected;
    NetworkAdaptersInfo disconnected;
    readNetworkAdapters(disconnected, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(disconnected.adapters[0].wireless);
    EXPECT_FALSE(disconnected.adapters[0].wifiLink.has_value());
    EXPECT_FALSE(disconnected.adapters[0].wifiSignalDbm.has_value());
    EXPECT_EQ(disconnected.adapters[0].wifiLinkUnavailable, "Not connected");

    g_State.interfaceState = wlan_interface_state_connected;
    g_State.openStatus = ERROR_SERVICE_NOT_ACTIVE;
    NetworkAdaptersInfo noService;
    readNetworkAdapters(noService, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(noService.adapters[0].wireless) << "still Wi-Fi by its interface type";
    EXPECT_FALSE(noService.adapters[0].wifiLink.has_value());
    EXPECT_TRUE(noService.adapters[0].wifiLinkUnavailable.contains("WLAN AutoConfig")) << noService.adapters[0].wifiLinkUnavailable;

    g_State.openStatus = ERROR_SUCCESS;
    Functions noDll = fakeFunctions();
    noDll.wlanQueryInterface = nullptr;
    const int opensBefore = g_State.opens;
    NetworkAdaptersInfo missing;
    readNetworkAdapters(missing, noDll, WITH_DETAILS);
    EXPECT_FALSE(missing.adapters[0].wifiLink.has_value());
    EXPECT_EQ(g_State.opens, opensBefore) << "nothing opened without every export";
    EXPECT_TRUE(missing.adapters[0].wifiLinkUnavailable.contains("wlanapi.dll")) << missing.adapters[0].wifiLinkUnavailable;
}

TEST_F(WindowsNetworkAdaptersTest, WlanInterfaceMarksAnAdapterWireless)
{
    // A Wi-Fi adapter whose driver reports another interface type is still matched by its GUID.
    FakeAdapter wifi(L"Wi-Fi 2", WIFI_GUID_TEXT, IF_TYPE_ETHERNET_CSMACD);
    g_State.adapters = {&wifi};
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);
    EXPECT_TRUE(info.adapters[0].wireless);
    EXPECT_TRUE(info.adapters[0].wifiLink.has_value());
}

TEST_F(WindowsNetworkAdaptersTest, DownWirelessAdapterThatIsNoWlanInterfaceIsNotConnected)
{
    // A Wi-Fi Direct virtual adapter: Wi-Fi by its interface type, but not one of WlanEnumInterfaces'.
    FakeAdapter direct(L"Wi-Fi 2", "{DDDDDDDD-0000-0000-0000-000000000005}", IF_TYPE_IEEE80211);
    direct.raw().OperStatus = IfOperStatusDown;
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&direct, &wifi};
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);
    ASSERT_EQ(info.adapters.size(), 2U);
    EXPECT_EQ(find(info, "Wi-Fi 2")->wifiLinkUnavailable, "Not connected");
    EXPECT_TRUE(find(info, "Wi-Fi")->wifiLinkUnavailable.empty());
}

TEST_F(WindowsNetworkAdaptersTest, WithoutWifiDetailsNoLocationGatedCallIsMade)
{
    // Opening the System tab must not raise a location prompt (#1782): neither the current connection
    // nor the BSS list is asked for, only the RSSI and channel number, which need no location access.
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    FakeAdapter ethernet(L"Ethernet", "{AAAAAAAA-0000-0000-0000-000000000002}");
    ethernet.unicast("192.168.0.20", 24);
    ethernet.gateway("192.168.0.1");
    ethernet.dns("192.168.0.1");
    wifi.unicast("192.168.1.20", 24);
    g_State.adapters = {&ethernet, &wifi};
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), /*includeWifiDetails=*/false);

    EXPECT_EQ(g_State.queries[wlan_intf_opcode_current_connection], 0);
    EXPECT_EQ(g_State.bssLists, 0);
    EXPECT_EQ(g_State.queries[wlan_intf_opcode_rssi], 1);
    EXPECT_EQ(g_State.queries[wlan_intf_opcode_channel_number], 1);

    const NetworkAdapter* adapter = find(info, "Wi-Fi");
    ASSERT_NE(adapter, nullptr);
    EXPECT_TRUE(adapter->wireless);
    EXPECT_TRUE(adapter->wifiDetailsWithheld);
    EXPECT_TRUE(adapter->wifiLinkUnavailable.empty()) << "not refused: not asked";
    EXPECT_EQ(adapter->wifiSignalDbm, std::optional<int>(-55));
    EXPECT_TRUE(wifiOf(*adapter).ssid.empty());
    EXPECT_FALSE(wifiOf(*adapter).signalQuality.has_value());
    EXPECT_EQ(wifiOf(*adapter).channel, 36U);
    EXPECT_EQ(adapter->addresses.size(), 1U) << "the rows that need no location stay automatic";
    const NetworkAdapter* wired = find(info, "Ethernet");
    ASSERT_NE(wired, nullptr);
    EXPECT_FALSE(wired->wifiDetailsWithheld);
    EXPECT_EQ(info.gatewayV4, "192.168.0.1");
    EXPECT_EQ(info.dnsServers, (std::vector<std::string>{"192.168.0.1"}));
}

TEST_F(WindowsNetworkAdaptersTest, WithWifiDetailsTheLocationGatedCallsAreMade)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), WITH_DETAILS);

    EXPECT_EQ(g_State.queries[wlan_intf_opcode_current_connection], 1);
    EXPECT_EQ(g_State.bssLists, 1);
    EXPECT_FALSE(info.adapters[0].wifiDetailsWithheld);
    EXPECT_EQ(wifiOf(info.adapters[0]).ssid, "HomeNet");
}

TEST_F(WindowsNetworkAdaptersTest, WithoutWifiDetailsADisconnectedAdapterIsStillNotConnected)
{
    FakeAdapter wifi(L"Wi-Fi", WIFI_GUID_TEXT, IF_TYPE_IEEE80211);
    g_State.adapters = {&wifi};
    g_State.interfaceState = wlan_interface_state_disconnected;
    NetworkAdaptersInfo info;
    readNetworkAdapters(info, fakeFunctions(), /*includeWifiDetails=*/false);
    EXPECT_EQ(info.adapters[0].wifiLinkUnavailable, "Not connected");
    EXPECT_FALSE(info.adapters[0].wifiDetailsWithheld) << "nothing to show: no button";
    EXPECT_TRUE(g_State.queries.empty());
    EXPECT_EQ(g_State.bssLists, 0);
}

TEST(WindowsNetworkAdaptersHelpers, Formatting)
{
    EXPECT_EQ(formatMac(std::array<std::uint8_t, 6>{0x00, 0x1a, 0x2B, 0x3c, 0x4d, 0x5e}), "00-1A-2B-3C-4D-5E");
    EXPECT_EQ(formatMac(std::array<std::uint8_t, 6>{}), "") << "all zeros is no address";
    EXPECT_EQ(formatMac({}), "");
    EXPECT_EQ(formatSocketAddress(SOCKET_ADDRESS{}), "");

    EXPECT_TRUE(isPlaceholderDnsServer("fec0:0:0:ffff::1"));
    EXPECT_TRUE(isPlaceholderDnsServer("fec0:0:0:ffff::3"));
    EXPECT_FALSE(isPlaceholderDnsServer("fe80::1"));

    EXPECT_EQ(phyName(dot11_phy_type_ht, 2437), "802.11n (Wi-Fi 4)");
    EXPECT_EQ(phyName(dot11_phy_type_vht, 5180), "802.11ac (Wi-Fi 5)");
    EXPECT_EQ(phyName(dot11_phy_type_he, 0), "802.11ax (Wi-Fi 6)");
    EXPECT_EQ(phyName(dot11_phy_type_eht, 6135), "802.11be (Wi-Fi 7)");
    EXPECT_EQ(phyName(dot11_phy_type_unknown, 0), "");

    EXPECT_EQ(Windows::guidText(WIFI_GUID), "{12345678-9ABC-DEF0-0123-456789ABCDEF}");
}

TEST(WindowsNetworkAdaptersReal, ReadsThisMachine)
{
    NetworkAdaptersInfo info;
    // Without the Wi-Fi details: the test run must never raise a Windows location prompt (#1782).
    readNetworkAdapters(info, systemFunctions(), /*includeWifiDetails=*/false);
    EXPECT_TRUE(info.available);
    EXPECT_TRUE(info.listed) << "GetAdaptersAddresses works on every Windows machine";
    bool anyGatewayV4 = false;
    for (const NetworkAdapter& adapter : info.adapters)
    {
        EXPECT_FALSE(adapter.name.empty());
        for (const AdapterAddress& address : adapter.addresses)
        {
            EXPECT_FALSE(address.address.empty());
            EXPECT_LE(address.prefix, address.v6 ? 128U : 32U);
        }
        const auto isV4 = [](const std::string& gateway)
        {
            return gateway.find(':') == std::string::npos;
        };
        anyGatewayV4 = anyGatewayV4 || (adapter.up && std::ranges::any_of(adapter.gateways, isV4));
    }
    // The routes are matched to the adapters (#2996): an up adapter's IPv4 gateway is a default route's.
    EXPECT_EQ(info.gatewayV4.empty(), !anyGatewayV4) << info.gatewayV4;
    EXPECT_EQ(info.gatewayV4.empty(), info.gatewayV4Adapter.empty());
}

} // namespace
} // namespace Platform::WindowsNetworkAdapters
