#pragma once

// The Vulkan versions Graphics & displays shows (#1635): the loader's instance version and each physical
// device's API and driver versions, with the LUID that matches a device to its DXGI adapter on Windows.
// TaskSmack neither links the Vulkan loader nor uses the SDK's headers. The few types and entry points
// read() needs are declared here, laid out as vulkan_core.h lays them out, and the caller passes in the
// loader's vkGetInstanceProcAddr, resolved at run time (LoadLibraryExW on Windows; dlopen on Linux).
// read() creates an instance without layers or extensions, lists the physical devices and destroys the
// instance before it returns. It never creates a device.
// Standard library only, so the tests drive read() with a fake loader on every platform.

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Platform::Vulkan
{

// VKAPI_CALL is __stdcall on 32-bit Windows and the default convention everywhere else. TaskSmack only
// builds 64-bit on Windows, where the two are the same, so the function pointer types below don't name one.
#ifdef _WIN32
static_assert(sizeof(void*) == 8, "Vulkan entry points are __stdcall on 32-bit Windows: declare VKAPI_CALL first");
#endif

// The Vulkan ABI subset read() uses: Vulkan 1.0 core, plus 1.1's vkGetPhysicalDeviceProperties2 and
// VkPhysicalDeviceIDProperties and 1.2's VkPhysicalDeviceDriverProperties.
using Result = std::int32_t;
inline constexpr Result SUCCESS = 0;
inline constexpr Result INCOMPLETE = 5;

inline constexpr std::int32_t STRUCTURE_TYPE_APPLICATION_INFO = 0;
inline constexpr std::int32_t STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
inline constexpr std::int32_t STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001;
inline constexpr std::int32_t STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004;
inline constexpr std::int32_t STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES = 1000196000;

// VkDriverId values that change how a device is decoded or matched.
inline constexpr std::uint32_t DRIVER_ID_NVIDIA_PROPRIETARY = 4;
inline constexpr std::uint32_t DRIVER_ID_INTEL_PROPRIETARY_WINDOWS = 5;
inline constexpr std::uint32_t DRIVER_ID_MESA_DOZEN = 23; ///< Mesa's Vulkan on Direct3D 12, layered over another driver

struct InstanceHandle;
struct PhysicalDeviceHandle;
using Instance = InstanceHandle*;             ///< VkInstance: dispatchable, so a pointer on every platform
using PhysicalDevice = PhysicalDeviceHandle*; ///< VkPhysicalDevice

struct ApplicationInfo
{
    std::int32_t sType = STRUCTURE_TYPE_APPLICATION_INFO;
    const void* pNext = nullptr;
    const char* pApplicationName = nullptr;
    std::uint32_t applicationVersion = 0;
    const char* pEngineName = nullptr;
    std::uint32_t engineVersion = 0;
    std::uint32_t apiVersion = 0;
};

struct InstanceCreateInfo
{
    std::int32_t sType = STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    const ApplicationInfo* pApplicationInfo = nullptr;
    std::uint32_t enabledLayerCount = 0;
    const char* const* ppEnabledLayerNames = nullptr;
    std::uint32_t enabledExtensionCount = 0;
    const char* const* ppEnabledExtensionNames = nullptr;
};

/// VkPhysicalDeviceProperties. Its last two members (VkPhysicalDeviceLimits and
/// VkPhysicalDeviceSparseProperties, 528 bytes together) aren't read, so they are declared as room for
/// the driver to write into, with plenty to spare.
struct PhysicalDeviceProperties
{
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorID = 0;
    std::uint32_t deviceID = 0;
    std::int32_t deviceType = 0;
    std::array<char, 256> deviceName{};
    std::array<std::uint8_t, 16> pipelineCacheUUID{};
    alignas(8) std::array<std::byte, 1024> limitsAndSparseProperties{};
};

/// VkPhysicalDeviceIDProperties (Vulkan 1.1).
struct PhysicalDeviceIdProperties
{
    std::int32_t sType = STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    void* pNext = nullptr;
    std::array<std::uint8_t, 16> deviceUUID{};
    std::array<std::uint8_t, 16> driverUUID{};
    std::array<std::uint8_t, 8> deviceLUID{};
    std::uint32_t deviceNodeMask = 0;
    std::uint32_t deviceLUIDValid = 0;
};

/// VkPhysicalDeviceDriverProperties (Vulkan 1.2).
struct PhysicalDeviceDriverProperties
{
    std::int32_t sType = STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    void* pNext = nullptr;
    std::int32_t driverID = 0;
    std::array<char, 256> driverName{};
    std::array<char, 256> driverInfo{};
    std::array<std::uint8_t, 4> conformanceVersion{};
};

/// VkPhysicalDeviceProperties2 (Vulkan 1.1).
struct PhysicalDeviceProperties2
{
    std::int32_t sType = STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    void* pNext = nullptr;
    PhysicalDeviceProperties properties{};
};

// The offsets vulkan_core.h gives them on 64-bit targets.
static_assert(sizeof(void*) != 8 || offsetof(PhysicalDeviceProperties, limitsAndSparseProperties) == 296);
static_assert(sizeof(void*) != 8 || offsetof(PhysicalDeviceIdProperties, deviceLUID) == 48);
static_assert(sizeof(void*) != 8 || offsetof(PhysicalDeviceIdProperties, deviceLUIDValid) == 60);
static_assert(sizeof(void*) != 8 || offsetof(PhysicalDeviceProperties2, properties) == 16);
static_assert(sizeof(void*) != 8 || sizeof(PhysicalDeviceDriverProperties) == 536);
static_assert(sizeof(void*) != 8 || offsetof(InstanceCreateInfo, pApplicationInfo) == 24);
static_assert(sizeof(void*) != 8 || offsetof(ApplicationInfo, apiVersion) == 44);

using VoidFunction = void (*)();
using GetInstanceProcAddrFn = VoidFunction (*)(Instance instance, const char* name);
using EnumerateInstanceVersionFn = Result (*)(std::uint32_t* apiVersion);
using CreateInstanceFn = Result (*)(const InstanceCreateInfo* createInfo, const void* allocator, Instance* instance);
using DestroyInstanceFn = void (*)(Instance instance, const void* allocator);
using EnumeratePhysicalDevicesFn = Result (*)(Instance instance, std::uint32_t* count, PhysicalDevice* devices);
using GetPhysicalDevicePropertiesFn = void (*)(PhysicalDevice device, PhysicalDeviceProperties* properties);
using GetPhysicalDeviceProperties2Fn = void (*)(PhysicalDevice device, PhysicalDeviceProperties2* properties);

/// VK_MAKE_API_VERSION with variant 0.
[[nodiscard]] constexpr std::uint32_t makeApiVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) noexcept
{
    return (major << 22U) | (minor << 12U) | patch;
}

