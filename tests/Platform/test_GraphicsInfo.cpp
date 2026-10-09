/// @file test_GraphicsInfo.cpp
/// @brief The Graphics & displays readers' pure parts (#1519): the EDID parser (Platform/EdidParser.h) on
/// real-looking blocks, checksum and header failures and missing descriptors; and the Linux reader
/// (Platform/Linux/LinuxGraphics.h): the display server choice, uevent and NVIDIA model parsing, and the
/// adapters and monitors under a fixture /sys/class/drm. Standard library only, so it runs everywhere.

#include "EdidTestData.h"
#include "Platform/EdidParser.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxGraphics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

using TestSupport::makeEdid;

TEST(EdidParserTest, ParsesARealLookingBlock)
{
    const auto parsed = Edid::parseEdid(makeEdid());
    ASSERT_TRUE(parsed.has_value());
    const Edid::EdidInfo info = parsed.value_or(Edid::EdidInfo{});
    EXPECT_EQ(info.manufacturerId, "DEL");
    EXPECT_EQ(info.productCode, 0x41A8);
    EXPECT_EQ(info.name, "DELL U2720Q");
    EXPECT_EQ(info.displayName(), "DELL U2720Q");
    EXPECT_EQ(info.serial, "ABC1234");
    EXPECT_EQ(info.serialText(), "ABC1234");
    EXPECT_EQ(info.widthMm, 597U); // the detailed timing's mm, not the 60 x 34 cm
    EXPECT_EQ(info.heightMm, 336U);

    std::vector<std::uint8_t> withExtension = makeEdid();
    withExtension.resize(256, 0x5A); // an extension block that isn't read
    EXPECT_TRUE(Edid::parseEdid(withExtension).has_value());
}

TEST(EdidParserTest, MissingDescriptorsFallBack)
{
    const auto parsed = Edid::parseEdid(makeEdid("", "", false));
    ASSERT_TRUE(parsed.has_value());
    const Edid::EdidInfo info = parsed.value_or(Edid::EdidInfo{});
    EXPECT_TRUE(info.name.empty());
    EXPECT_EQ(info.displayName(), "DEL 41A8");
    EXPECT_EQ(info.serialText(), "67305985"); // the numeric serial 0x04030201
    EXPECT_EQ(info.widthMm, 600U);            // the cm size
    EXPECT_EQ(info.heightMm, 340U);
}

TEST(EdidParserTest, RejectsCorruptBlocks)
{
    std::vector<std::uint8_t> edid = makeEdid();
    edid[20] ^= 0x01U; // the checksum no longer adds up
    EXPECT_FALSE(Edid::parseEdid(edid).has_value());

    std::vector<std::uint8_t> header = makeEdid();
    header[0] = 0x01;
    header[127] = static_cast<std::uint8_t>(header[127] - 1); // checksum still fine, header wrong
    EXPECT_FALSE(Edid::parseEdid(header).has_value());

    std::vector<std::uint8_t> shortBlock = makeEdid();
    shortBlock.pop_back();
    EXPECT_FALSE(Edid::parseEdid(shortBlock).has_value());
    EXPECT_FALSE(Edid::parseEdid({}).has_value());
}

TEST(EdidParserTest, DescriptorTextStopsAtLineFeedAndControlBytes)
{
    const std::vector<std::uint8_t> text{'A', 'B', ' ', 0x0A, 'C'};
    EXPECT_EQ(Edid::descriptorText(text), "AB");
    const std::vector<std::uint8_t> control{'X', 0x01, 'Y'};
    EXPECT_EQ(Edid::descriptorText(control), "X");
}

TEST(LinuxGraphicsTest, DisplayServer)
{
    EXPECT_EQ(LinuxGraphics::displayServer("wayland", "wayland-0", ":0"), "Wayland");
    EXPECT_EQ(LinuxGraphics::displayServer("x11", "", ":0"), "X11");
    EXPECT_EQ(LinuxGraphics::displayServer("", "wayland-1", ""), "Wayland");
    EXPECT_EQ(LinuxGraphics::displayServer("", "", ":1"), "X11");
    EXPECT_EQ(LinuxGraphics::displayServer("tty", "", ""), "");
    EXPECT_EQ(LinuxGraphics::displayServer("", "", ""), "");
    EXPECT_EQ(LinuxGraphics::displayServer("mir", "", ""), "mir");
}

TEST(LinuxGraphicsTest, ParsesUeventAndNvidiaInformation)
{
    constexpr std::string_view UEVENT = "DRIVER=amdgpu\nPCI_ID=1002:73BF\nPCI_SLOT_NAME=0000:03:00.0\n";
    EXPECT_EQ(LinuxGraphics::ueventValue(UEVENT, "DRIVER"), "amdgpu");
    EXPECT_EQ(LinuxGraphics::ueventValue(UEVENT, "PCI_SLOT_NAME"), "0000:03:00.0");
    EXPECT_EQ(LinuxGraphics::ueventValue(UEVENT, "PCI"), "");
    EXPECT_EQ(LinuxGraphics::ueventValue("", "DRIVER"), "");
    EXPECT_EQ(LinuxGraphics::nvidiaModel("Model: \t\t NVIDIA GeForce RTX 4070\nIRQ:   140\n"), "NVIDIA GeForce RTX 4070");
    EXPECT_EQ(LinuxGraphics::nvidiaModel("Model:"), "");
    EXPECT_EQ(LinuxGraphics::nvidiaModel("IRQ: 1"), "");
}

