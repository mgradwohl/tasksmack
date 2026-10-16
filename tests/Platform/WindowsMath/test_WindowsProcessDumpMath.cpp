/// @file test_WindowsProcessDumpMath.cpp
/// @brief Windows' "Create dump file" (#807), its pure parts: the dump file's name, and what each failure
/// says -- access denied, a protected process, a full disk, a cancel, a process of another architecture. Pure, so
/// built everywhere.

#include "Platform/Windows/WindowsProcessDumpMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace Platform::DumpMath
{
namespace
{

constexpr DumpTime TIME{.year = 2026, .month = 10, .day = 3, .hour = 4, .minute = 5, .second = 9};

TEST(DumpFileNameTest, ImageNamePidAndTime)
{
    EXPECT_EQ(dumpFileName(R"(C:\Windows\System32\notepad.exe)", 1234, TIME), "notepad_1234_2026-10-03_04-05-09.dmp");
    EXPECT_EQ(dumpFileName("Notepad.EXE", 1234, TIME), "Notepad_1234_2026-10-03_04-05-09.dmp");
    // A later attempt, the first name taken already
    EXPECT_EQ(dumpFileName("notepad.exe", 1234, TIME, 2), "notepad_1234_2026-10-03_04-05-09 (2).dmp");
}

TEST(DumpFileNameTest, TheStemIsSafeForAFileName)
{
    EXPECT_EQ(dumpFileStem("my:app?.exe"), "my_app_");
    EXPECT_EQ(dumpFileStem("tool.com"), "tool.com");
    EXPECT_EQ(dumpFileStem(std::string("a\tb") + ".exe"), "a_b");
    EXPECT_EQ(dumpFileStem("trailing. "), "trailing");
    // Nothing to name it after
    EXPECT_EQ(dumpFileStem(""), "process");
    EXPECT_EQ(dumpFileStem(R"(C:\dir\)"), "process");
    EXPECT_EQ(dumpFileStem(".exe"), ".exe"); // nothing before the extension to drop it from
    // UTF-8 passes through untouched
    EXPECT_EQ(dumpFileStem("caf\xc3\xa9.exe"), "caf\xc3\xa9");
}

TEST(DumpErrorTest, AWrappedWin32ErrorIsUnwrapped)
{
    EXPECT_EQ(win32CodeFromDumpError(0x80070070U), ERROR_CODE_DISK_FULL);
    EXPECT_EQ(win32CodeFromDumpError(ERROR_CODE_ACCESS_DENIED), ERROR_CODE_ACCESS_DENIED);
    EXPECT_EQ(win32CodeFromDumpError(0x8000FFFFU), 0x8000FFFFU); // E_UNEXPECTED is not a wrapped Win32 code
    EXPECT_TRUE(isDiskFull(ERROR_CODE_DISK_FULL));
    EXPECT_TRUE(isDiskFull(ERROR_CODE_HANDLE_DISK_FULL));
    EXPECT_TRUE(isDiskFull(0x80070070U));
    EXPECT_FALSE(isDiskFull(ERROR_CODE_ACCESS_DENIED));
}

TEST(DumpErrorTest, OpeningSaysProtectedOrElevatedOrGone)
{
    const std::string protectedText = openFailureMessage(ERROR_CODE_ACCESS_DENIED, 600, true);
    EXPECT_TRUE(protectedText.contains("protected process")) << protectedText;
    EXPECT_TRUE(protectedText.contains("even as administrator")) << protectedText;

    const std::string elevatedText = openFailureMessage(ERROR_CODE_ACCESS_DENIED, 600, false);
    EXPECT_TRUE(elevatedText.starts_with("Access denied")) << elevatedText;
    EXPECT_TRUE(elevatedText.contains("run TaskSmack as administrator")) << elevatedText;

    EXPECT_EQ(openFailureMessage(ERROR_CODE_INVALID_PARAMETER, 600, false), "Process not found - may have already exited");
    EXPECT_EQ(openFailureMessage(1234, 600, false), "Failed to open process 600: error 1234");
}

TEST(DumpErrorTest, CreatingTheFileNamesTheFolder)
{
    EXPECT_EQ(createFileFailureMessage(ERROR_CODE_ACCESS_DENIED, "C:\\Locked"), "Access denied: can't write a file in C:\\Locked");
    EXPECT_EQ(createFileFailureMessage(ERROR_CODE_PATH_NOT_FOUND, "C:\\Gone"), "The folder C:\\Gone doesn't exist");
    EXPECT_EQ(createFileFailureMessage(ERROR_CODE_DISK_FULL, "D:\\"), "The disk holding D:\\ is full");
    EXPECT_EQ(createFileFailureMessage(31, "D:\\"), "Can't create a dump file in D:\\: error 31");
}

TEST(DumpErrorTest, WritingSaysDiskFullCancelledOrPartial)
{
    const std::string full = writeFailureMessage(0x80070070U, 42, "C:\\Temp", false);
    EXPECT_TRUE(full.starts_with("Not enough disk space in C:\\Temp")) << full;
    EXPECT_TRUE(full.contains("partial file was deleted")) << full;

    EXPECT_EQ(writeFailureMessage(0, 42, "C:\\Temp", true), "Cancelled; the partial dump file was deleted");
    EXPECT_EQ(writeFailureMessage(0x800704C7U, 42, "C:\\Temp", false), "Cancelled; the partial dump file was deleted");

    EXPECT_TRUE(writeFailureMessage(ERROR_CODE_PARTIAL_COPY, 42, "C:\\Temp", false).contains("Could not read all of the memory"));
    EXPECT_TRUE(writeFailureMessage(ERROR_CODE_ACCESS_DENIED, 42, "C:\\Temp", false).starts_with("Access denied"));
    EXPECT_EQ(writeFailureMessage(0x80004005U, 42, "C:\\Temp", false),
              "Failed to write the dump of process 42: error 0x80004005; the partial file was deleted");
}

TEST(DumpSnapshotTest, ALiveDumpSaysWhyThereWasNoSnapshot)
{
    // #2001: the access a snapshot needs refused (at the open, or by PssCaptureSnapshot itself).
    EXPECT_EQ(snapshotFailureReason(0, true), "Windows refused the access a snapshot of it needs");
    EXPECT_EQ(snapshotFailureReason(ERROR_CODE_ACCESS_DENIED, false), "Windows refused the access a snapshot of it needs");
    EXPECT_EQ(snapshotFailureReason(ERROR_CODE_NOT_ENOUGH_MEMORY, false), "there wasn't enough memory to take a snapshot of it");
    EXPECT_EQ(snapshotFailureReason(ERROR_CODE_COMMITMENT_LIMIT, false), "there wasn't enough memory to take a snapshot of it");
    EXPECT_EQ(snapshotFailureReason(ERROR_CODE_NOT_SUPPORTED, false), "this version of Windows can't take a snapshot of it");
    EXPECT_EQ(snapshotFailureReason(1234, false), "the snapshot failed with error 1234");
}

/// A row of dumpArchitecture()'s table: TaskSmack's machine, the process's, and the decision.
struct ArchitectureCase
{
    std::uint16_t self;
    std::uint16_t target;
    DumpArchitecture expected;
};

TEST(DumpArchitectureTest, EveryMachinePairDecides)
{
    constexpr std::array CASES{
        // The x64 build: x64 as usual, its x86 WOW64 guests with a note, anything ARM refused (#2152).
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_AMD64, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_I386, .expected = DumpArchitecture::Wow64Target},
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_ARM64, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_ARM64EC, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_ARM64X, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_ARMNT, .expected = DumpArchitecture::Unsupported},
        // IA64, which no process on these machines runs as.
        ArchitectureCase{.self = MACHINE_AMD64, .target = 0x0200, .expected = DumpArchitecture::Unsupported},
        // Unreadable: tried as before.
        ArchitectureCase{.self = MACHINE_AMD64, .target = MACHINE_UNKNOWN, .expected = DumpArchitecture::Same},
        // An ARM64 build: ARM64 (EC and X too) as usual, x86 and ARM32 WOW64 guests with a note, x64 refused.
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_ARM64, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_ARM64EC, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_ARM64X, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_I386, .expected = DumpArchitecture::Wow64Target},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_ARMNT, .expected = DumpArchitecture::Wow64Target},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_AMD64, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_ARM64, .target = MACHINE_UNKNOWN, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_ARM64EC, .target = MACHINE_ARM64, .expected = DumpArchitecture::Same},
        ArchitectureCase{.self = MACHINE_ARM64EC, .target = MACHINE_AMD64, .expected = DumpArchitecture::Unsupported},
        // No 32-bit build (#2152): a 32-bit TaskSmack, were there one, refuses everything.
        ArchitectureCase{.self = MACHINE_I386, .target = MACHINE_I386, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_I386, .target = MACHINE_AMD64, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_ARMNT, .target = MACHINE_ARMNT, .expected = DumpArchitecture::Unsupported},
        ArchitectureCase{.self = MACHINE_UNKNOWN, .target = MACHINE_AMD64, .expected = DumpArchitecture::Unsupported},
    };
    for (const ArchitectureCase& row : CASES)
    {
        EXPECT_EQ(dumpArchitecture(row.self, row.target), row.expected)
            << std::format("self 0x{:04X}, target 0x{:04X}", row.self, row.target);
    }
    static_assert(dumpArchitecture(MACHINE_AMD64, MACHINE_ARM64) == DumpArchitecture::Unsupported);
}

