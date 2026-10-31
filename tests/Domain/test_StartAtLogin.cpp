/// @file test_StartAtLogin.cpp
/// @brief Start at sign-in's decisions (#3093, part 2): the Run value's text (the quoted path, then
/// --minimized), the program a command starts, path comparison, and what an entry means for this copy:
/// Off, On, another copy (Repair), or a failure. Pure, so they run on every host.

#include "Domain/StartAtLogin.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace Domain::StartAtLogin
{
namespace
{

constexpr const char* INSTALLED = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
constexpr const char* PORTABLE = R"(D:\Tools\TaskSmack 0.10\TaskSmack.exe)";

TEST(StartAtLoginTest, TheRunValueIsTheQuotedPathThenMinimized)
{
    EXPECT_EQ(runCommand(R"(C:\TaskSmack\TaskSmack.exe)"), R"("C:\TaskSmack\TaskSmack.exe" --minimized)");
    // A path with spaces stays one argument.
    EXPECT_EQ(runCommand(INSTALLED), R"("C:\Program Files\TaskSmack\TaskSmack.exe" --minimized)");
    EXPECT_EQ(runCommand(PORTABLE), R"("D:\Tools\TaskSmack 0.10\TaskSmack.exe" --minimized)");
    EXPECT_EQ(MINIMIZED_ARGUMENT, "--minimized");
}

TEST(StartAtLoginTest, TheProgramIsReadBackFromTheRunValue)
{
    EXPECT_EQ(commandProgram(runCommand(INSTALLED)), std::optional<std::string>(INSTALLED));
    EXPECT_EQ(commandProgram(runCommand(PORTABLE)), std::optional<std::string>(PORTABLE));
    EXPECT_EQ(commandProgram(R"(  "C:\a b\x.exe")"), std::optional<std::string>(R"(C:\a b\x.exe)"));
    // Unquoted: up to the first space, as Windows reads it.
    EXPECT_EQ(commandProgram(R"(C:\TaskSmack\TaskSmack.exe --minimized)"), std::optional<std::string>(R"(C:\TaskSmack\TaskSmack.exe)"));
    EXPECT_EQ(commandProgram(R"(C:\x.exe)"), std::optional<std::string>(R"(C:\x.exe)"));
    EXPECT_EQ(commandProgram(""), std::nullopt);
    EXPECT_EQ(commandProgram("   "), std::nullopt);
    EXPECT_EQ(commandProgram(R"("C:\never closed)"), std::nullopt);
    EXPECT_EQ(commandProgram(R"("" --minimized)"), std::nullopt);
}

TEST(StartAtLoginTest, PathsCompareByWindowsRules)
{
    EXPECT_TRUE(samePath(INSTALLED, R"(c:\program files\tasksmack\TASKSMACK.EXE)", true));
    EXPECT_FALSE(samePath(INSTALLED, R"(c:\program files\tasksmack\TASKSMACK.EXE)", false));
    EXPECT_TRUE(samePath("C:/TaskSmack/TaskSmack.exe", R"(C:\TaskSmack\TaskSmack.exe)", true));
    EXPECT_FALSE(samePath(INSTALLED, PORTABLE, true));
    EXPECT_FALSE(samePath("", "", true));
    EXPECT_FALSE(samePath(INSTALLED, "", true));
}

TEST(StartAtLoginTest, InsideAFolderMeansBelowIt)
{
    EXPECT_TRUE(isInside(INSTALLED, R"(C:\Program Files)", true));
    EXPECT_TRUE(isInside(INSTALLED, R"(c:\program files\)", true));
    EXPECT_FALSE(isInside(INSTALLED, R"(c:\program files)", false));
    EXPECT_FALSE(isInside(R"(C:\Program Files (x86)\TaskSmack\TaskSmack.exe)", R"(C:\Program Files)", true));
    EXPECT_FALSE(isInside(R"(C:\Program FilesX\TaskSmack.exe)", R"(C:\Program Files)", true));
    EXPECT_FALSE(isInside(R"(C:\Program Files)", R"(C:\Program Files)", true));
    EXPECT_FALSE(isInside(INSTALLED, "", true));
    EXPECT_FALSE(isInside(INSTALLED, R"(\\)", true));
}

TEST(StartAtLoginTest, NoEntryIsOff)
{
    const Status status = classify(std::nullopt, INSTALLED, true, true);
    EXPECT_EQ(status.state, State::Off);
    EXPECT_TRUE(status.known());
    EXPECT_FALSE(status.enabled());
    EXPECT_TRUE(status.installed);
    EXPECT_FALSE(showPortableNote(status));
}

TEST(StartAtLoginTest, ThisCopysEntryIsOn)
{
    const Status status = classify(runCommand(INSTALLED), R"(c:\PROGRAM FILES\TaskSmack\TaskSmack.exe)", true, true);
    EXPECT_EQ(status.state, State::On);
    EXPECT_TRUE(status.enabled());
    EXPECT_TRUE(status.otherProgram.empty());
    EXPECT_FALSE(showPortableNote(status)) << "an installed copy";
    // This copy's path without --minimized (edited by hand) still starts this copy.
    EXPECT_EQ(classify(std::string("\"") + INSTALLED + "\"", INSTALLED, true, true).state, State::On);
}

TEST(StartAtLoginTest, APortableCopyThatStartsAtSignInIsWarned)
{
    const Status status = classify(runCommand(PORTABLE), PORTABLE, false, true);
    EXPECT_EQ(status.state, State::On);
    EXPECT_TRUE(showPortableNote(status));
    EXPECT_STREQ(PORTABLE_NOTE, "Turn this off before moving or deleting this copy.");
    EXPECT_FALSE(showPortableNote(classify(std::nullopt, PORTABLE, false, true))) << "only while it is on";
}

TEST(StartAtLoginTest, AnotherCopysEntryOffersRepair)
{
    const Status status = classify(runCommand(R"(E:\Old\TaskSmack.exe)"), INSTALLED, true, true);
    EXPECT_EQ(status.state, State::OtherCopy);
    EXPECT_TRUE(status.enabled()) << "the box is ticked: something starts";
    EXPECT_EQ(status.otherProgram, R"(E:\Old\TaskSmack.exe)");
    EXPECT_EQ(statusLine(status, false), R"(Points to another copy: E:\Old\TaskSmack.exe. Repair points it to this copy.)");
    EXPECT_FALSE(showPortableNote(status));

    // On Linux paths compare with their case.
    EXPECT_EQ(classify(runCommand("/opt/TaskSmack"), "/opt/tasksmack", false, false).state, State::OtherCopy);

    // A value that names no program (empty, not text) reads as another, unreadable one.
    const Status unreadable = classify(std::string{}, INSTALLED, true, true);
    EXPECT_EQ(unreadable.state, State::OtherCopy);
    EXPECT_TRUE(unreadable.otherProgram.empty());
    EXPECT_EQ(statusLine(unreadable, false), "Points to a command TaskSmack can't read. Repair points it to this copy.");
}

TEST(StartAtLoginTest, AFailureSaysWhy)
{
    const Status status = failed("couldn't open the Run key: Access is denied.");
    EXPECT_EQ(status.state, State::Failed);
    EXPECT_FALSE(status.known());
    EXPECT_FALSE(status.enabled());
    EXPECT_EQ(statusLine(status, false), "Couldn't read or change it: couldn't open the Run key: Access is denied.");
}

TEST(StartAtLoginTest, EachStateHasItsLine)
{
    EXPECT_EQ(statusLine(Status{}, false), "Checking\xE2\x80\xA6");
    EXPECT_FALSE(Status{}.known());
    EXPECT_EQ(statusLine(Status{}, true), "Updating\xE2\x80\xA6");
    const Status off = classify(std::nullopt, INSTALLED, true, true);
    EXPECT_EQ(statusLine(off, false), "Off: TaskSmack doesn't start when you sign in.");
    EXPECT_EQ(statusLine(off, true), "Updating\xE2\x80\xA6");
    EXPECT_EQ(statusLine(classify(runCommand(INSTALLED), INSTALLED, true, true), false),
              "On: this copy starts minimized when you sign in.");
}

TEST(StartAtLoginTest, StatusesCompareByEveryField)
{
    EXPECT_EQ(classify(std::nullopt, INSTALLED, true, true), classify(std::nullopt, INSTALLED, true, true));
    EXPECT_NE(classify(std::nullopt, INSTALLED, true, true), classify(std::nullopt, INSTALLED, false, true));
    EXPECT_NE(failed("a"), failed("b"));
}

} // namespace
} // namespace Domain::StartAtLogin