/// A fresh fixture root under the temp directory, removed (best effort) at the end.
class DrmFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned> counter{0};
        m_Root = std::filesystem::temp_directory_path() /
                 std::format("ts_graphics_{}_{}", counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    void write(std::string_view relative, std::string_view text) const
    {
        const auto file = m_Root / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << text;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return m_Root;
    }

  private:
    std::filesystem::path m_Root;
};

TEST_F(DrmFixture, ReadsAdaptersAndConnectedMonitors)
{
    write("sys/class/drm/card0/device/uevent", "DRIVER=amdgpu\nPCI_ID=1002:73BF\nPCI_SLOT_NAME=0000:03:00.0\n");
    write("sys/class/drm/card0/device/product_name", "AMD Radeon RX 6800 XT\n");
    write("sys/class/drm/card0/device/mem_info_vram_total", "17163091968\n");
    write("sys/class/drm/card1/device/uevent", "DRIVER=nvidia\nPCI_ID=10DE:2786\nPCI_SLOT_NAME=0000:01:00.0\n");
    write("sys/module/nvidia/version", "560.35.03\n");
#ifndef _WIN32 // a ':' can't be in a Windows file name
    write("proc/driver/nvidia/gpus/0000:01:00.0/information", "Model: \t\t NVIDIA GeForce RTX 4070\n");
    constexpr std::string_view NVIDIA_NAME = "NVIDIA GeForce RTX 4070";
#else
    constexpr std::string_view NVIDIA_NAME = "NVIDIA GPU (10DE:2786)";
#endif
    write("sys/class/drm/card2/device/uevent", "DRIVER=i915\nPCI_ID=8086:A7A0\nPCI_SLOT_NAME=0000:00:02.0\n");
    const std::vector<std::uint8_t> edid = makeEdid();
    write("sys/class/drm/card0-DP-1/status", "connected\n");
    write("sys/class/drm/card0-DP-1/edid", std::string(edid.begin(), edid.end()));
    write("sys/class/drm/card0-HDMI-A-1/status", "disconnected\n");
    write("sys/class/drm/card2-eDP-1/status", "connected\n");
    write("sys/class/drm/card2-eDP-1/edid", ""); // no EDID read
    write("sys/class/drm/renderD128/dev", "226:128\n");
    write("sys/class/drm/version", "drm 1.1.0 20060810\n");

    GraphicsInfo info;
    LinuxGraphics::readGraphicsFacts(root(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    ASSERT_TRUE(info.adaptersRead);
    ASSERT_EQ(info.adapters.size(), 3U);
    EXPECT_EQ(info.adapters[0].name, "AMD Radeon RX 6800 XT");
    EXPECT_EQ(info.adapters[0].driver, "amdgpu");
    EXPECT_EQ(info.adapters[0].driverVersion, ""); // in-kernel: no module version
    EXPECT_EQ(info.adapters[0].location, "0000:03:00.0");
    EXPECT_EQ(info.adapters[0].vendorId, 0x1002U);
    EXPECT_EQ(info.adapters[0].deviceId, 0x73BFU);
    EXPECT_EQ(info.adapters[0].dedicatedBytes, 17'163'091'968U);
    EXPECT_EQ(info.adapters[1].name, NVIDIA_NAME);
    EXPECT_EQ(info.adapters[1].driverVersion, "560.35.03");
    EXPECT_EQ(info.adapters[2].name, "Intel GPU (8086:A7A0)");

    ASSERT_TRUE(info.monitorsRead);
    ASSERT_EQ(info.monitors.size(), 2U); // the disconnected HDMI port is left out
    EXPECT_EQ(info.monitors[0].connector, "DP-1");
    EXPECT_EQ(info.monitors[0].name, "DELL U2720Q");
    EXPECT_EQ(info.monitors[0].serial, "ABC1234");
    EXPECT_EQ(info.monitors[0].widthMm, 597U);
    EXPECT_FALSE(info.monitors[0].hasDesktopRect);
    EXPECT_EQ(info.monitors[1].connector, "eDP-1");
    EXPECT_TRUE(info.monitors[1].name.empty());
}

TEST_F(DrmFixture, MissingDrmIsUnread)
{
    GraphicsInfo info;
    LinuxGraphics::readGraphicsFacts(root() / "nothing", info);
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.adaptersRead);
    EXPECT_FALSE(info.monitorsRead);
    EXPECT_TRUE(info.adapters.empty());
}

} // namespace
} // namespace Platform
