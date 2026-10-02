/// @file test_ProcessTableSettings.cpp
/// @brief Tests for the filter the Processes table's saved column layout passes through on its way
/// to and from the config file (#952).

#include "App/Panels/ProcessTableSettings.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace App
{
namespace
{

using ProcessTableSettings::extractTableSection;
using ProcessTableSettings::MAX_STORED_BYTES;
using ProcessTableSettings::MAX_STORED_LINES;
using ProcessTableSettings::sanitize;

// A section as ImGui writes it, for a table whose third column is the stretch one.
constexpr std::string_view SECTION = "[Table][0x1A2B3C4D,3]\n"
                                     "RefScale=13\n"
                                     "Column 0  Width=60 Order=1 ID=0x00000000\n"
                                     "Column 1  Width=180 Order=0 Sort=0^\n"
                                     "Column 2  Weight=1.0000 Order=2\n";

// ========== sanitize ==========

TEST(ProcessTableSettingsTest, WellFormedSectionSurvivesUnchanged)
{
    EXPECT_EQ(sanitize(SECTION), SECTION);
}

TEST(ProcessTableSettingsTest, SanitizeIsIdempotent)
{
    const std::string once = sanitize(SECTION);
    EXPECT_EQ(sanitize(once), once);
}

// Column visibility lives in [process_columns]. A second copy here would let the two disagree, with
// ImGui's copy winning at startup, so it is stripped.
TEST(ProcessTableSettingsTest, VisibilityIsRemoved)
{
    const std::string_view withVisible = "[Table][0x1A2B3C4D,2]\n"
                                         "Column 0  Width=60 Visible=1 Order=0\n"
                                         "Column 1  Width=180 Visible=0 Order=1 Sort=0v\n";
    EXPECT_EQ(sanitize(withVisible),
              "[Table][0x1A2B3C4D,2]\n"
              "Column 0  Width=60 Order=0\n"
              "Column 1  Width=180 Order=1 Sort=0v\n");
}

// Only the first section is passed on. The text comes from a user-editable file and goes to
// ImGui's settings parser, which would otherwise happily apply a window position or docking layout.
TEST(ProcessTableSettingsTest, AnythingAfterTheFirstSectionIsDropped)
{
    const std::string text = std::string(SECTION) + "\n[Window][Debug##Default]\nPos=60,60\nSize=400,400\n";
    EXPECT_EQ(sanitize(text), SECTION);

    const std::string adjacent = std::string(SECTION) + "[Docking][Data]\nDockSpace ID=0x1 Window=0x2\n";
    EXPECT_EQ(sanitize(adjacent), SECTION);
}

TEST(ProcessTableSettingsTest, TextNotStartingWithATableHeaderIsRejected)
{
    EXPECT_EQ(sanitize(""), "");
    EXPECT_EQ(sanitize("Column 0  Width=60\n"), "");
    EXPECT_EQ(sanitize("[Window][Main]\nPos=0,0\n"), "");
    EXPECT_EQ(sanitize("garbage"), "");
}

TEST(ProcessTableSettingsTest, MalformedHeaderIsRejected)
{
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4,3]\nColumn 0  Width=60\n"), "");      // 7 hex digits
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4G,3]\nColumn 0  Width=60\n"), "");     // not hex
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,]\nColumn 0  Width=60\n"), "");      // no column count
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,3\nColumn 0  Width=60\n"), "");      // no closing bracket
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,99999]\nColumn 0  Width=60\n"), ""); // absurd column count
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,3] extra\nColumn 0  Width=60\n"), "");
}

