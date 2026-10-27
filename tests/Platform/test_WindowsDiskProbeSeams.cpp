/// @file test_WindowsDiskProbeSeams.cpp
/// @brief WindowsDiskProbe against fake PDH, drive-open and DeviceIoControl calls (#1719): the
/// localized PhysicalDisk object name (#2994), the logical-drive fallback, a disk whose IOCTL_DISK_PERFORMANCE read fails, open failures
/// with their warn-once logging, rescans and the busy-time clock, and the device notifications that say when to rescan (#3151). No drive
/// is opened: the fake "drives" are event handles, the fake DeviceIoControl answers IOCTL_DISK_PERFORMANCE only, and a test simulates a
/// disk arriving by calling the callback the probe registered.

#include "Platform/StorageTypes.h"
#include "Platform/Windows/WindowsDiskProbe.h"
#include "Platform/Windows/WindowsDiskProbeMath.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
// clang-format on

// NOLINTBEGIN(misc-include-cleaner,readability-non-const-parameter)
// Win32 types; the fakes keep the exact Win32 signatures.
namespace Platform
{
namespace
{

using namespace std::chrono_literals;

// GUID_DEVINTERFACE_DISK and GUID_DEVINTERFACE_VOLUME (winioctl.h)
constexpr GUID DISK_INTERFACE{
    .Data1 = 0x53f56307, .Data2 = 0xb6bf, .Data3 = 0x11d0, .Data4 = {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr GUID VOLUME_INTERFACE{
    .Data1 = 0x53f5630d, .Data2 = 0xb6bf, .Data3 = 0x11d0, .Data4 = {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

/// One CM_Register_Notification the probe made: what it asked for, and how to call it back.
struct Registration
{
    GUID interfaceClass{};
    PVOID context = nullptr;
    PCM_NOTIFY_CALLBACK callback = nullptr;
    HCMNOTIFICATION handle = nullptr;
};

struct OpenDrive
{
    int index = -1;
    int ioctlCalls = 0; // The first is the probe's at open time
};

/// What every fake returns, and what the probe asked of them.
struct FakeDisks // NOLINT(clang-analyzer-optin.performance.Padding)
{
    // The only name the fake PDH knows PhysicalDisk by: a localized system's, as on a German Windows
    // (#2994). The English name is no object here, as it is on such a system.
    std::wstring objectName = L"PhysischerDatentr\u00E4ger";
    bool failLookup = false;
    DWORD lookupNeeds = 0; // Characters the lookup wants room for; 0 for the name and its terminator
    int lookups = 0;
    std::vector<std::wstring> enumeratedAs; // The object name of every PdhEnumObjectItemsW call
    std::vector<std::wstring> instances{L"0 C:", L"1 D: E:", L"_Total"};
    bool failRefresh = false;
    bool failSizing = false;
    bool failItems = false;
    int refreshes = 0;
    int failRegistration = -1;        // The registration (0 the first) that fails; -1 for none
    bool notifyDuringRefresh = false; // A disk arrives while PdhEnumObjectsW runs
    std::vector<Registration> registrations;
    std::vector<HCMNOTIFICATION> unregistered;
    std::set<int> failOpen;
    std::set<int> failProbe;
    std::set<int> failRead;
    std::map<int, DISK_PERFORMANCE> perf;
    std::map<HANDLE, OpenDrive> handles;
    std::vector<DWORD> otherIoctls; // Any control code other than IOCTL_DISK_PERFORMANCE
    DWORD logicalDrives = 0;
    bool failLogicalDrives = false;
    std::map<wchar_t, UINT> driveTypes;
};

FakeDisks& fake()
{
    static FakeDisks state;
    return state;
}

[[nodiscard]] DISK_PERFORMANCE defaultPerf(int index)
{
    DISK_PERFORMANCE perf{};
    perf.BytesRead.QuadPart = 1'048'576LL * (index + 1);
    perf.BytesWritten.QuadPart = 5'120;
    perf.ReadCount = 7;
    perf.WriteCount = 3;
    perf.ReadTime.QuadPart = 50'000;  // 5 ms
    perf.WriteTime.QuadPart = 30'000; // 3 ms
    return perf;                      // IdleTime 0: the driver does not track it
}

PDH_STATUS WINAPI fakeLookupPerfName(LPCWSTR machine, DWORD index, LPWSTR name, LPDWORD size)
{
    auto& f = fake();
    ++f.lookups;
    EXPECT_EQ(machine, nullptr);
    EXPECT_EQ(index, 234U) << "PhysicalDisk's index";
    if (f.failLookup)
    {
        return static_cast<PDH_STATUS>(PDH_INVALID_ARGUMENT);
    }
    const auto needed = std::max(static_cast<DWORD>(f.objectName.size() + 1), f.lookupNeeds);
    if (*size < needed)
    {
        // The documented contract: a size-only call (0) is told the size; a too-small buffer is not
        // reliably, so the fake tells it a wrong one.
        *size = (*size == 0) ? needed : 1;
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }
    EXPECT_NE(name, nullptr);
    std::ranges::copy(f.objectName, name);
    name[f.objectName.size()] = L'\0';
    *size = static_cast<DWORD>(f.objectName.size() + 1);
    return ERROR_SUCCESS;
}

/// A disk (or volume) arriving or leaving, or @p action whatever it is: the callback registered for
/// @p interfaceClass is called, as CfgMgr32 would call it on its own thread.
void notify(CM_NOTIFY_ACTION action, const GUID& interfaceClass = DISK_INTERFACE)
{
    for (const Registration& registration : fake().registrations)
    {
        if (registration.interfaceClass == interfaceClass &&
            std::ranges::find(fake().unregistered, registration.handle) == fake().unregistered.end())
        {
            EXPECT_EQ(registration.callback(registration.handle, registration.context, action, nullptr, 0),
                      static_cast<DWORD>(ERROR_SUCCESS));
        }
    }
}

PDH_STATUS WINAPI
fakeEnumObjects(LPCWSTR /*source*/, LPCWSTR /*machine*/, PZZWSTR /*list*/, LPDWORD /*size*/, DWORD /*detail*/, BOOL refresh)
{
    ++fake().refreshes;
    EXPECT_TRUE(refresh != FALSE);
    if (fake().notifyDuringRefresh)
    {
        fake().notifyDuringRefresh = false;
        notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL);
    }
    return fake().failRefresh ? static_cast<PDH_STATUS>(PDH_CSTATUS_NO_MACHINE) : static_cast<PDH_STATUS>(PDH_MORE_DATA);
}

PDH_STATUS WINAPI fakeEnumObjectItems(LPCWSTR /*source*/,
                                      LPCWSTR /*machine*/,
                                      LPCWSTR object,
                                      PZZWSTR counters,
                                      LPDWORD counterLength,
                                      PZZWSTR instances,
                                      LPDWORD instanceLength,
                                      DWORD /*detail*/,
                                      DWORD /*flags*/)
{
    fake().enumeratedAs.emplace_back(object);
    if (std::wstring_view(object) != fake().objectName)
    {
        return static_cast<PDH_STATUS>(PDH_CSTATUS_NO_OBJECT);
    }
    std::wstring list;
    for (const auto& name : fake().instances)
    {
        list += name;
        list += L'\0';
    }
    list += L'\0';
    if (counters == nullptr)
    {
        if (fake().failSizing)
        {
            return static_cast<PDH_STATUS>(PDH_CSTATUS_NO_OBJECT);
        }
        *counterLength = 2;
        *instanceLength = static_cast<DWORD>(list.size());
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }
    if (fake().failItems || *instanceLength < list.size())
    {
        return static_cast<PDH_STATUS>(PDH_CSTATUS_NO_OBJECT);
    }
    counters[0] = L'\0';
    counters[1] = L'\0';
    std::ranges::copy(list, instances);
    return ERROR_SUCCESS;
}

Windows::UniqueHandle fakeOpenDrive(int index)
{
    if (fake().failOpen.contains(index))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return {};
    }
    // A real handle for the probe to own and close; no drive is opened
    Windows::UniqueHandle handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    fake().handles[handle.get()] = OpenDrive{.index = index};
    return handle;
}

BOOL WINAPI fakeDeviceIoControl(
    HANDLE device, DWORD code, LPVOID /*in*/, DWORD /*inSize*/, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    auto& f = fake();
    if (code != IOCTL_DISK_PERFORMANCE || outSize < sizeof(DISK_PERFORMANCE))
    {
        f.otherIoctls.push_back(code);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    const auto it = f.handles.find(device);
    if (it == f.handles.end())
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const int index = it->second.index;
    const bool probe = it->second.ioctlCalls++ == 0;
    if ((probe && f.failProbe.contains(index)) || (!probe && f.failRead.contains(index)))
    {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    const auto perf = f.perf.find(index);
    *static_cast<DISK_PERFORMANCE*>(out) = (perf != f.perf.end()) ? perf->second : defaultPerf(index);
    *returned = sizeof(DISK_PERFORMANCE);
    return TRUE;
}

DWORD WINAPI fakeGetLogicalDrives()
{
    if (fake().failLogicalDrives)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return fake().logicalDrives;
}

UINT WINAPI fakeGetDriveType(LPCWSTR root)
{
    const auto it = fake().driveTypes.find(root[0]);
    return it != fake().driveTypes.end() ? it->second : static_cast<UINT>(DRIVE_NO_ROOT_DIR);
}

CONFIGRET WINAPI fakeRegisterNotification(PCM_NOTIFY_FILTER filter, PVOID context, PCM_NOTIFY_CALLBACK callback, PHCMNOTIFICATION handle)
{
    auto& f = fake();
    EXPECT_EQ(filter->cbSize, sizeof(CM_NOTIFY_FILTER));
    EXPECT_EQ(filter->FilterType, CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE);
    EXPECT_EQ(filter->Flags, 0U);
    if (static_cast<int>(f.registrations.size()) == f.failRegistration)
    {
        f.failRegistration = -1;
        return CR_FAILURE;
    }
    // Any distinct non-null value: the probe only hands it back to unregister it.
    *handle = reinterpret_cast<HCMNOTIFICATION>(static_cast<std::uintptr_t>(f.registrations.size() + 1));
    f.registrations.push_back(
        Registration{.interfaceClass = filter->u.DeviceInterface.ClassGuid, .context = context, .callback = callback, .handle = *handle});
    return CR_SUCCESS;
}

CONFIGRET WINAPI fakeUnregisterNotification(HCMNOTIFICATION handle)
{
    fake().unregistered.push_back(handle);
    return CR_SUCCESS;
}

/// The fake monotonic clock, apart from FakeDisks so reading it cannot throw.
std::chrono::steady_clock::time_point& fakeClock() noexcept
{
    static std::chrono::steady_clock::time_point now;
    return now;
}

std::chrono::steady_clock::time_point fakeNow() noexcept
{
    return fakeClock();
}

[[nodiscard]] WindowsDiskProbeFunctions fakeFunctions()
{
    WindowsDiskProbeFunctions fns;
    fns.pdh.lookupPerfNameByIndexW = &fakeLookupPerfName;
    fns.pdh.enumObjectsW = &fakeEnumObjects;
    fns.pdh.enumObjectItemsW = &fakeEnumObjectItems;
    fns.drive.openPhysicalDriveQueryOnly = &fakeOpenDrive;
    fns.drive.deviceIoControl = &fakeDeviceIoControl;
    fns.drive.getLogicalDrives = &fakeGetLogicalDrives;
    fns.drive.getDriveTypeW = &fakeGetDriveType;
    fns.devices.registerNotification = &fakeRegisterNotification;
    fns.devices.unregisterNotification = &fakeUnregisterNotification;
    fns.now = &fakeNow;
    return fns;
}

[[nodiscard]] std::vector<std::string> names(const SystemDiskCounters& counters)
{
    std::vector<std::string> out;
    out.reserve(counters.disks.size());
    for (const auto& disk : counters.disks)
    {
        out.push_back(disk.deviceName);
    }
    return out;
}

class WindowsDiskProbeSeamsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeDisks{};
        fakeClock() = std::chrono::steady_clock::time_point{} + 1h;
        m_Previous = spdlog::default_logger();
        const auto logger = std::make_shared<spdlog::logger>("disk-seams", std::make_shared<spdlog::sinks::ostream_sink_st>(m_Log));
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("%l|%v");
        spdlog::set_default_logger(logger);
    }

    void TearDown() override
    {
        spdlog::set_default_logger(m_Previous);
        fake() = FakeDisks{};
    }

    /// How many lines at @p level ("warning", "debug") contain @p text.
    [[nodiscard]] std::size_t logged(std::string_view level, std::string_view text) const
    {
        std::size_t found = 0;
        std::istringstream lines(m_Log.str());
        std::string line;
        while (std::getline(lines, line))
        {
            if (line.starts_with(level) && line.contains(text))
            {
                ++found;
            }
        }
        return found;
    }

    std::ostringstream m_Log;
    std::shared_ptr<spdlog::logger> m_Previous;
};

} // namespace

TEST_F(WindowsDiskProbeSeamsTest, PhysicalDisksAreReadThroughDiskPerformanceOnly)
{
    WindowsDiskProbe probe(fakeFunctions());
    const SystemDiskCounters counters = probe.read();

    ASSERT_EQ(names(counters), (std::vector<std::string>{"0 C:", "1 D: E:"}));
    const DiskCounters& second = counters.disks[1];
    EXPECT_EQ(second.readSectors, 2ULL * 1'048'576 / 512);
    EXPECT_EQ(second.writeSectors, 10U);
    EXPECT_EQ(second.readsCompleted, 7U);
    EXPECT_EQ(second.writesCompleted, 3U);
    EXPECT_EQ(second.readTimeMs, 5U);
    EXPECT_EQ(second.writeTimeMs, 3U);
    EXPECT_EQ(second.ioTimeMs, 8U) << "no IdleTime: busy time falls back to read + write time";
    EXPECT_TRUE(second.isPhysicalDevice);
    EXPECT_TRUE(probe.capabilities().hasReadWriteBytes);
    EXPECT_TRUE(probe.capabilities().hasIoTime);
    EXPECT_TRUE(fake().otherIoctls.empty());
}

TEST_F(WindowsDiskProbeSeamsTest, EachReadReportsTheDisksCurrentCounters)
{
    // The deterministic form of "the counters rise with disk I/O" (#2298): every read queries
    // IOCTL_DISK_PERFORMANCE afresh, so I/O between two reads shows in the second, by exactly
    // as much as the disk reported.
    fake().failOpen = {1};
    WindowsDiskProbe probe(fakeFunctions());
    const DiskCounters before = probe.read().disks.at(0);

    DISK_PERFORMANCE perf = defaultPerf(0);
    perf.BytesRead.QuadPart += 4LL * 1'048'576; // 4 MiB read in 4 requests
    perf.ReadCount += 4;
    perf.ReadTime.QuadPart += 20'000;              // 2 ms
    perf.BytesWritten.QuadPart += 2LL * 1'048'576; // 2 MiB written in 2 requests
    perf.WriteCount += 2;
    perf.WriteTime.QuadPart += 10'000; // 1 ms
    fake().perf[0] = perf;
    const DiskCounters after = probe.read().disks.at(0);

    EXPECT_EQ(after.readSectors - before.readSectors, 4ULL * 1'048'576 / 512);
    EXPECT_EQ(after.readsCompleted - before.readsCompleted, 4U);
    EXPECT_EQ(after.readTimeMs - before.readTimeMs, 2U);
    EXPECT_EQ(after.writeSectors - before.writeSectors, 2ULL * 1'048'576 / 512);
    EXPECT_EQ(after.writesCompleted - before.writesCompleted, 2U);
    EXPECT_EQ(after.writeTimeMs - before.writeTimeMs, 1U);
    EXPECT_EQ(after.ioTimeMs - before.ioTimeMs, 3U) << "no IdleTime: busy time is read + write time";

    EXPECT_EQ(probe.read().disks.at(0).readSectors, after.readSectors) << "no I/O, no change";
}

TEST_F(WindowsDiskProbeSeamsTest, NoOpenableDriveFallsBackToTheFixedLogicalDrives)
{
    fake().failOpen = {0, 1};
    fake().logicalDrives = (1U << 2U) | (1U << 3U) | (1U << 4U) | (1U << 5U); // C, D, E, F
    fake().driveTypes = {{L'C', DRIVE_FIXED}, {L'D', DRIVE_CDROM}, {L'E', DRIVE_REMOVABLE}, {L'F', DRIVE_FIXED}};
    WindowsDiskProbe probe(fakeFunctions());
    const SystemDiskCounters counters = probe.read();

    EXPECT_EQ(names(counters), (std::vector<std::string>{"C:", "F:"}));
    EXPECT_EQ(counters.disks[0].readSectors, 0U);
    EXPECT_FALSE(counters.disks[0].hasIoCounters) << "zeros nothing measured, not an idle disk (#2994)";
    EXPECT_FALSE(counters.disks[1].hasIoCounters);
    EXPECT_FALSE(probe.capabilities().hasReadWriteBytes);
    EXPECT_FALSE(probe.capabilities().hasIoTime);
    EXPECT_TRUE(probe.capabilities().hasDiskStats);
    EXPECT_EQ(logged("warning", "CreateFileW failed"), 2U);
}

TEST_F(WindowsDiskProbeSeamsTest, FailedLogicalDrivesGivesNoDisks)
{
    fake().failOpen = {0, 1};
    fake().failLogicalDrives = true;
    WindowsDiskProbe probe(fakeFunctions());
    EXPECT_TRUE(probe.read().disks.empty());
    EXPECT_EQ(logged("warning", "GetLogicalDrives failed"), 1U);
}

TEST_F(WindowsDiskProbeSeamsTest, EachPdhEnumerationFailureFallsBackToLogicalDrives)
{
    for (int failing = 0; failing < 3; ++failing)
    {
        fake() = FakeDisks{};
        fake().failRefresh = failing == 0;
        fake().failSizing = failing == 1;
        fake().failItems = failing == 2;
        fake().logicalDrives = 1U << 2U;
        fake().driveTypes = {{L'C', DRIVE_FIXED}};
        WindowsDiskProbe probe(fakeFunctions());
        const SystemDiskCounters counters = probe.read();
        EXPECT_EQ(names(counters), (std::vector<std::string>{"C:"})) << failing;
        EXPECT_FALSE(counters.disks.at(0).hasIoCounters) << failing;
        EXPECT_FALSE(probe.capabilities().hasReadWriteBytes) << failing;
        EXPECT_TRUE(fake().handles.empty()) << failing << ": no drive is opened";
    }
}

TEST_F(WindowsDiskProbeSeamsTest, PhysicalDiskIsEnumeratedByItsLocalizedName)
{
    // #2994: PdhEnumObjectItemsW takes the object's localized name, and a German Windows has no
    // "PhysicalDisk": every enumeration failed there, leaving the logical-drive fallback's zeros.
    WindowsDiskProbe probe(fakeFunctions());
    const SystemDiskCounters counters = probe.read();

    EXPECT_EQ(names(counters), (std::vector<std::string>{"0 C:", "1 D: E:"}));
    EXPECT_TRUE(counters.disks.at(0).hasIoCounters);
    EXPECT_TRUE(probe.capabilities().hasReadWriteBytes);
    EXPECT_EQ(fake().enumeratedAs, (std::vector<std::wstring>(2, fake().objectName)));
    EXPECT_EQ(fake().lookups, 1);

    fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
    (void) probe.read(); // Rescans
    EXPECT_EQ(fake().enumeratedAs, (std::vector<std::wstring>(4, fake().objectName)));
    EXPECT_EQ(fake().lookups, 1) << "the name is looked up once";
}

TEST_F(WindowsDiskProbeSeamsTest, EnglishNameIsUsedWhenTheLookupFails)
{
    fake().failLookup = true;
    fake().objectName = L"PhysicalDisk"; // An English system, whose name the English one is
    WindowsDiskProbe probe(fakeFunctions());

    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:"}));
    EXPECT_EQ(fake().enumeratedAs, (std::vector<std::wstring>(2, L"PhysicalDisk")));
    EXPECT_EQ(logged("debug", "looking up perf object 234's name failed"), 1U);
}

TEST_F(WindowsDiskProbeSeamsTest, FailedLookupOnALocalizedSystemFallsBackUntilARescanFindsTheName)
{
    // Without the localized name the enumeration fails: the fixed logical drives are listed, their
    // counters marked unread rather than passed off as idle disks, and the next rescan looks again.
    fake().failLookup = true;
    fake().logicalDrives = 1U << 2U;
    fake().driveTypes = {{L'C', DRIVE_FIXED}};
    WindowsDiskProbe probe(fakeFunctions());

    const SystemDiskCounters fallback = probe.read();
    EXPECT_EQ(names(fallback), (std::vector<std::string>{"C:"}));
    EXPECT_FALSE(fallback.disks.at(0).hasIoCounters);
    EXPECT_FALSE(probe.capabilities().hasReadWriteBytes);
    EXPECT_FALSE(probe.capabilities().hasIoTime);
    EXPECT_EQ(fake().enumeratedAs, (std::vector<std::wstring>{L"PhysicalDisk"}));
    EXPECT_EQ(logged("debug", "enumerating PhysicalDisk ('PhysicalDisk') failed"), 1U);

    fake().failLookup = false;
    fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
    (void) probe.read(); // Rescans, and finds the name this time
    EXPECT_EQ(fake().lookups, 2);
    const SystemDiskCounters measured = probe.read();
    EXPECT_EQ(names(measured), (std::vector<std::string>{"0 C:", "1 D: E:"}));
    EXPECT_TRUE(measured.disks.at(0).hasIoCounters);
    EXPECT_TRUE(probe.capabilities().hasReadWriteBytes);
}

TEST_F(WindowsDiskProbeSeamsTest, LookupWantingALargerBufferIsAskedForItsSize)
{
    // PDH_MAX_COUNTER_NAME holds every name, but a lookup that asks for more anyway is asked, with a
    // size-only call, how much: the size it returns for a too-small buffer isn't reliable.
    fake().lookupNeeds = PDH_MAX_COUNTER_NAME + 64;
    WindowsDiskProbe probe(fakeFunctions());

    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:"}));
    EXPECT_EQ(fake().lookups, 3);
}

TEST_F(WindowsDiskProbeSeamsTest, DriveWhoseProbeIoctlFailsIsLeftOut)
{
    fake().failProbe = {0};
    WindowsDiskProbe probe(fakeFunctions());
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"1 D: E:"}));
    EXPECT_EQ(logged("warning", "IOCTL_DISK_PERFORMANCE probe failed"), 1U);
}

