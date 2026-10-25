/// @file test_DevicesInfo.cpp
/// @brief The Devices readers' pure parts (#1520): the pci.ids / usb.ids parser (Platform/HwIdsParser.h),
/// and the Linux reader (Platform/Linux/LinuxDevices.h): the ALSA cards and PCM parsers, USB speeds, and
/// PCI, USB and audio under a fixture root. Standard library only, so it runs everywhere.

#include "Platform/HwIdsParser.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxDevices.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

constexpr std::string_view PCI_IDS = "# pci.ids excerpt\n"
                                     "8086  Intel Corporation\n"
                                     "\t46a6  Alder Lake-P GT2 [Iris Xe Graphics]\n"
                                     "\t\t1028 0b19  Iris Xe Graphics\n"
                                     "\t51f0  Alder Lake-P PCH CNVi WiFi\r\n"
                                     "10de  NVIDIA Corporation\n"
                                     "\t2786  AD104 [GeForce RTX 4070]\n"
                                     "\n"
                                     "C 03  Display controller\n"
                                     "\t00  VGA compatible controller\n"
                                     "\t\t00  VGA controller\n"
                                     "C 06  Bridge\n"
                                     "C 0d  Wireless controller\n";

TEST(HwIdsParserTest, KeepsOnlyWhatIsAskedFor)
{
    const HwIds::IdNames names = HwIds::parseIds(PCI_IDS, {0x8086}, {HwIds::deviceKey(0x8086, 0x51F0), HwIds::deviceKey(0x10DE, 0x2786)});
    ASSERT_EQ(names.vendors.size(), 1U); // NVIDIA's vendor wasn't asked for
    EXPECT_EQ(names.vendors.at(0x8086), "Intel Corporation");
    ASSERT_EQ(names.devices.size(), 2U);
    EXPECT_EQ(names.devices.at(HwIds::deviceKey(0x8086, 0x51F0)), "Alder Lake-P PCH CNVi WiFi"); // the CR is dropped
    EXPECT_EQ(names.devices.at(HwIds::deviceKey(0x10DE, 0x2786)), "AD104 [GeForce RTX 4070]");
    EXPECT_EQ(names.className(0x03, 0x00), "VGA compatible controller");
    EXPECT_EQ(names.className(0x03, 0x80), "Display controller"); // an unknown subclass falls back to the class
    EXPECT_EQ(names.className(0x0D, 0x11), "Wireless controller");
    EXPECT_EQ(names.className(0x42, 0x00), "");
}

TEST(HwIdsParserTest, SkipsMalformedLinesAndOtherSections)
{
    // usb.ids' own sections after the vendors: their indented lines aren't devices of the last vendor.
    constexpr std::string_view USB_IDS = "046d  Logitech, Inc.\n"
                                         "\tc52b  Unifying Receiver\n"
                                         "HID 22  Unknown\n"
                                         "\tc52b  Not a Logitech device\n"
                                         "zzzz  Not hex\n"
                                         "\tc52c  Under a bad vendor\n"
                                         "1234 One space\n"
                                         "abcd  \n"
                                         "\t";
    const HwIds::IdNames names =
        HwIds::parseIds(USB_IDS, {0x046D, 0x1234, 0xABCD}, {HwIds::deviceKey(0x046D, 0xC52B), HwIds::deviceKey(0x046D, 0xC52C)});
    ASSERT_EQ(names.vendors.size(), 1U);
    ASSERT_EQ(names.devices.size(), 1U);
    EXPECT_EQ(names.devices.at(HwIds::deviceKey(0x046D, 0xC52B)), "Unifying Receiver");
    EXPECT_TRUE(HwIds::parseIds("", {1}, {1}).vendors.empty());
}

