/// @file test_ProcessArchitectureNote.cpp
/// @brief Process Details' Architecture row tooltip (#2394): what emulation costs, WOW64 and a 32-bit
/// program on Linux, ARM64EC, a native process, and nothing for an unread architecture.

#include "App/Panels/ProcessArchitectureNote.h"

#include <gtest/gtest.h>

#include <string_view>

namespace App
{
namespace
{

using ProcessArchitectureNote::architectureNote;

TEST(ProcessArchitectureNoteTest, EmulationSaysItCostsPerformance)
{
    for (const std::string_view emulated : {"x64 (emulated)", "x86 (emulated)", "ARM64 (emulated)"})
    {
        const std::string_view note = architectureNote(emulated);
        EXPECT_TRUE(note.starts_with("Emulated")) << emulated;
        EXPECT_TRUE(note.contains("costs performance")) << emulated;
    }
}

TEST(ProcessArchitectureNoteTest, EachKindHasItsOwnNote)
{
    EXPECT_TRUE(architectureNote("x86 (WOW64)").contains("WOW64"));
    EXPECT_TRUE(architectureNote("ARM32 (WOW64)").contains("WOW64"));
    EXPECT_TRUE(architectureNote("x86 (32-bit)").contains("64-bit kernel"));
    EXPECT_TRUE(architectureNote("ARM64EC").starts_with("ARM64EC"));
    EXPECT_TRUE(architectureNote("x64").starts_with("Native"));
    EXPECT_TRUE(architectureNote("ARM64").starts_with("Native"));
    EXPECT_TRUE(architectureNote("").empty());
}

} // namespace
} // namespace App