TEST_F(WindowsDiskProbeSeamsTest, FailedReadSkipsTheDiskAndWarnsOnceUntilItRecovers)
{
    WindowsDiskProbe probe(fakeFunctions());
    fake().failRead = {1};

    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:"}));
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:"}));
    EXPECT_EQ(logged("warning", "IOCTL_DISK_PERFORMANCE failed for 1 D: E:"), 1U);
    EXPECT_EQ(logged("debug", "IOCTL_DISK_PERFORMANCE failed for 1 D: E:"), 1U);
    EXPECT_EQ(fake().refreshes, 1) << "no rescan within the after-failure interval";

    fake().failRead.clear();
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:"}));

    fake().failRead = {1};
    (void) probe.read();
    EXPECT_EQ(logged("warning", "IOCTL_DISK_PERFORMANCE failed for 1 D: E:"), 2U) << "a recovered disk warns again";
}

TEST_F(WindowsDiskProbeSeamsTest, FailingDiskIsDroppedByTheNextRescan)
{
    WindowsDiskProbe probe(fakeFunctions());
    fake().failRead = {1};
    fake().instances = {L"0 C:", L"_Total"}; // Disk 1 was removed

    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 1);

    fakeClock() += 5s;
    (void) probe.read(); // Fails again, then rescans
    EXPECT_EQ(fake().refreshes, 2);

    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:"}));
    EXPECT_TRUE(probe.capabilities().hasReadWriteBytes);
}