inline constexpr std::uint32_t API_VERSION_1_0 = makeApiVersion(1, 0, 0);
inline constexpr std::uint32_t API_VERSION_1_1 = makeApiVersion(1, 1, 0);
inline constexpr std::uint32_t API_VERSION_1_2 = makeApiVersion(1, 2, 0);

/// An API version without its variant (the top three bits), so it compares as major.minor.patch.
[[nodiscard]] constexpr std::uint32_t withoutVariant(std::uint32_t version) noexcept
{
    return version & 0x1FFF'FFFFU;
}

/// An API version as vulkaninfo writes it (VK_API_VERSION_MAJOR/MINOR/PATCH): "1.4.303".
[[nodiscard]] inline std::string apiVersionText(std::uint32_t version)
{
    return std::format("{}.{}.{}", (version >> 22U) & 0x7FU, (version >> 12U) & 0x3FFU, version & 0xFFFU);
}

inline constexpr std::uint32_t VENDOR_NVIDIA = 0x10DE;
inline constexpr std::uint32_t VENDOR_INTEL = 0x8086;

/// A driver version in its driver's packing: NVIDIA's 10.8.8.6 bits ("560.94.0.0"), Intel's 18.14 on
/// Windows ("101.6881"), and VK_MAKE_VERSION's 10.10.12 for every other driver, Mesa's included
/// ("24.2.8"). @p driverId (VkDriverId; 0 when the device doesn't report it) says which driver it is;
/// without it the vendor and platform decide, as vulkaninfo decides. A Mesa driver on an NVIDIA or Intel
/// GPU (nouveau's NVK, Dozen on Windows) reports its own id, so it is decoded as Mesa.
[[nodiscard]] inline std::string driverVersionText(std::uint32_t vendorId, std::uint32_t driverId, std::uint32_t version, bool windows)
{
    const bool nvidia = driverId != 0 ? driverId == DRIVER_ID_NVIDIA_PROPRIETARY : vendorId == VENDOR_NVIDIA;
    const bool intelWindows = driverId != 0 ? driverId == DRIVER_ID_INTEL_PROPRIETARY_WINDOWS : vendorId == VENDOR_INTEL && windows;
    if (nvidia)
    {
        return std::format("{}.{}.{}.{}", (version >> 22U) & 0x3FFU, (version >> 14U) & 0xFFU, (version >> 6U) & 0xFFU, version & 0x3FU);
    }
    if (intelWindows)
    {
        return std::format("{}.{}", version >> 14U, version & 0x3FFFU);
    }
    return std::format("{}.{}.{}", version >> 22U, (version >> 12U) & 0x3FFU, version & 0xFFFU);
}

