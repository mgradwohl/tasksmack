/// @file test_ProcessTableSettings.cpp
/// @brief Tests for the filter the Processes table's saved column layout passes through on its way
/// to and from the config file (#952).

#include "App/Panels/ProcessTableSettings.h"

#include <gtest/gtest.h>

#include <format>
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
    // ImGui shifts a 64-bit mask by the sort order, so 64 and above must never reach it.
    EXPECT_EQ(withLine("Column 0  Sort=64^"), header);
    EXPECT_EQ(withLine("Column 0  Sort=99v"), header);
    // A zero stretch weight makes ImGui's weight / sum-of-weights a 0 / 0.
    EXPECT_EQ(withLine("Column 0  Weight=0"), header);
    EXPECT_EQ(withLine("Column 0  Weight=0.0000"), header);
    EXPECT_EQ(withLine("Column 0  Weight=000.000"), header);
    EXPECT_EQ(withLine("Column 0  ID=0x123"), header);
    EXPECT_EQ(withLine("Column 0  ID=1A2B3C4D5E"), header);
    EXPECT_EQ(withLine("Column 9999  Width=60"), header);
    EXPECT_EQ(withLine("Column x  Width=60"), header);
    EXPECT_EQ(withLine("Column 0  Bogus=1"), header);
    EXPECT_EQ(withLine("Column 0  Width="), header);
}