TEST_F(WindowsDiskProbeSeamsTest, WithoutNotificationsAPeriodicRescanFindsANewDisk)
{
    // CM_Register_Notification failed: the probe rescans every 30 s, as before #3151.
    fake().failRegistration = 0;
    WindowsDiskProbe probe(fakeFunctions());
    EXPECT_TRUE(fake().registrations.empty());
    fake().instances.insert(fake().instances.begin() + 2, L"2 F:");

    fakeClock() += 29s;
    EXPECT_EQ(probe.read().disks.size(), 2U);
    fakeClock() += 1s;
    EXPECT_EQ(probe.read().disks.size(), 2U) << "the rescan runs after the read";
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:", "2 F:"}));
}

TEST_F(WindowsDiskProbeSeamsTest, RescanWhoseEnumerationFailsKeepsTheCurrentDisks)
{
    WindowsDiskProbe probe(fakeFunctions());
    for (int failing = 0; failing < 2; ++failing)
    {
        fake().failRefresh = failing == 0;
        fake().failItems = failing == 1;
        fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
        (void) probe.read();
        EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:"})) << failing;
    }
}

TEST_F(WindowsDiskProbeSeamsTest, OpenFailureAndUnparseableNameWarnOnceAcrossRescans)
{
    fake().failOpen = {1};
    fake().instances.insert(fake().instances.begin(), L"bogus");
    WindowsDiskProbe probe(fakeFunctions());

    fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
    (void) probe.read(); // Rescans
    EXPECT_EQ(fake().refreshes, 2);
    EXPECT_EQ(logged("warning", "CreateFileW failed for \\\\.\\PhysicalDrive1"), 1U);
    EXPECT_EQ(logged("debug", "CreateFileW failed for \\\\.\\PhysicalDrive1"), 1U);
    EXPECT_EQ(logged("warning", "'bogus'"), 1U);
    EXPECT_EQ(logged("debug", "'bogus'"), 1U);

    fake().failOpen.clear();
    fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
    (void) probe.read();
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:"}));

    fake().failOpen = {1};
    fakeClock() += DISK_REENUMERATE_SAFETY_INTERVAL;
    (void) probe.read();
    EXPECT_EQ(logged("warning", "CreateFileW failed for \\\\.\\PhysicalDrive1"), 2U) << "an opened drive warns again";
}