TEST(DumpArchitectureTest, TheProcessMachineTellsEmulatedX64FromNativeArm64)
{
    // Windows 11: ProcessMachineTypeInfo is read, and says what the process runs as.
    EXPECT_EQ(processMachine(MACHINE_AMD64, MACHINE_UNKNOWN, MACHINE_ARM64), MACHINE_AMD64); // x64 emulated on ARM64
    EXPECT_EQ(processMachine(MACHINE_ARM64, MACHINE_UNKNOWN, MACHINE_ARM64), MACHINE_ARM64); // native ARM64
    EXPECT_EQ(processMachine(MACHINE_I386, MACHINE_I386, MACHINE_AMD64), MACHINE_I386);
    // Windows 10 (IsWow64Process2 only): a WOW64 process's guest machine, else the native machine.
    EXPECT_EQ(processMachine(MACHINE_UNKNOWN, MACHINE_I386, MACHINE_AMD64), MACHINE_I386);
    EXPECT_EQ(processMachine(MACHINE_UNKNOWN, MACHINE_ARMNT, MACHINE_ARM64), MACHINE_ARMNT);
    EXPECT_EQ(processMachine(MACHINE_UNKNOWN, MACHINE_UNKNOWN, MACHINE_AMD64), MACHINE_AMD64);
    EXPECT_EQ(processMachine(MACHINE_UNKNOWN, MACHINE_UNKNOWN, MACHINE_ARM64), MACHINE_ARM64);
    // Nothing read
    EXPECT_EQ(processMachine(MACHINE_UNKNOWN, MACHINE_UNKNOWN, MACHINE_UNKNOWN), MACHINE_UNKNOWN);
}

