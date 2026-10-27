// The Processes row menu's "Open file location" and "Search online" (#3064): the search URL and the
// result lines.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessLookup.h"
#include "Platform/IProcessActions.h"
#include "Platform/UriEncoding.h"

#include <gtest/gtest.h>

#include <string>

namespace App
{
namespace
{

using Platform::ProcessActionResult;

TEST(ProcessLookupTest, SearchOnlineSendsTheNameOnlyPercentEncoded)
{
    EXPECT_EQ(Detail::searchOnlineUrl("bash"), "https://duckduckgo.com/?q=bash%20process");
    EXPECT_EQ(Detail::searchOnlineUrl("Web Content"), "https://duckduckgo.com/?q=Web%20Content%20process");
    // '&', '#' and '+' would end or change the query unencoded.
    EXPECT_EQ(Detail::searchOnlineUrl("a&b#c+d"), "https://duckduckgo.com/?q=a%26b%23c%2Bd%20process");
    EXPECT_EQ(Detail::searchOnlineUrl("z\xC3\xBC"), "https://duckduckgo.com/?q=z%C3%BC%20process");
}

TEST(ProcessLookupTest, PercentEncodeKeepsUnreservedAndAskedForBytes)
{
    EXPECT_EQ(Platform::percentEncode("AZaz09-._~"), "AZaz09-._~");
    EXPECT_EQ(Platform::percentEncode("/a b/"), "%2Fa%20b%2F");
    EXPECT_EQ(Platform::percentEncode("/a b/", "/"), "/a%20b/");
    EXPECT_EQ(Platform::percentEncode(std::string("\x00\x7F\x80", 3)), "%00%7F%80");
    EXPECT_EQ(Platform::percentEncode(""), "");
}

TEST(ProcessLookupTest, OpenFileLocationResultLines)
{
    const Detail::ActionResultMessage opened = Detail::formatOpenFileLocationResult("vim", 321, ProcessActionResult::ok());
    EXPECT_TRUE(opened.ok);
    EXPECT_EQ(opened.text, "Opened the file location of vim (PID 321)");

    const Detail::ActionResultMessage noted =
        Detail::formatOpenFileLocationResult("vim", 321, ProcessActionResult::ok("its file has since been deleted or replaced"));
    EXPECT_EQ(noted.text, "Opened the file location of vim (PID 321) (its file has since been deleted or replaced)");

    const Detail::ActionResultMessage failed =
        Detail::formatOpenFileLocationResult("kworker/0:1", 12, ProcessActionResult::error("It has no executable file (a kernel thread)"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.text, "Could not open the file location of kworker/0:1 (PID 12): It has no executable file (a kernel thread)");
}

TEST(ProcessLookupTest, SearchOnlineResultLines)
{
    EXPECT_EQ(Detail::formatSearchOnlineResult("bash", ProcessActionResult::ok()).text, "Searching online for bash");
    const Detail::ActionResultMessage failed =
        Detail::formatSearchOnlineResult("bash", ProcessActionResult::error("no web browser could be started"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.text, "Could not search online for bash: no web browser could be started");
}

} // namespace
} // namespace App
