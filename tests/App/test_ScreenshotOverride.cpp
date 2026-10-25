/// @file test_ScreenshotOverride.cpp
/// @brief App::ScreenshotOverride (#2827): the TASKSMACK_SCREENSHOT parser -- the path, the after=,
/// frames= and fps= options, their defaults and limits, and the values it refuses -- and the names a
/// sequence's files get.

#include "App/ScreenshotOverride.h"
#include "Core/Utf8Path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

namespace
{

using App::ScreenshotOverride::DEFAULT_AFTER_SECONDS;
using App::ScreenshotOverride::DEFAULT_FPS;
using App::ScreenshotOverride::MAX_AFTER_SECONDS;
using App::ScreenshotOverride::MAX_FPS;
using App::ScreenshotOverride::MAX_FRAMES;
using App::ScreenshotOverride::parse;
using App::ScreenshotOverride::Request;

/// A request for @p path with the given numbers, the defaults unless said otherwise.
[[nodiscard]] Request request(const char* path, const int after = DEFAULT_AFTER_SECONDS, const int frames = 1, const int fps = DEFAULT_FPS)
{
    return Request{.path = Core::utf8ToPath(path), .afterSeconds = after, .frames = frames, .fps = fps};
}

// ========== Parser ==========

TEST(ScreenshotOverrideParseTest, UnsetAndBlankGiveNoRequestAndNoWarning)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "\t\r\n"})
    {
        SCOPED_TRACE(value == nullptr ? "(nullptr)" : value);
        const auto result = parse(value);
        EXPECT_FALSE(result.request.has_value());
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(ScreenshotOverrideParseTest, APathAloneTakesTheDefaults)
{
    struct Case
    {
        const char* value = nullptr;
        const char* path = nullptr; ///< the path the value names
    };
    for (const Case& c : {
             Case{.value = "shot.png", .path = "shot.png"},
             Case{.value = "out/shot.png", .path = "out/shot.png"},
             Case{.value = "shot", .path = "shot"},
             Case{.value = " shot.png ", .path = "shot.png"},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_EQ(result.request, std::optional<Request>{request(c.path)}); // the defaults: after 3 s, 1 frame, 10 fps
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
    EXPECT_EQ(DEFAULT_AFTER_SECONDS, 3);
    EXPECT_EQ(DEFAULT_FPS, 10);
}

TEST(ScreenshotOverrideParseTest, OptionsParseInAnyOrderAndCase)
{
    struct Case
    {
        const char* value = nullptr;
        Request expected;
    };
    for (const Case& c : {
             Case{.value = "shot.png;after=5", .expected = request("shot.png", 5)},
             Case{.value = "shot.png;AFTER=5", .expected = request("shot.png", 5)},
             Case{.value = " shot.png ; after = 5 ", .expected = request("shot.png", 5)},
             Case{.value = "shot.png;after=0", .expected = request("shot.png", 0)},
             Case{.value = "shot.png;after=007", .expected = request("shot.png", 7)},
             Case{.value = "run.png;frames=30;fps=10", .expected = request("run.png", DEFAULT_AFTER_SECONDS, 30, 10)},
             Case{.value = "run.png;fps=30;frames=3;after=1", .expected = request("run.png", 1, 3, 30)},
             Case{.value = "run.png;Frames=2", .expected = request("run.png", DEFAULT_AFTER_SECONDS, 2, DEFAULT_FPS)},
             Case{.value = "run.png;FPS=1", .expected = request("run.png", DEFAULT_AFTER_SECONDS, 1, 1)},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_EQ(result.request, std::optional<Request>{c.expected});
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(ScreenshotOverrideParseTest, TheLimitsAreInclusive)
{
    const auto result = parse("run.png;after=60;frames=600;fps=60");
    EXPECT_EQ(result.request, std::optional<Request>{request("run.png", MAX_AFTER_SECONDS, MAX_FRAMES, MAX_FPS)});
    EXPECT_TRUE(result.warning.empty()) << result.warning;
    EXPECT_EQ(MAX_AFTER_SECONDS, 60);
    EXPECT_EQ(MAX_FRAMES, 600);
    EXPECT_EQ(MAX_FPS, 60);
}

TEST(ScreenshotOverrideParseTest, NumbersOutsideTheirRangesAreClampedWithAWarning)
{
    struct Case
    {
        const char* value = nullptr;
        Request expected;
        const char* clamped = nullptr; ///< the option named in the warning
    };
    for (const Case& c : {
             Case{
                 .value = "shot.png;after=61", .expected = request("shot.png", MAX_AFTER_SECONDS), .clamped = "after clamped to 60 (0-60)"},
             // Past int: saturates, then clamps like any other number that is too large.
             Case{.value = "shot.png;after=99999999999999999999",
                  .expected = request("shot.png", MAX_AFTER_SECONDS),
                  .clamped = "after clamped to 60 (0-60)"},
             Case{.value = "run.png;frames=0", .expected = request("run.png"), .clamped = "frames clamped to 1 (1-600)"},
             Case{.value = "run.png;frames=601",
                  .expected = request("run.png", DEFAULT_AFTER_SECONDS, MAX_FRAMES),
                  .clamped = "frames clamped to 600 (1-600)"},
             Case{.value = "run.png;fps=0",
                  .expected = request("run.png", DEFAULT_AFTER_SECONDS, 1, 1),
                  .clamped = "fps clamped to 1 (1-60)"},
             Case{.value = "run.png;fps=61",
                  .expected = request("run.png", DEFAULT_AFTER_SECONDS, 1, MAX_FPS),
                  .clamped = "fps clamped to 60 (1-60)"},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_EQ(result.request, std::optional<Request>{c.expected});
        EXPECT_NE(result.warning.find(c.clamped), std::string::npos) << result.warning;
    }

    // Every clamped number is named.
    const auto both = parse("run.png;after=61;fps=0");
    EXPECT_EQ(both.request, std::optional<Request>{request("run.png", MAX_AFTER_SECONDS, 1, 1)});
    EXPECT_NE(both.warning.find("after clamped to 60"), std::string::npos) << both.warning;
    EXPECT_NE(both.warning.find("fps clamped to 1"), std::string::npos) << both.warning;
}

TEST(ScreenshotOverrideParseTest, InvalidValuesAreIgnoredWithAWarning)
{
    for (const char* value : {";after=3",
                              " ; ",
                              "shot.png;",
                              "shot.png;after",
                              "shot.png;after=",
                              "shot.png;=3",
                              "shot.png;after=abc",
                              "shot.png;after=-1",
                              "shot.png;after=+1",
                              "shot.png;after=1.5",
                              "shot.png;after=3s",
                              "shot.png;later=3",
                              "shot.png;after=3;after=4",
                              "run.png;frames=3;FRAMES=4",
                              "run.png;fps=10;fps=10"})
    {
        SCOPED_TRACE(value);
        const auto result = parse(value);
        EXPECT_FALSE(result.request.has_value());
        EXPECT_NE(result.warning.find("ignored"), std::string::npos) << result.warning;
    }
}

// #2170: every TASKSMACK_* hook trims the same ASCII whitespace (Platform::trimAscii()), so a value
// copied from a Windows .env file, with its trailing CR, parses as it does without one.
TEST(ScreenshotOverrideParseTest, AnyAsciiWhitespaceAroundTheValueIsTrimmed)
{
    const auto result = parse("\tshot.png;after=2\r\n");
    EXPECT_EQ(result.request, std::optional<Request>{request("shot.png", 2)});
    EXPECT_TRUE(result.warning.empty()) << result.warning;
}

// The value is UTF-8, as every hook's is (#1648): the path is decoded as such, whatever the code page.
TEST(ScreenshotOverrideParseTest, ThePathIsDecodedAsUtf8)
{
    const auto result = parse("снимок экрана.png;after=1");
    EXPECT_EQ(result.request, std::optional<Request>{request("снимок экрана.png", 1)});
    EXPECT_EQ(Core::pathToUtf8(result.request.value_or(Request{}).path), "снимок экрана.png");
}

// ========== A sequence's file names ==========

TEST(ScreenshotOverrideTest, FrameFileNamesAreNumberedBesideThePath)
{
    using App::ScreenshotOverride::frameFileName;
    EXPECT_EQ(frameFileName(std::filesystem::path("shots") / "run.png", 1), std::filesystem::path("shots") / "run-0001.png");
    EXPECT_EQ(frameFileName("run", 12), std::filesystem::path("run-0012.png"));
    EXPECT_EQ(frameFileName(std::filesystem::path("a") / "b" / "c.PNG", 600), std::filesystem::path("a") / "b" / "c-0600.png");
    EXPECT_EQ(frameFileName(Core::utf8ToPath("снимок.png"), 3), Core::utf8ToPath("снимок-0003.png"));
}

TEST(ScreenshotOverrideTest, SequencePatternNamesTheFilesForALogLine)
{
    using App::ScreenshotOverride::sequencePattern;
    EXPECT_EQ(sequencePattern(std::filesystem::path("shots") / "run.png"),
              Core::pathToUtf8(std::filesystem::path("shots") / "run-NNNN.png"));
    EXPECT_EQ(sequencePattern("run.png"), "run-NNNN.png");
}

TEST(ScreenshotOverrideTest, Describe)
{
    using App::ScreenshotOverride::describe;
    EXPECT_EQ(describe(request("shot.png")), "shot.png after 3 s");
    EXPECT_EQ(describe(request("shot.png", 0)), "shot.png after 0 s");
    EXPECT_EQ(describe(request("run.png", 3, 30, 10)), "30 frames at 10 fps after 3 s -> run-NNNN.png");
}

} // namespace