TEST_F(WindowsDiskProbeSeamsTest, BusyTimeFollowsIdleTimeAndSurvivesACounterReset)
{
    DISK_PERFORMANCE perf = defaultPerf(0);
    perf.IdleTime.QuadPart = 10'000'000; // 1 s
    fake().perf[0] = perf;
    fake().failOpen = {1};
    WindowsDiskProbe probe(fakeFunctions());

    EXPECT_EQ(probe.read().disks.at(0).ioTimeMs, 0U) << "the first read is the baseline";

    fakeClock() += 1s;
    fake().perf[0].IdleTime.QuadPart += 2'500'000; // Idle 250 ms of the second
    EXPECT_EQ(probe.read().disks.at(0).ioTimeMs, 750U);

    fakeClock() += 1s;
    fake().perf[0].IdleTime.QuadPart = 1'000; // The counters were reset
    EXPECT_EQ(probe.read().disks.at(0).ioTimeMs, 750U) << "a reset starts a new baseline, never going back";

    fakeClock() += 1s;
    fake().perf[0].IdleTime.QuadPart += 5'000'000; // Idle 500 ms
    EXPECT_EQ(probe.read().disks.at(0).ioTimeMs, 1'250U);
}

TEST_F(WindowsDiskProbeSeamsTest, NegativeCountersReadAsZero)
{
    DISK_PERFORMANCE perf = defaultPerf(0);
    perf.BytesRead.QuadPart = -1;
    perf.ReadTime.QuadPart = -10'000;
    fake().perf[0] = perf;
    WindowsDiskProbe probe(fakeFunctions());
    const DiskCounters disk = probe.read().disks.at(0);

    EXPECT_EQ(disk.readSectors, 0U);
    EXPECT_EQ(disk.readTimeMs, 0U);
    EXPECT_EQ(disk.ioTimeMs, 3U);
}

// =============================================================================
// Device notifications (#3151): a rescan refreshes PDH's whole object list (60-130 ms of CPU), so it
// runs when a disk or volume arrives or leaves, not every 30 s.
// =============================================================================

TEST_F(WindowsDiskProbeSeamsTest, RegistersForDiskAndVolumeArrivalsAndUnregistersOnDestruction)
{
    {
        const WindowsDiskProbe probe(fakeFunctions());
        ASSERT_EQ(fake().registrations.size(), 2U);
        EXPECT_TRUE(fake().registrations[0].interfaceClass == DISK_INTERFACE);
        EXPECT_TRUE(fake().registrations[1].interfaceClass == VOLUME_INTERFACE);
        EXPECT_NE(fake().registrations[0].context, nullptr);
        EXPECT_TRUE(fake().unregistered.empty());
    }
    // Both, newest first: CM_Unregister_Notification waits for a callback still running, so none can
    // touch the probe once it is gone.
    EXPECT_EQ(fake().unregistered, (std::vector<HCMNOTIFICATION>{fake().registrations[1].handle, fake().registrations[0].handle}));
}

TEST_F(WindowsDiskProbeSeamsTest, WithoutANotificationNoRescanRunsUntilTheSafetyInterval)
{
    WindowsDiskProbe probe(fakeFunctions());
    fake().instances.insert(fake().instances.begin() + 2, L"2 F:"); // Attached with no notification
    ASSERT_EQ(fake().refreshes, 1);

    // Every second for the whole safety interval but its last second: no rescan.
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(DISK_REENUMERATE_SAFETY_INTERVAL).count();
    for (long long second = 1; second < seconds; ++second)
    {
        fakeClock() += 1s;
        ASSERT_EQ(probe.read().disks.size(), 2U) << second;
    }
    EXPECT_EQ(fake().refreshes, 1) << "no 30 s rescan while the notifications are registered";

    fakeClock() += 1s;
    (void) probe.read(); // The safety rescan, after the read
    EXPECT_EQ(fake().refreshes, 2);
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:", "2 F:"}));
}

TEST_F(WindowsDiskProbeSeamsTest, ADiskArrivalRescansOnceItsVolumesHaveSettled)
{
    WindowsDiskProbe probe(fakeFunctions());
    fakeClock() += 1min;
    (void) probe.read();
    ASSERT_EQ(fake().refreshes, 1);

    fake().instances.insert(fake().instances.begin() + 2, L"2 F:");
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL);
    fakeClock() += 1s;
    EXPECT_EQ(probe.read().disks.size(), 2U);
    EXPECT_EQ(fake().refreshes, 1) << "the rescan waits for the disk's volumes to be lettered";

    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY - 1ms;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 1);

    fakeClock() += 1ms;
    EXPECT_EQ(probe.read().disks.size(), 2U) << "the rescan runs after the read";
    EXPECT_EQ(fake().refreshes, 2);
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D: E:", "2 F:"}));

    fakeClock() += 1min;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 2) << "one notification, one rescan";
}