// Lines that are not settings lines, or contain characters ImGui never writes, are skipped rather
// than passed to the parser.
TEST(ProcessTableSettingsTest, UnexpectedLinesAreSkipped)
{
    const std::string_view text = "[Table][0x1A2B3C4D,2]\n"
                                  "RefScale=13\n"
                                  "LastUsed=20261002\n"
                                  "Column 0  Width=60 Order=0\n"
                                  "Column 1  Width=180%n Order=1\n"
                                  "Column 1  Width=180 Order=1\n";
    EXPECT_EQ(sanitize(text),
              "[Table][0x1A2B3C4D,2]\n"
              "RefScale=13\n"
              "Column 0  Width=60 Order=0\n"
              "Column 1  Width=180 Order=1\n");
}

TEST(ProcessTableSettingsTest, LeadingBlankLinesAndCarriageReturnsAreTolerated)
{
    const std::string_view text = "\n\r\n[Table][0x1A2B3C4D,1]\r\nColumn 0  Width=60 Order=0\r\n";
    EXPECT_EQ(sanitize(text), "[Table][0x1A2B3C4D,1]\nColumn 0  Width=60 Order=0\n");
}

TEST(ProcessTableSettingsTest, OversizedTextIsRejected)
{
    std::string text(SECTION);
    while (text.size() <= MAX_STORED_BYTES)
    {
        text += "Column 9  Width=60 Order=9\n";
    }
    EXPECT_EQ(sanitize(text), "");
}

// The line limit is a rejection boundary, not a truncation point, and it counts every line in the
// section: exactly MAX_STORED_LINES is accepted whole, one more is rejected whole.
TEST(ProcessTableSettingsTest, SectionAtTheLineLimitIsAccepted)
{
    std::string text = "[Table][0x1A2B3C4D,31]\n";
    for (std::size_t i = 1; i < MAX_STORED_LINES; ++i)
    {
        text += "Column 1  Width=60\n";
    }
    EXPECT_EQ(sanitize(text), text);
}

TEST(ProcessTableSettingsTest, SectionOverTheLineLimitIsRejectedNotTruncated)
{
    std::string text = "[Table][0x1A2B3C4D,31]\n";
    for (std::size_t i = 1; i < MAX_STORED_LINES + 1; ++i)
    {
        text += "Column 1  Width=60\n";
    }
    EXPECT_EQ(sanitize(text), "");
}

// Lines that would be skipped still count, so the limit cannot be sidestepped with filler.
TEST(ProcessTableSettingsTest, SkippedLinesCountTowardsTheLineLimit)
{
    std::string text = "[Table][0x1A2B3C4D,31]\nColumn 0  Width=60\n";
    for (std::size_t i = 0; i < MAX_STORED_LINES; ++i)
    {
        text += "junk\n";
    }
    EXPECT_EQ(sanitize(text), "");
}

// The limit applies to the section, not to whatever follows it.
TEST(ProcessTableSettingsTest, LinesAfterTheSectionDoNotCountTowardsTheLimit)
{
    std::string text = std::string(SECTION) + "\n";
    for (std::size_t i = 0; i < MAX_STORED_LINES; ++i)
    {
        text += "x\n";
    }
    EXPECT_EQ(sanitize(text), SECTION);
}

TEST(ProcessTableSettingsTest, HeaderAloneIsAccepted)
{
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,31]\n"), "[Table][0x1A2B3C4D,31]\n");
    EXPECT_EQ(sanitize("[Table][0x1A2B3C4D,31]"), "[Table][0x1A2B3C4D,31]\n");
}

// ========== Numeric and grammar validation ==========

