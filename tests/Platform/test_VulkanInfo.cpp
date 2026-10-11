/// @file test_VulkanInfo.cpp
/// @brief Platform::Vulkan (#1635): the API and vendor driver version decodings, and read() against a fake
/// loader: no loader, a 1.0 loader, a loader without a driver, the devices with and without a LUID, a list
/// that grows between the count and the read (VK_INCOMPLETE), and the instance destroyed exactly once.
/// Standard library only, so it runs everywhere.

#include "Platform/VulkanInfo.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace Platform::Vulkan
{
namespace
{

/// One device the fake loader lists.
struct FakeDevice
{
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::int32_t driverId = 0;
    std::array<std::uint8_t, 8> luid{};
    bool luidValid = false;
};

/// The fake loader's behaviour and what read() asked of it.
struct FakeLoader
{
    bool hasCreateInstance = true;
    std::optional<std::uint32_t> instanceVersion; ///< nullopt: a 1.0 loader without vkEnumerateInstanceVersion
    Result createResult = SUCCESS;
    std::vector<FakeDevice> devices;
    int growAfterCount = 0; ///< Times the list reports VK_INCOMPLETE before it answers in full

    std::uint32_t requestedApiVersion = 0;
    std::uint32_t requestedLayers = 0;
    std::uint32_t requestedExtensions = 0;
    int created = 0;
    int destroyed = 0;
    int properties2Calls = 0;
    int driverQueries = 0;
};

FakeLoader g_Loader; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] Instance fakeInstance()
{
    return std::bit_cast<Instance>(std::uintptr_t{0x1635});
}

[[nodiscard]] PhysicalDevice handleOf(std::size_t index)
{
    return std::bit_cast<PhysicalDevice>(static_cast<std::uintptr_t>(index + 1));
}

[[nodiscard]] const FakeDevice& deviceOf(PhysicalDevice handle)
{
    return g_Loader.devices.at(static_cast<std::size_t>(std::bit_cast<std::uintptr_t>(handle)) - 1);
}

Result fakeEnumerateInstanceVersion(std::uint32_t* version)
{
    *version = g_Loader.instanceVersion.value_or(0);
    return SUCCESS;
}

Result fakeCreateInstance(const InstanceCreateInfo* create, const void* /*allocator*/, Instance* instance)
{
    g_Loader.requestedApiVersion = create->pApplicationInfo != nullptr ? create->pApplicationInfo->apiVersion : 0;
    g_Loader.requestedLayers = create->enabledLayerCount;
    g_Loader.requestedExtensions = create->enabledExtensionCount;
    if (g_Loader.createResult != SUCCESS)
    {
        return g_Loader.createResult;
    }
    ++g_Loader.created;
    *instance = fakeInstance();
    return SUCCESS;
}

void fakeDestroyInstance(Instance instance, const void* /*allocator*/)
{
    EXPECT_EQ(instance, fakeInstance());
    ++g_Loader.destroyed;
}

Result fakeEnumeratePhysicalDevices(Instance /*instance*/, std::uint32_t* count, PhysicalDevice* devices)
{
    const auto total = static_cast<std::uint32_t>(g_Loader.devices.size());
    if (devices == nullptr)
    {
        *count = total;
        if (g_Loader.growAfterCount > 0)
        {
            --g_Loader.growAfterCount;
            *count = total - 1; // one more device arrives before the list is read
        }
        return SUCCESS;
    }
    const std::uint32_t written = *count < total ? *count : total;
    std::size_t index = 0;
    while (index < written)
    {
        devices[index] = handleOf(index); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ++index;
    }
    *count = written;
    return written < total ? INCOMPLETE : SUCCESS;
}

void fakeProperties(PhysicalDevice handle, PhysicalDeviceProperties* properties)
{
    const FakeDevice& device = deviceOf(handle);
    properties->vendorID = device.vendorId;
    properties->deviceID = device.deviceId;
    properties->apiVersion = device.apiVersion;
    properties->driverVersion = device.driverVersion;
}

void fakeProperties2(PhysicalDevice handle, PhysicalDeviceProperties2* properties)
{
    ++g_Loader.properties2Calls;
    EXPECT_EQ(properties->sType, STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    fakeProperties(handle, &properties->properties);
    auto* const id = static_cast<PhysicalDeviceIdProperties*>(properties->pNext);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->sType, STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    const FakeDevice& device = deviceOf(handle);
    id->deviceLUID = device.luid;
    id->deviceLUIDValid = device.luidValid ? 1U : 0U;
    if (id->pNext != nullptr)
    {
        ++g_Loader.driverQueries;
        EXPECT_GE(device.apiVersion, API_VERSION_1_2); // asked only of a device that has it
        auto* const driver = static_cast<PhysicalDeviceDriverProperties*>(id->pNext);
        EXPECT_EQ(driver->sType, STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        driver->driverID = device.driverId;
    }
}

template<typename Fn> [[nodiscard]] VoidFunction asVoidFunction(Fn function)
{
    return reinterpret_cast<VoidFunction>(function); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

VoidFunction fakeGetInstanceProcAddr(Instance instance, const char* name)
{
    const auto entry = [name](const char* entryPoint)
    {
        return std::strcmp(name, entryPoint) == 0;
    };
    if (instance == nullptr)
    {
        if (entry("vkCreateInstance") && g_Loader.hasCreateInstance)
        {
            return asVoidFunction(&fakeCreateInstance);
        }
        if (entry("vkEnumerateInstanceVersion") && g_Loader.instanceVersion.has_value())
        {
            return asVoidFunction(&fakeEnumerateInstanceVersion);
        }
        return nullptr;
    }
    if (entry("vkDestroyInstance"))
    {
        return asVoidFunction(&fakeDestroyInstance);
    }
    if (entry("vkEnumeratePhysicalDevices"))
    {
        return asVoidFunction(&fakeEnumeratePhysicalDevices);
    }
    if (entry("vkGetPhysicalDeviceProperties"))
    {
        return asVoidFunction(&fakeProperties);
    }
    if (entry("vkGetPhysicalDeviceProperties2"))
    {
        return asVoidFunction(&fakeProperties2);
    }
    return nullptr;
}

/// read() through the fake loader, with an instanceVersion of 0 when it found none. A plain value keeps
/// clang-tidy's optional dataflow satisfied: bugprone-unchecked-optional-access doesn't model gtest's
/// ASSERT_TRUE early return.
[[nodiscard]] Info readFake()
{
    return read(&fakeGetInstanceProcAddr).value_or(Info{.instanceVersion = 0, .devices = {}});
}

constexpr std::array<std::uint8_t, 8> NVIDIA_LUID{0x34, 0x12, 0, 0, 0, 0, 0, 0};

[[nodiscard]] FakeDevice nvidiaDevice()
{
    return {
        .vendorId = VENDOR_NVIDIA,
        .deviceId = 0x2786,
        .apiVersion = makeApiVersion(1, 4, 303),
        .driverVersion = (560U << 22U) | (94U << 14U),
        .driverId = static_cast<std::int32_t>(DRIVER_ID_NVIDIA_PROPRIETARY),
        .luid = NVIDIA_LUID,
        .luidValid = true,
    };
}

TEST(VulkanInfoTest, ApiVersionText)
{
    EXPECT_EQ(apiVersionText(makeApiVersion(1, 3, 290)), "1.3.290");
    EXPECT_EQ(apiVersionText(makeApiVersion(1, 0, 0)), "1.0.0");
    EXPECT_EQ(apiVersionText(makeApiVersion(1, 4, 4095)), "1.4.4095");
    // The variant (the top three bits) isn't part of the version shown.
    EXPECT_EQ(apiVersionText((1U << 29U) | makeApiVersion(1, 2, 3)), "1.2.3");
    EXPECT_EQ(withoutVariant((7U << 29U) | API_VERSION_1_1), API_VERSION_1_1);
}

TEST(VulkanInfoTest, DriverVersionTextPerVendor)
{
    // NVIDIA: 10.8.8.6 bits, on every platform.
    EXPECT_EQ(driverVersionText(VENDOR_NVIDIA, 0, (560U << 22U) | (94U << 14U), true), "560.94.0.0");
    EXPECT_EQ(driverVersionText(VENDOR_NVIDIA, 0, (535U << 22U) | (183U << 14U) | (1U << 6U) | 2U, false), "535.183.1.2");
    EXPECT_EQ(driverVersionText(VENDOR_NVIDIA, DRIVER_ID_NVIDIA_PROPRIETARY, (560U << 22U) | (94U << 14U), true), "560.94.0.0");
    // Intel on Windows: 18.14 bits.
    EXPECT_EQ(driverVersionText(VENDOR_INTEL, 0, (101U << 14U) | 6881U, true), "101.6881");
    EXPECT_EQ(driverVersionText(VENDOR_INTEL, DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, (101U << 14U) | 8860U, true), "101.8860");
    // Intel on Linux (Mesa) and everyone else: VK_MAKE_VERSION.
    EXPECT_EQ(driverVersionText(VENDOR_INTEL, 0, makeApiVersion(24, 2, 8), false), "24.2.8");
    EXPECT_EQ(driverVersionText(0x1002, 0, makeApiVersion(2, 0, 317), true), "2.0.317");
    // A Mesa driver on an Intel or NVIDIA GPU says so: Dozen on Windows, NVK (driver id 24) on Linux.
    EXPECT_EQ(driverVersionText(VENDOR_INTEL, DRIVER_ID_MESA_DOZEN, makeApiVersion(26, 2, 99), true), "26.2.99");
    EXPECT_EQ(driverVersionText(VENDOR_NVIDIA, 24, makeApiVersion(25, 1, 0), false), "25.1.0");
}

TEST(VulkanInfoTest, NoLoader)
{
    EXPECT_FALSE(read(nullptr).has_value());

    g_Loader = {};
    g_Loader.hasCreateInstance = false;
    EXPECT_FALSE(read(&fakeGetInstanceProcAddr).has_value());
    EXPECT_EQ(g_Loader.created, 0);
}

TEST(VulkanInfoTest, ReadsDevicesAndDestroysTheInstance)
{
    g_Loader = {};
    g_Loader.instanceVersion = makeApiVersion(1, 4, 304);
    const FakeDevice software{
        .vendorId = 0x10005,
        .deviceId = 0,
        .apiVersion = makeApiVersion(1, 3, 0),
        .driverVersion = 1,
        .driverId = 10,
    };
    const FakeDevice old{.vendorId = 0x1002, .deviceId = 0x73BF, .apiVersion = API_VERSION_1_0, .driverVersion = makeApiVersion(2, 0, 1)};
    const FakeDevice v11{.vendorId = 0x1002, .deviceId = 0x73BF, .apiVersion = makeApiVersion(1, 1, 99), .driverVersion = 1, .driverId = 1};
    g_Loader.devices = {nvidiaDevice(), software, old, v11};

    const Info info = readFake();
    ASSERT_NE(info.instanceVersion, 0U);
    EXPECT_EQ(info.instanceVersion, makeApiVersion(1, 4, 304));
    EXPECT_EQ(g_Loader.requestedApiVersion, API_VERSION_1_2);
    EXPECT_EQ(g_Loader.requestedLayers, 0U);
    EXPECT_EQ(g_Loader.requestedExtensions, 0U);
    EXPECT_EQ(g_Loader.created, 1);
    EXPECT_EQ(g_Loader.destroyed, 1);

    ASSERT_EQ(info.devices.size(), 4U);
    EXPECT_EQ(info.devices[0].vendorId, VENDOR_NVIDIA);
    EXPECT_EQ(info.devices[0].deviceId, 0x2786U);
    EXPECT_EQ(info.devices[0].apiVersion, makeApiVersion(1, 4, 303));
    EXPECT_EQ(info.devices[0].driverVersion, (560U << 22U) | (94U << 14U));
    EXPECT_EQ(info.devices[0].luid, NVIDIA_LUID);
    EXPECT_EQ(info.devices[0].driverId, DRIVER_ID_NVIDIA_PROPRIETARY);
    EXPECT_FALSE(info.devices[1].luid.has_value()); // the driver says its LUID isn't valid
    EXPECT_EQ(info.devices[1].driverId, 10U);
    EXPECT_FALSE(info.devices[2].luid.has_value()); // a 1.0 device isn't asked
    EXPECT_EQ(info.devices[2].driverId, 0U);
    EXPECT_EQ(info.devices[3].driverId, 0U); // a 1.1 device has no driver properties to ask for
    EXPECT_EQ(g_Loader.properties2Calls, 3);
    EXPECT_EQ(g_Loader.driverQueries, 2);
}

TEST(VulkanInfoTest, OneZeroLoader)
{
    // No vkEnumerateInstanceVersion: Vulkan 1.0, an instance of 1.0 and no vkGetPhysicalDeviceProperties2.
    g_Loader = {};
    g_Loader.devices = {nvidiaDevice()};
    const Info info = readFake();
    ASSERT_NE(info.instanceVersion, 0U);
    EXPECT_EQ(info.instanceVersion, API_VERSION_1_0);
    EXPECT_EQ(g_Loader.requestedApiVersion, API_VERSION_1_0);
    ASSERT_EQ(info.devices.size(), 1U);
    EXPECT_FALSE(info.devices[0].luid.has_value());
    EXPECT_EQ(g_Loader.properties2Calls, 0);
    EXPECT_EQ(g_Loader.destroyed, 1);
}

TEST(VulkanInfoTest, LoaderWithoutADriver)
{
    // vkCreateInstance fails (VK_ERROR_INCOMPATIBLE_DRIVER): the loader's version still, no devices.
    g_Loader = {};
    g_Loader.instanceVersion = makeApiVersion(1, 3, 290);
    g_Loader.createResult = -9;
    g_Loader.devices = {nvidiaDevice()};
    const Info info = readFake();
    ASSERT_NE(info.instanceVersion, 0U);
    EXPECT_EQ(apiVersionText(info.instanceVersion), "1.3.290");
    EXPECT_TRUE(info.devices.empty());
    EXPECT_EQ(g_Loader.destroyed, 0);
}

TEST(VulkanInfoTest, ListGrowsBetweenCountAndRead)
{
    g_Loader = {};
    g_Loader.instanceVersion = makeApiVersion(1, 4, 304);
    g_Loader.devices = {nvidiaDevice(), nvidiaDevice()};
    g_Loader.growAfterCount = 1;
    const Info info = readFake();
    ASSERT_NE(info.instanceVersion, 0U);
    EXPECT_EQ(info.devices.size(), 2U); // asked again after VK_INCOMPLETE

    // A list that never settles is given up on, and the instance is still destroyed.
    g_Loader = {};
    g_Loader.instanceVersion = makeApiVersion(1, 4, 304);
    g_Loader.devices = {nvidiaDevice(), nvidiaDevice()};
    g_Loader.growAfterCount = 5;
    const Info unsettled = readFake();
    ASSERT_NE(unsettled.instanceVersion, 0U);
    EXPECT_TRUE(unsettled.devices.empty());
    EXPECT_EQ(g_Loader.destroyed, 1);
}

} // namespace
} // namespace Platform::Vulkan