TEST_F(WindowsDiskProbeSeamsTest, ABurstOfNotificationsIsOneRescan)
{
    // A disk, then each of its volumes: one rescan, timed from the first.
    WindowsDiskProbe probe(fakeFunctions());
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL);
    (void) probe.read();
    fakeClock() += 1s;
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, VOLUME_INTERFACE);
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, VOLUME_INTERFACE);
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 1);

    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY - 1s; // The settle delay since the first
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 2);
    fakeClock() += 1min;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 2);
}

TEST_F(WindowsDiskProbeSeamsTest, RemovalAndVolumeChangesRescanButOtherActionsDoNot)
{
    WindowsDiskProbe probe(fakeFunctions());
    notify(CM_NOTIFY_ACTION_DEVICEQUERYREMOVE); // Not an interface arriving or leaving
    (void) probe.read();
    fakeClock() += 1min;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 1);

    fake().instances = {L"0 C:", L"_Total"}; // Disk 1 left cleanly
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL);
    (void) probe.read();
    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 2);
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:"}));

    fake().instances = {L"0 C:", L"1 D:", L"_Total"}; // Disk 1 is back, and a volume on it is lettered
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, VOLUME_INTERFACE);
    (void) probe.read();
    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 3);
    EXPECT_EQ(names(probe.read()), (std::vector<std::string>{"0 C:", "1 D:"}));
}

