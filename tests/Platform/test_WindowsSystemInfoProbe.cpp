/// @file test_WindowsSystemInfoProbe.cpp
/// @brief The real WindowsSystemInfoProbe (#1512), smoke-tested: it reports an edition and a build,
/// and the session facts any signed-in test run has; and the real SMBIOS table (#1513) parses.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/SmbiosParser.h"
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
// clang-format on

#include <chrono>
#include <cstdint>
#include <vector>

namespace Platform
{
namespace
{

TEST(WindowsSystemInfoProbeTest, ReadsAnEditionAndABuild)
{
    WindowsSystemInfoProbe probe;
    ASSERT_TRUE(probe.capabilities().hasOs);

    const OsInfo info = probe.readOs();
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.name.starts_with("Windows")) << info.name;
    EXPECT_FALSE(info.build.empty());
    EXPECT_NE(info.build.find('.'), std::string::npos) << info.build; // CurrentBuild.UBR
    EXPECT_FALSE(info.architecture.empty());
    EXPECT_FALSE(info.computerName.empty());
    EXPECT_FALSE(info.locale.empty());
    EXPECT_FALSE(info.timeZone.empty());
    EXPECT_TRUE(info.utcOffsetMinutes.has_value());
    EXPECT_FALSE(info.systemDirectory.empty());
    EXPECT_FALSE(info.windowsDirectory.empty());

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    EXPECT_GT(info.bootUnixSeconds, 0U);
    EXPECT_LE(info.bootUnixSeconds, now);
}

TEST(WindowsSystemInfoProbeTest, RealSmbiosTableParses)
{
    constexpr DWORD RSMB = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(RSMB, 0, nullptr, 0);
    if (size == 0)
    {
        GTEST_SKIP() << "GetSystemFirmwareTable('RSMB') failed: " << GetLastError();
    }
    std::vector<std::uint8_t> raw(size);
    const UINT written = GetSystemFirmwareTable(RSMB, 0, raw.data(), size);
    if (written == 0 || written > size)
    {
        GTEST_SKIP() << "GetSystemFirmwareTable('RSMB') failed on the second call";
    }
    raw.resize(written);

    const auto parsed = Smbios::parseRawSmbiosData(raw);
    ASSERT_TRUE(parsed.has_value());
    const Smbios::Table table = parsed.value_or(Smbios::Table{});
    EXPECT_GE(table.majorVersion, 2);
    const auto structures = Smbios::parseStructures(table.data);
    ASSERT_NE(Smbios::findFirst(structures, Smbios::TYPE_SYSTEM), nullptr);
    EXPECT_FALSE(Smbios::decodeSystem(*Smbios::findFirst(structures, Smbios::TYPE_SYSTEM), table).manufacturer.empty());

    WindowsSystemInfoProbe probe;
    const FirmwareInfo info = probe.readFirmware();
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.systemManufacturer.empty());
    EXPECT_FALSE(info.biosVersion.empty());
    EXPECT_FALSE(info.smbiosVersion.empty());
    EXPECT_NE(info.firmwareMode, FirmwareMode::Unknown);
}

} // namespace
} // namespace Platform