TEST(DumpArchitectureTest, TheNoteAndTheRefusalNameTheMachines)
{
    EXPECT_TRUE(wow64DumpNote(MACHINE_I386).contains(".effmach x86")) << wow64DumpNote(MACHINE_I386);
    EXPECT_TRUE(wow64DumpNote(MACHINE_ARMNT).contains(".effmach arm ")) << wow64DumpNote(MACHINE_ARMNT);
    EXPECT_TRUE(wow64DumpNote(MACHINE_I386).starts_with("This is a 32-bit process"));

    EXPECT_EQ(unsupportedArchitectureMessage(9, MACHINE_AMD64, MACHINE_ARM64),
              "Process 9 is an ARM64 process, and this TaskSmack is an x64 build: it can't write a usable dump of a process of another "
              "architecture. Task Manager can dump it (Details, right-click, Create memory dump file).");
    const std::string ec = unsupportedArchitectureMessage(9, MACHINE_AMD64, MACHINE_ARM64EC);
    EXPECT_TRUE(ec.contains("is an ARM64EC process")) << ec;
    const std::string other = unsupportedArchitectureMessage(9, MACHINE_AMD64, 0x0200);
    EXPECT_TRUE(other.contains("runs as machine type 0x0200")) << other;
    const std::string oddSelf = unsupportedArchitectureMessage(9, MACHINE_UNKNOWN, MACHINE_AMD64);
    EXPECT_TRUE(oddSelf.contains("built for machine type 0x0000")) << oddSelf;

    EXPECT_EQ(machineName(MACHINE_ARM64X), "ARM64");
    EXPECT_EQ(machineName(MACHINE_ARMNT), "ARM32");
    EXPECT_TRUE(machineName(MACHINE_UNKNOWN).empty());
}

} // namespace
} // namespace Platform::DumpMath