TEST(LinuxDevicesTest, ParsesAlsaCardsAndPcms)
{
    constexpr std::string_view CARDS = " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n"
                                       "                      HDA Intel PCH at 0xf7f10000 irq 32\n"
                                       " 1 [NVidia         ]: HDA-Intel - HDA NVidia\n"
                                       "                      HDA NVidia at 0xf7080000 irq 17\n";
    const std::map<int, std::string> cards = LinuxDevices::parseAsoundCards(CARDS);
    ASSERT_EQ(cards.size(), 2U);
    EXPECT_EQ(cards.at(0), "HDA Intel PCH");
    EXPECT_EQ(cards.at(1), "HDA NVidia");
    EXPECT_TRUE(LinuxDevices::parseAsoundCards("--- no soundcards ---\n").empty());

    constexpr std::string_view PCM = "00-00: ALC892 Analog : ALC892 Analog : playback 1 : capture 1\n"
                                     "00-02: ALC892 Alt Analog : ALC892 Alt Analog : capture 1\n"
                                     "01-03: HDMI 0 : HDMI 0 : playback 1\n"
                                     "02-00: Orphan : Orphan : playback 1\n"
                                     "garbage\n";
    const std::vector<AudioEndpoint> endpoints = LinuxDevices::parseAsoundPcm(PCM, cards);
    ASSERT_EQ(endpoints.size(), 5U);
    EXPECT_EQ(endpoints[0].name, "HDA Intel PCH: ALC892 Analog");
    EXPECT_EQ(endpoints[0].flow, AudioFlow::Output);
    EXPECT_EQ(endpoints[1].flow, AudioFlow::Input);
    EXPECT_EQ(endpoints[2].name, "HDA Intel PCH: ALC892 Alt Analog");
    EXPECT_EQ(endpoints[2].flow, AudioFlow::Input);
    EXPECT_EQ(endpoints[3].name, "HDA NVidia: HDMI 0");
    EXPECT_EQ(endpoints[4].name, "Card 2: Orphan");
}

TEST(LinuxDevicesTest, ParsesIdsAndSpeeds)
{
    EXPECT_EQ(LinuxDevices::hexValue("0x8086\n"), 0x8086U);
    EXPECT_EQ(LinuxDevices::hexValue("046d"), 0x046DU);
    EXPECT_EQ(LinuxDevices::hexValue("0x030000"), 0x030000U);
    EXPECT_EQ(LinuxDevices::hexValue("nope"), 0U);
    EXPECT_DOUBLE_EQ(LinuxDevices::usbSpeedMbps("1.5"), 1.5);
    EXPECT_DOUBLE_EQ(LinuxDevices::usbSpeedMbps("480"), 480.0);
    EXPECT_DOUBLE_EQ(LinuxDevices::usbSpeedMbps("5000"), 5000.0);
    EXPECT_DOUBLE_EQ(LinuxDevices::usbSpeedMbps("unknown"), 0.0);
}

/// A fresh fixture root under the temp directory, removed (best effort) at the end.
class DevicesFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned> counter{0};
        m_Root = std::filesystem::temp_directory_path() /
                 std::format("ts_devices_{}_{}", counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count());
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

    void
    pciDevice(std::string_view dir, std::string_view vendor, std::string_view device, std::string_view cls, std::string_view uevent) const
    {
        const std::string base = std::format("sys/bus/pci/devices/{}/", dir); // real names have ':', which Windows can't
        write(base + "vendor", vendor);
        write(base + "device", device);
        write(base + "class", cls);
        write(base + "uevent", uevent);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return m_Root;
    }

  private:
    std::filesystem::path m_Root;
};