TEST_F(WindowsDiskProbeSeamsTest, ANotificationDuringARescanAsksForAnother)
{
    // The flag is taken before the rescan, so a disk arriving while PDH enumerates isn't lost.
    WindowsDiskProbe probe(fakeFunctions());
    notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL);
    (void) probe.read();
    fake().notifyDuringRefresh = true;
    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY;
    (void) probe.read();
    ASSERT_EQ(fake().refreshes, 2);

    (void) probe.read(); // Takes the change made during that rescan
    EXPECT_EQ(fake().refreshes, 2);
    fakeClock() += DISK_DEVICE_CHANGE_SETTLE_DELAY;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 3);
}

TEST_F(WindowsDiskProbeSeamsTest, AFailedRegistrationUnregistersTheOtherAndRescansEvery30Seconds)
{
    fake().failRegistration = 1; // The disks' succeeds, the volumes' fails
    WindowsDiskProbe probe(fakeFunctions());
    ASSERT_EQ(fake().registrations.size(), 1U);
    EXPECT_EQ(fake().unregistered, (std::vector<HCMNOTIFICATION>{fake().registrations[0].handle})) << "both or neither";
    EXPECT_EQ(logged("info", "CM_Register_Notification failed"), 1U);

    fakeClock() += DISK_REENUMERATE_INTERVAL - 1s;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 1);
    fakeClock() += 1s;
    (void) probe.read();
    EXPECT_EQ(fake().refreshes, 2);
}

TEST_F(WindowsDiskProbeSeamsTest, SystemFunctionsAreAllSet)
{
    const WindowsDiskProbeFunctions fns = WindowsDiskProbe::systemFunctions();
    EXPECT_NE(fns.pdh.lookupPerfNameByIndexW, nullptr);
    EXPECT_NE(fns.pdh.enumObjectsW, nullptr);
    EXPECT_NE(fns.pdh.enumObjectItemsW, nullptr);
    EXPECT_NE(fns.drive.openPhysicalDriveQueryOnly, nullptr);
    EXPECT_NE(fns.drive.deviceIoControl, nullptr);
    EXPECT_NE(fns.drive.getLogicalDrives, nullptr);
    EXPECT_NE(fns.drive.getDriveTypeW, nullptr);
    EXPECT_NE(fns.devices.registerNotification, nullptr);
    EXPECT_NE(fns.devices.unregisterNotification, nullptr);
    EXPECT_NE(fns.now, nullptr);
}

} // namespace Platform
// NOLINTEND(misc-include-cleaner,readability-non-const-parameter)