// The text is read back by ImGui with "%d" and "%f". "%f" accepts "nan" and "inf", and an integer
// too large for an int is undefined behaviour in "%d", so values are validated as bounded digit
// runs -- a character filter alone lets all of these through.
TEST(ProcessTableSettingsTest, NonFiniteAndOutOfRangeValuesAreRejected)
{
    const std::string_view header = "[Table][0x1A2B3C4D,2]\n";
    const auto withLine = [&](std::string_view line)
    {
        return sanitize(std::string(header) + std::string(line) + "\n");
    };

    EXPECT_EQ(withLine("RefScale=nan"), header);
    EXPECT_EQ(withLine("RefScale=inf"), header);
    EXPECT_EQ(withLine("RefScale=-13"), header);
    EXPECT_EQ(withLine("RefScale=1e9"), header);
    EXPECT_EQ(withLine("RefScale=13.5.2"), header);
    EXPECT_EQ(withLine("RefScale=123456789"), header);
    EXPECT_EQ(withLine("RefScale="), header);

    EXPECT_EQ(withLine("Column 0  Width=99999999999999999999"), header);
    EXPECT_EQ(withLine("Column 0  Width=123456"), header);
    EXPECT_EQ(withLine("Column 0  Width=-60"), header);
    EXPECT_EQ(withLine("Column 0  Width=6.5"), header);
    EXPECT_EQ(withLine("Column 0  Weight=nan"), header);
    EXPECT_EQ(withLine("Column 0  Weight=inf"), header);
    EXPECT_EQ(withLine("Column 0  Weight=1.0e5"), header);
    EXPECT_EQ(withLine("Column 0  Order=99999"), header);
    EXPECT_EQ(withLine("Column 0  Sort=0x"), header);
    EXPECT_EQ(withLine("Column 0  Sort=999^"), header);
    EXPECT_EQ(withLine("Column 0  Sort=^"), header);
    EXPECT_EQ(withLine("Column 0  ID=0x123"), header);
    EXPECT_EQ(withLine("Column 0  ID=1A2B3C4D5E"), header);
    EXPECT_EQ(withLine("Column 9999  Width=60"), header);
    EXPECT_EQ(withLine("Column x  Width=60"), header);
    EXPECT_EQ(withLine("Column 0  Bogus=1"), header);
    EXPECT_EQ(withLine("Column 0  Width="), header);
}

TEST(ProcessTableSettingsTest, InRangeValuesAreKept)
{
    const std::string_view text = "[Table][0x1A2B3C4D,3]\n"
                                  "RefScale=13.3333\n"
                                  "Column 0  Width=99999 Order=511 Sort=12^ ID=0xDEADBEEF\n"
                                  "Column 10 Weight=0.5000 Order=0\n"
                                  "Column 2  Width=0\n";
    EXPECT_EQ(sanitize(text), text);
}

// ImGui's parser tries each key once, in a fixed sequence, so keys out of order are only partly
// read. The filter writes them back in ImGui's order whatever order they arrived in.
TEST(ProcessTableSettingsTest, KeysAreRewrittenInImGuiOrder)
{
    const std::string_view scrambled = "[Table][0x1A2B3C4D,1]\n"
                                       "Column 0 ID=0x00000001   Sort=0v Order=2  Width=60\n";
    EXPECT_EQ(sanitize(scrambled), "[Table][0x1A2B3C4D,1]\nColumn 0  Width=60 Order=2 Sort=0v ID=0x00000001\n");
}

// ========== carrySortForward ==========

using ProcessTableSettings::carrySortForward;

// The reviewed defect: a layout captured in tree view has no sort, and saving it as it stands would
// erase the list-view sort. The sort is carried across from the list-view layout onto the freshly
// captured widths and order.
TEST(ProcessTableSettingsTest, SortIsCarriedOntoALayoutCapturedWithoutOne)
{
    const std::string_view listView = "[Table][0x1A2B3C4D,3]\n"
                                      "RefScale=11\n"
                                      "Column 0  Width=60 Order=0\n"
                                      "Column 1  Width=120 Order=1 Sort=0^ ID=0x00000001\n"
                                      "Column 2  Weight=1.0000 Order=2\n";
    const std::string_view treeView = "[Table][0x1A2B3C4D,3]\n"
                                      "RefScale=11\n"
                                      "Column 0  Width=60 Order=1\n"
                                      "Column 1  Width=240 Order=0 ID=0x00000001\n"
                                      "Column 2  Weight=1.0000 Order=2\n";

    EXPECT_EQ(carrySortForward(treeView, listView),
              "[Table][0x1A2B3C4D,3]\n"
              "RefScale=11\n"
              "Column 0  Width=60 Order=1\n"
              "Column 1  Width=240 Order=0 Sort=0^ ID=0x00000001\n"
              "Column 2  Weight=1.0000 Order=2\n");
}