/// One physical device as read() reports it.
struct Device
{
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0; ///< In the driver's packing: driverVersionText()
    std::uint32_t driverId = 0;      ///< VkDriverId (Vulkan 1.2); 0 when the device doesn't report it
    /// VkPhysicalDeviceIDProperties::deviceLUID when the driver says it is valid (Windows, Vulkan 1.1).
    std::optional<std::array<std::uint8_t, 8>> luid;
};

/// What the loader reports.
struct Info
{
    std::uint32_t instanceVersion = API_VERSION_1_0; ///< A 1.0 loader has no vkEnumerateInstanceVersion
    std::vector<Device> devices;                     ///< Empty when no instance could be created (no driver) or none is listed
};

/// An entry point, cast from the PFN_vkVoidFunction vkGetInstanceProcAddr returns to its own type.
template<typename Fn> [[nodiscard]] Fn entryPoint(GetInstanceProcAddrFn getInstanceProcAddr, Instance instance, const char* name)
{
    // vkGetInstanceProcAddr's contract: the caller casts the result to the entry point's own type.
    return reinterpret_cast<Fn>(getInstanceProcAddr(instance, name)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// Destroys the instance when it leaves scope, so a throwing allocation can't leak it.
class InstanceScope
{
  public:
    InstanceScope(Instance instance, DestroyInstanceFn destroy) noexcept : m_Instance(instance), m_Destroy(destroy)
    {}
    ~InstanceScope()
    {
        m_Destroy(m_Instance, nullptr);
    }
    InstanceScope(const InstanceScope&) = delete;
    InstanceScope& operator=(const InstanceScope&) = delete;
    InstanceScope(InstanceScope&&) = delete;
    InstanceScope& operator=(InstanceScope&&) = delete;

  private:
    Instance m_Instance;
    DestroyInstanceFn m_Destroy;
};

/// Every physical device's handle; empty when the list can't be read.
[[nodiscard]] inline std::vector<PhysicalDevice> listPhysicalDevices(EnumeratePhysicalDevicesFn enumerate, Instance instance)
{
    std::vector<PhysicalDevice> devices;
    Result result = INCOMPLETE;
    int attempt = 0;
    // A device can appear between the count and the list (VK_INCOMPLETE): ask again, a few times at most.
    while (result == INCOMPLETE && attempt < 3)
    {
        ++attempt;
        std::uint32_t count = 0;
        if (enumerate(instance, &count, nullptr) != SUCCESS)
        {
            break;
        }
        devices.resize(count);
        result = enumerate(instance, &count, devices.data());
        devices.resize(count);
    }
    if (result != SUCCESS)
    {
        devices.clear();
    }
    return devices;
}

/// The loader's instance version and each physical device's versions, through @p getInstanceProcAddr;
/// nullopt without a loader (a null @p getInstanceProcAddr, or one that resolves no vkCreateInstance).
[[nodiscard]] inline std::optional<Info> read(GetInstanceProcAddrFn getInstanceProcAddr)
{
    if (getInstanceProcAddr == nullptr)
    {
        return std::nullopt;
    }
    const auto createInstance = entryPoint<CreateInstanceFn>(getInstanceProcAddr, nullptr, "vkCreateInstance");
    if (createInstance == nullptr)
    {
        return std::nullopt;
    }
    Info info;
    if (const auto enumerateVersion = entryPoint<EnumerateInstanceVersionFn>(getInstanceProcAddr, nullptr, "vkEnumerateInstanceVersion"))
    {
        std::uint32_t version = 0;
        if (enumerateVersion(&version) == SUCCESS)
        {
            info.instanceVersion = version;
        }
    }

    // A 1.0 loader rejects any other apiVersion. A later one takes 1.2 whatever its drivers support, and
    // then each device answers up to min(1.2, its own version): its LUID from 1.1, its driver id from 1.2.
    const bool instance11 = withoutVariant(info.instanceVersion) >= API_VERSION_1_1;
    const ApplicationInfo application{.pApplicationName = "TaskSmack", .apiVersion = instance11 ? API_VERSION_1_2 : API_VERSION_1_0};
    const InstanceCreateInfo create{.pApplicationInfo = &application};
    Instance instance = nullptr;
    if (createInstance(&create, nullptr, &instance) != SUCCESS || instance == nullptr)
    {
        return info; // VK_ERROR_INCOMPATIBLE_DRIVER: a loader without a driver
    }
    const auto destroyInstance = entryPoint<DestroyInstanceFn>(getInstanceProcAddr, instance, "vkDestroyInstance");
    if (destroyInstance == nullptr)
    {
        return info; // Not a conformant loader; nothing can release the instance
    }
    const InstanceScope scope(instance, destroyInstance);

    const auto enumerate = entryPoint<EnumeratePhysicalDevicesFn>(getInstanceProcAddr, instance, "vkEnumeratePhysicalDevices");
    const auto properties = entryPoint<GetPhysicalDevicePropertiesFn>(getInstanceProcAddr, instance, "vkGetPhysicalDeviceProperties");
    const auto properties2 =
        instance11 ? entryPoint<GetPhysicalDeviceProperties2Fn>(getInstanceProcAddr, instance, "vkGetPhysicalDeviceProperties2") : nullptr;
    if (enumerate == nullptr || properties == nullptr)
    {
        return info;
    }
    for (auto* const handle : listPhysicalDevices(enumerate, instance))
    {
        PhysicalDeviceProperties2 queried;
        properties(handle, &queried.properties);
        Device device{
            .vendorId = queried.properties.vendorID,
            .deviceId = queried.properties.deviceID,
            .apiVersion = queried.properties.apiVersion,
            .driverVersion = queried.properties.driverVersion,
            .driverId = 0,
            .luid = std::nullopt,
        };
        // vkGetPhysicalDeviceProperties2 is valid only for a device of 1.1 or later, and the driver
        // properties only for one of 1.2 or later.
        const std::uint32_t deviceVersion = withoutVariant(device.apiVersion);
        if (properties2 != nullptr && deviceVersion >= API_VERSION_1_1)
        {
            PhysicalDeviceIdProperties id;
            PhysicalDeviceDriverProperties driver;
            id.pNext = deviceVersion >= API_VERSION_1_2 ? &driver : nullptr;
            queried.pNext = &id;
            properties2(handle, &queried);
            if (id.deviceLUIDValid != 0U)
            {
                device.luid = id.deviceLUID;
            }
            device.driverId = driver.driverID > 0 ? static_cast<std::uint32_t>(driver.driverID) : 0U;
        }
        info.devices.push_back(device);
    }
    return info;
}

} // namespace Platform::Vulkan
