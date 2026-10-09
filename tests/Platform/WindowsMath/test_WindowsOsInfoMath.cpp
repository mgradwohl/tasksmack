/// @file test_WindowsOsInfoMath.cpp
/// @brief WindowsOsInfoMath.h (#1512): the "Windows 11" fix-up of ProductName, the display version
/// and build strings, and the architecture names. Header-only, so these run on every platform.

#include "Platform/Windows/WindowsOsInfoMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace Platform::WindowsOsInfo
{
namespace
{

TEST(WindowsOsInfoMathTest, ProductNameSaysElevenFromBuild22000)
{
    // ProductName still says "Windows 10" on Windows 11.
    EXPECT_EQ(productName("Windows 10 Home", "Core", 26100), "Windows 11 Home");
    EXPECT_EQ(productName("Windows 10 Pro", "Professional", WINDOWS_11_FIRST_BUILD), "Windows 11 Pro");
    EXPECT_EQ(productName("Windows 10 Pro", "Professional", 19045), "Windows 10 Pro");
    EXPECT_EQ(productName("Windows 10 Pro", "Professional", std::nullopt), "Windows 10 Pro"); // build unknown: as written
    EXPECT_EQ(productName("Windows Server 2022 Datacenter", "ServerDatacenter", 20348), "Windows Server 2022 Datacenter");
    EXPECT_EQ(productName("Windows Server 2025 Standard", "ServerStandard", 26100), "Windows Server 2025 Standard");
    // Without a ProductName, from EditionID.
    EXPECT_EQ(productName("", "Professional", 26100), "Windows 11 Professional");
    EXPECT_EQ(productName("", "Core", 19045), "Windows 10 Core");
    EXPECT_EQ(productName("", "", 26100), "");
}

TEST(WindowsOsInfoMathTest, DisplayVersionFallsBackToReleaseId)
{
    EXPECT_EQ(displayVersion("25H2", "2009"), "25H2");
    EXPECT_EQ(displayVersion("", "2004"), "2004");
    EXPECT_EQ(displayVersion("", ""), "");
}

TEST(WindowsOsInfoMathTest, BuildAndUbr)
{
    EXPECT_EQ(parseBuild("26100"), std::optional<std::uint32_t>{26100});
    EXPECT_FALSE(parseBuild("").has_value());
    EXPECT_FALSE(parseBuild("26100a").has_value());
    EXPECT_EQ(buildString("26100", 4652), "26100.4652");
    EXPECT_EQ(buildString("26100", std::nullopt), "26100");
    EXPECT_EQ(buildString("", 4652), "");
}

TEST(WindowsOsInfoMathTest, MachineNames)
{
    EXPECT_EQ(machineName(0x8664), "x64");
    EXPECT_EQ(machineName(0xAA64), "ARM64");
    EXPECT_EQ(machineName(0x014C), "x86");
    EXPECT_EQ(machineName(0), "");
}

TEST(WindowsOsInfoMathTest, PlatformRoleNames)
{
    EXPECT_EQ(platformRoleName(0), ""); // PlatformRoleUnspecified
    EXPECT_EQ(platformRoleName(1), "Desktop");
    EXPECT_EQ(platformRoleName(2), "Mobile");
    EXPECT_EQ(platformRoleName(4), "Enterprise server");
    EXPECT_EQ(platformRoleName(8), "Slate");
    EXPECT_EQ(platformRoleName(9), "");
}

} // namespace
} // namespace Platform::WindowsOsInfo