TEST(ProcessTableSettingsTest, BoundaryValuesAreKept)
{
    const std::string_view text = "[Table][0x1A2B3C4D,3]\n"
                                  "Column 0  Sort=63^\n"
                                  "Column 1  Weight=0.0001\n"
                                  "Column 2  Sort=0v\n";
    EXPECT_EQ(sanitize(text), text);
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

// A sort on a *different* column is still the newer sort: the user re-sorted after the backup was
// taken. Carrying the old one across too would make a two-column sort out of a one-column one.
TEST(ProcessTableSettingsTest, ASortOnAnotherColumnIsNotJoinedByTheOldOne)
{
    const std::string_view source = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60 Sort=0^\nColumn 1  Width=60\n";
    const std::string_view captured = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60\nColumn 1  Width=60 Sort=0v\n";
    EXPECT_EQ(carrySortForward(captured, source), captured);
}

// A sort that the captured layout does have is the newer one, and wins.
TEST(ProcessTableSettingsTest, AnExistingSortIsNotOverwritten)
{
    const std::string_view source = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60 Sort=0^\nColumn 1  Width=60\n";
    const std::string_view captured = "[Table][0x1A2B3C4D,2]\nColumn 0  Width=60 Sort=0v\nColumn 1  Width=60\n";
    EXPECT_EQ(carrySortForward(captured, source), captured);
}

// The reviewed defect: ImGui writes a header-only section when nothing but the masked-out sort
// differs from the defaults. The sorted column then has no line to receive the sort, so one is added.
TEST(ProcessTableSettingsTest, SortIsCarriedIntoAHeaderOnlyCapture)
{
    const std::string_view source = "[Table][0x1A2B3C4D,3]\nColumn 0  Width=60 Order=2\nColumn 1  Width=120 Sort=0^\n";
    const std::string_view headerOnly = "[Table][0x1A2B3C4D,3]\n";
    EXPECT_EQ(carrySortForward(headerOnly, source), "[Table][0x1A2B3C4D,3]\nColumn 1  Sort=0^\n");
}

// The same when the capture has lines, just not for the sorted column.
TEST(ProcessTableSettingsTest, SortIsAddedForASortedColumnTheCaptureHasNoLineFor)
{
    const std::string_view source = "[Table][0x1A2B3C4D,3]\nColumn 2  Width=90 Sort=0v\n";
    const std::string_view captured = "[Table][0x1A2B3C4D,3]\nRefScale=11\nColumn 0  Width=60\n";
    EXPECT_EQ(carrySortForward(captured, source), "[Table][0x1A2B3C4D,3]\nRefScale=11\nColumn 0  Width=60\nColumn 2  Sort=0v\n");
}

// A sort only means something for the table it was saved from.
TEST(ProcessTableSettingsTest, SortIsNotCarriedFromADifferentTable)
{
    const std::string_view source = "[Table][0x0BADF00D,3]\nColumn 1  Width=120 Sort=0^\n";
    const std::string_view headerOnly = "[Table][0x1A2B3C4D,3]\n";
    EXPECT_EQ(carrySortForward(headerOnly, source), headerOnly);
}

TEST(ProcessTableSettingsTest, CarrySortForwardWithNothingToCarryReturnsTheSanitisedCapture)
{
    EXPECT_EQ(carrySortForward(SECTION, ""), SECTION);
    EXPECT_EQ(carrySortForward(SECTION, "not a layout"), SECTION);
    EXPECT_EQ(carrySortForward(SECTION, "[Table][0x1A2B3C4D,3]\nColumn 0  Width=60\n"), SECTION);
    EXPECT_EQ(carrySortForward("", SECTION), "");
    EXPECT_EQ(carrySortForward("garbage", SECTION), "");
}

// ========== withExplicitOrder (#1393) ==========

using ProcessTableSettings::withExplicitOrder;

// The reported layout: what ImGui saves for the Processes table after a session with nothing
// resized or moved, sorted by CPU % descending. ImGui wrote no order, so the default order is
// written out for every column, the sorted column included, and the sort is kept.
TEST(ProcessTableSettingsTest, SortOnlySectionGetsTheDefaultOrderForEveryColumn)
{
    const std::string_view saved = "[Table][0xA7EC3B99,32]\n"
                                   "RefScale=11\n"
                                   "Column 8  Sort=0^ ID=0x9E4DF042\n";
    const std::string filled = withExplicitOrder(saved);

    std::string expected = "[Table][0xA7EC3B99,32]\n"
                           "RefScale=11\n"
                           "Column 8  Order=8 Sort=0^ ID=0x9E4DF042\n";
    for (int index = 0; index < 32; ++index)
    {
        if (index != 8)
        {
            expected += std::format("Column {:<2} Order={}\n", index, index);
        }
    }
    EXPECT_EQ(filled, expected);
    // And it is a section sanitize() keeps as it is on the next launch.
    EXPECT_EQ(sanitize(filled), filled);
}

// A section the user has reordered already carries an order for every column, and it is theirs.
TEST(ProcessTableSettingsTest, ARealColumnOrderIsLeftAlone)
{
    EXPECT_EQ(withExplicitOrder(SECTION), SECTION);

    const std::string_view reordered = "[Table][0x1A2B3C4D,3]\n"
                                       "RefScale=11\n"
                                       "Column 0  Width=60 Order=2 ID=0x00000001\n"
                                       "Column 1  Width=120 Order=0 Sort=0v ID=0x00000002\n"
                                       "Column 2  Weight=1.0000 Order=1 ID=0x00000003\n";
    EXPECT_EQ(withExplicitOrder(reordered), reordered);
}

// A resized table has a line for every column, with its width; they gain an order and keep the rest.
TEST(ProcessTableSettingsTest, WidthsAndSortSurviveTheFill)
{
    const std::string_view resized = "[Table][0x1A2B3C4D,3]\n"
                                     "RefScale=13\n"
                                     "Column 0  Width=60 ID=0x00000001\n"
                                     "Column 1  Width=240 Sort=0^ ID=0x00000002\n"
                                     "Column 2  Weight=1.0000 ID=0x00000003\n";
    EXPECT_EQ(withExplicitOrder(resized),
              "[Table][0x1A2B3C4D,3]\n"
              "RefScale=13\n"
              "Column 0  Width=60 Order=0 ID=0x00000001\n"
              "Column 1  Width=240 Order=1 Sort=0^ ID=0x00000002\n"
              "Column 2  Weight=1.0000 Order=2 ID=0x00000003\n");
}

// Hiding a column makes ImGui write every column with "Visible=". Visibility is stripped (it lives in
// [process_columns]), and the lines that are left get the default order.
TEST(ProcessTableSettingsTest, AHiddenColumnSectionGetsTheDefaultOrderWithoutVisibility)
{
    const std::string_view hidden = "[Table][0x1A2B3C4D,3]\n"
                                    "Column 0  Visible=1 ID=0x00000001\n"
                                    "Column 1  Visible=0 Sort=0v ID=0x00000002\n"
                                    "Column 2  Visible=1 ID=0x00000003\n";
    EXPECT_EQ(withExplicitOrder(hidden),
              "[Table][0x1A2B3C4D,3]\n"
              "Column 0  Order=0 ID=0x00000001\n"
              "Column 1  Order=1 Sort=0v ID=0x00000002\n"
              "Column 2  Order=2 ID=0x00000003\n");

    // Hidden and moved: the moved order is kept.
    const std::string_view hiddenAndMoved = "[Table][0x1A2B3C4D,2]\n"
                                            "Column 0  Visible=0 Order=1\n"
                                            "Column 1  Visible=1 Order=0\n";
    EXPECT_EQ(withExplicitOrder(hiddenAndMoved), "[Table][0x1A2B3C4D,2]\nColumn 0  Order=1\nColumn 1  Order=0\n");
}

// A layout saved before a column was added has fewer columns than the table. Only its own columns
// get an order; ImGui places the new one after them.
TEST(ProcessTableSettingsTest, AnOlderLayoutWithFewerColumnsIsFilledToItsOwnCount)
{
    EXPECT_EQ(withExplicitOrder("[Table][0x1A2B3C4D,3]\nColumn 2  Sort=0^\n"),
              "[Table][0x1A2B3C4D,3]\nColumn 2  Order=2 Sort=0^\nColumn 0  Order=0\nColumn 1  Order=1\n");
}

// A header-only section (nothing differs from the defaults) is given the default order too, so a
// sort carried into it later still loads in the right order.
TEST(ProcessTableSettingsTest, AHeaderOnlySectionGetsTheDefaultOrder)
{
    EXPECT_EQ(withExplicitOrder("[Table][0x1A2B3C4D,2]\n"), "[Table][0x1A2B3C4D,2]\nColumn 0  Order=0\nColumn 1  Order=1\n");
    EXPECT_EQ(withExplicitOrder(carrySortForward("[Table][0x1A2B3C4D,2]\n", "[Table][0x1A2B3C4D,2]\nColumn 1  Sort=0^\n")),
              "[Table][0x1A2B3C4D,2]\nColumn 1  Order=1 Sort=0^\nColumn 0  Order=0\n");
}

// A line past the column count is ignored by ImGui; it is not counted as covering a column.
TEST(ProcessTableSettingsTest, ALinePastTheColumnCountIsNotGivenAnOrder)
{
    EXPECT_EQ(withExplicitOrder("[Table][0x1A2B3C4D,1]\nColumn 5  Sort=0^\n"),
              "[Table][0x1A2B3C4D,1]\nColumn 5  Sort=0^\nColumn 0  Order=0\n");
}

TEST(ProcessTableSettingsTest, WithExplicitOrderIsIdempotentAndRejectsWhatSanitizeRejects)
{
    const std::string once = withExplicitOrder("[Table][0xA7EC3B99,32]\nRefScale=11\nColumn 8  Sort=0^ ID=0x9E4DF042\n");
    EXPECT_EQ(withExplicitOrder(once), once);
    EXPECT_EQ(withExplicitOrder(""), "");
    EXPECT_EQ(withExplicitOrder("garbage"), "");
    EXPECT_EQ(withExplicitOrder("[Window][Main]\nPos=0,0\n"), "");
}

// A column count too large to fill within the stored limits is returned unfilled, so the next launch
// does not reject the whole layout.
TEST(ProcessTableSettingsTest, ASectionTooLargeToFillIsReturnedUnfilled)
{
    const std::string_view huge = "[Table][0x1A2B3C4D,500]\nColumn 0  Sort=0^\n";
    EXPECT_EQ(withExplicitOrder(huge), huge);
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

// ========== withColumnWidth (#1209) ==========

// Closing in tree view: the Name column's list width is saved in place of tree view's automatic one.
TEST(ProcessTableSettingsTest, WithColumnWidthReplacesTheColumnsWidth)
{
    EXPECT_EQ(ProcessTableSettings::withColumnWidth(SECTION, 1, 120),
              "[Table][0x1A2B3C4D,3]\n"
              "RefScale=13\n"
              "Column 0  Width=60 Order=1 ID=0x00000000\n"
              "Column 1  Width=120 Order=0 Sort=0^\n"
              "Column 2  Weight=1.0000 Order=2\n");
}

TEST(ProcessTableSettingsTest, WithColumnWidthAddsALineForAColumnWithout)
{
    const std::string_view noNameLine = "[Table][0x1A2B3C4D,3]\n"
                                        "Column 0  Width=60\n";
    EXPECT_EQ(ProcessTableSettings::withColumnWidth(noNameLine, 1, 120),
              "[Table][0x1A2B3C4D,3]\n"
              "Column 0  Width=60\n"
              "Column 1  Width=120\n");
    // The result is a section sanitize() keeps as it is.
    const std::string result = ProcessTableSettings::withColumnWidth(noNameLine, 1, 120);
    EXPECT_EQ(sanitize(result), result);
}

TEST(ProcessTableSettingsTest, WithColumnWidthLeavesStretchColumnsAndBadWidthsAlone)
{
    // Column 2 is the stretch column: it has a weight, not a width.
    EXPECT_EQ(ProcessTableSettings::withColumnWidth(SECTION, 2, 300), sanitize(SECTION));
    EXPECT_EQ(ProcessTableSettings::withColumnWidth(SECTION, 1, 0), sanitize(SECTION));
    EXPECT_EQ(ProcessTableSettings::withColumnWidth(SECTION, 1, 100000), sanitize(SECTION));
    EXPECT_TRUE(ProcessTableSettings::withColumnWidth("", 1, 120).empty());
}

} // namespace
} // namespace App