// A sort that the captured layout does have is the newer one, and wins.
TEST(ProcessTableSettingsTest, AnExistingSortIsNotOverwritten)
{
    const std::string_view source = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60 Sort=0^\nColumn 1  Width=60\n";
    const std::string_view captured = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60 Sort=0v\nColumn 1  Width=60\n";
    EXPECT_EQ(carrySortForward(captured, source), captured);
}

TEST(ProcessTableSettingsTest, CarrySortForwardWithNothingToCarryReturnsTheSanitisedCapture)
{
    EXPECT_EQ(carrySortForward(SECTION, ""), SECTION);
    EXPECT_EQ(carrySortForward(SECTION, "not a layout"), SECTION);
    EXPECT_EQ(carrySortForward(SECTION, "[Table][0x1A2B3C4D,3]\nColumn 0  Width=60\n"), SECTION);
    EXPECT_EQ(carrySortForward("", SECTION), "");
    EXPECT_EQ(carrySortForward("garbage", SECTION), "");
}

// ========== extractTableSection ==========

TEST(ProcessTableSettingsTest, ExtractsTheNamedTableFromFullIni)
{
    const std::string ini = "[Window][Debug##Default]\nPos=60,60\nSize=400,400\n\n"
                            "[Table][0x0BADF00D,2]\nColumn 0  Width=10\nColumn 1  Width=20\n\n" +
                            std::string(SECTION) + "\n[Docking][Data]\nDockSpace ID=0x1\n";

    EXPECT_EQ(extractTableSection(ini, 0x1A2B3C4DU), SECTION);
    EXPECT_EQ(extractTableSection(ini, 0x0BADF00DU), "[Table][0x0BADF00D,2]\nColumn 0  Width=10\nColumn 1  Width=20\n");
}

TEST(ProcessTableSettingsTest, ExtractReturnsEmptyWhenTheTableHasNoSection)
{
    const std::string ini = "[Window][Main]\nPos=0,0\n\n" + std::string(SECTION);
    EXPECT_EQ(extractTableSection(ini, 0xFFFFFFFFU), "");
    EXPECT_EQ(extractTableSection("", 0x1A2B3C4DU), "");
}

// The header must start a line: the same characters inside another line are not a section.
TEST(ProcessTableSettingsTest, ExtractIgnoresAHeaderThatDoesNotStartALine)
{
    const std::string ini = "Note=[Table][0x1A2B3C4D,3]\nColumn 0  Width=1\n\n" + std::string(SECTION);
    EXPECT_EQ(extractTableSection(ini, 0x1A2B3C4DU), SECTION);
}

// A large ini must not defeat extraction: only the one section is size-checked.
TEST(ProcessTableSettingsTest, ExtractWorksInsideAnIniLargerThanTheStoredLimit)
{
    std::string ini;
    while (ini.size() <= MAX_STORED_BYTES * 2)
    {
        ini += "[Window][W]\nPos=1,1\nSize=2,2\n\n";
    }
    ini += std::string(SECTION) + "\n";
    EXPECT_EQ(extractTableSection(ini, 0x1A2B3C4DU), SECTION);
}

TEST(ProcessTableSettingsTest, ExtractedSectionHasNoVisibility)
{
    const std::string ini = "[Table][0x1A2B3C4D,1]\nColumn 0  Width=60 Visible=0 Order=0\n\n";
    EXPECT_EQ(extractTableSection(ini, 0x1A2B3C4DU), "[Table][0x1A2B3C4D,1]\nColumn 0  Width=60 Order=0\n");
}

} // namespace
} // namespace App