TEST_F(DevicesFixture, ReadsPciUsbAndAudio)
{
    pciDevice("a", "0x8086\n", "0x46a6\n", "0x030000\n", "DRIVER=i915\nPCI_SLOT_NAME=0000:00:02.0\n");
    pciDevice("b", "0x8086\n", "0x51f0\n", "0x028000\n", "PCI_SLOT_NAME=0000:00:14.3\n"); // no driver: a problem
    pciDevice("c", "0x8086\n", "0x4621\n", "0x060000\n", "PCI_SLOT_NAME=0000:00:00.0\n"); // a host bridge: not
    pciDevice("d", "0x1b21\n", "0x2142\n", "0x0c0330\n", "DRIVER=xhci_hcd\n");            // no ids for it, no slot
    write("usr/share/hwdata/pci.ids", PCI_IDS);

    write("sys/bus/usb/devices/usb1/idVendor", "1d6b\n"); // the root hub: left out
    write("sys/bus/usb/devices/1-1/idVendor", "05e3\n");
    write("sys/bus/usb/devices/1-1/idProduct", "0610\n");
    write("sys/bus/usb/devices/1-1/speed", "480\n");
    write("sys/bus/usb/devices/1-1.2/idVendor", "046d\n");
    write("sys/bus/usb/devices/1-1.2/idProduct", "c52b\n");
    write("sys/bus/usb/devices/1-1.2/manufacturer", "Logitech\n");
    write("sys/bus/usb/devices/1-1.2/product", "USB Receiver\n");
    write("sys/bus/usb/devices/1-1.2/serial", "ABC123\n");
    write("sys/bus/usb/devices/1-1.2/speed", "12\n");
#ifndef _WIN32                                                      // a ':' can't be in a Windows file name
    write("sys/bus/usb/devices/1-1.2:1.0/bInterfaceClass", "03\n"); // an interface: left out
#endif
    write("usr/share/misc/usb.ids", "05e3  Genesys Logic, Inc.\n\t0610  Hub\n046d  Logitech, Inc.\n\tc52b  Unifying Receiver\n");

    write("proc/asound/cards", " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n");
    write("proc/asound/pcm", "00-00: ALC892 Analog : ALC892 Analog : playback 1 : capture 1\n");

    DevicesInfo info;
    LinuxDevices::readDeviceFacts(root(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    ASSERT_TRUE(info.pciRead);
    ASSERT_EQ(info.pci.size(), 4U);
    EXPECT_EQ(info.pci[0].vendor, "Intel Corporation");
    EXPECT_EQ(info.pci[0].name, "Alder Lake-P GT2 [Iris Xe Graphics]");
    EXPECT_EQ(info.pci[0].className, "VGA compatible controller");
    EXPECT_EQ(info.pci[0].location, "0000:00:02.0");
    EXPECT_EQ(info.pci[0].driver, "i915");
    EXPECT_TRUE(info.pci[0].problem.empty());
    EXPECT_EQ(info.pci[1].problem, "No driver");
    EXPECT_EQ(info.pci[1].className, "Class 0280");
    EXPECT_TRUE(info.pci[2].problem.empty());
    EXPECT_EQ(info.pci[2].className, "Bridge");
    EXPECT_EQ(info.pci[3].vendorId, 0x1B21U);
    EXPECT_TRUE(info.pci[3].vendor.empty());
    EXPECT_EQ(info.pci[3].location, "d"); // no PCI_SLOT_NAME: the directory's name
    ASSERT_TRUE(info.problemsRead);
    ASSERT_EQ(info.problems.size(), 1U);
    EXPECT_EQ(info.problems[0].location, "0000:00:14.3");

    ASSERT_TRUE(info.usbRead);
    ASSERT_EQ(info.usb.size(), 2U); // the root hub left out
    EXPECT_EQ(info.usb[0].name, "Hub");
    EXPECT_EQ(info.usb[0].vendor, "Genesys Logic, Inc.");
    EXPECT_EQ(info.usb[0].depth, 1U);
    EXPECT_FALSE(info.usb[0].parent.has_value());
    EXPECT_DOUBLE_EQ(info.usb[0].speedMbps, 480.0);
    EXPECT_EQ(info.usb[1].name, "USB Receiver"); // its own strings beat usb.ids
    EXPECT_EQ(info.usb[1].vendor, "Logitech");
    EXPECT_EQ(info.usb[1].serial, "ABC123");
    EXPECT_EQ(info.usb[1].depth, 2U);
    EXPECT_EQ(info.usb[1].parent, std::optional<std::size_t>(0));

    ASSERT_TRUE(info.audioRead);
    ASSERT_EQ(info.audio.size(), 2U);
    EXPECT_EQ(info.audio[0].flow, AudioFlow::Output);
    EXPECT_EQ(info.audio[1].flow, AudioFlow::Input);
}

TEST_F(DevicesFixture, MissingTreesAndIdsFiles)
{
    pciDevice("a", "0x8086\n", "0x46a6\n", "0x030000\n", "DRIVER=i915\n");
    write("proc/asound/cards", " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n"); // no pcm list

    DevicesInfo info;
    LinuxDevices::readDeviceFacts(root(), info);
    ASSERT_EQ(info.pci.size(), 1U);
    EXPECT_TRUE(info.pci[0].vendor.empty()); // no pci.ids: the section shows the ids
    EXPECT_TRUE(info.pci[0].name.empty());
    EXPECT_EQ(info.pci[0].className, "Class 0300");
    EXPECT_FALSE(info.usbRead);
    ASSERT_EQ(info.audio.size(), 1U);
    EXPECT_EQ(info.audio[0].name, "HDA Intel PCH");
    EXPECT_EQ(info.audio[0].flow, AudioFlow::Unknown);

    DevicesInfo none;
    LinuxDevices::readDeviceFacts(root() / "nothing", none);
    EXPECT_TRUE(none.available);
    EXPECT_FALSE(none.pciRead);
    EXPECT_FALSE(none.problemsRead);
    EXPECT_FALSE(none.audioRead);
}

} // namespace
} // namespace Platform
