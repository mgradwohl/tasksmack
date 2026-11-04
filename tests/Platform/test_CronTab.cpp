/// @file test_CronTab.cpp
/// @brief Reading cron tables for the Linux Startup tab (#3425): named and five-field schedules, the system tables'
/// user column, and the lines that aren't jobs (comments, blanks, variable assignments, short or unknown lines).

#include "Platform/Linux/CronTab.h"

#include <gtest/gtest.h>

#include <vector>

namespace Platform
{
namespace
{

using CronTab::parse;

TEST(CronTabTest, AUsersCrontabsJobsWithTheirScheduleCommandAndLine)
{
    const std::vector<CronTab::Entry> jobs = parse("# m h dom mon dow command\n"
                                                   "MAILTO=me@example.com\n"
                                                   "PATH = /usr/bin:/bin\n"
                                                   "\n"
                                                   "@reboot /home/me/bin/start-sync --quiet\n"
                                                   "*/15 * * * *  /usr/bin/backup.sh > /dev/null 2>&1\n"
                                                   "\t30 2 * * 1-5 cd /srv && make nightly\r\n",
                                                   /*withUser=*/false);
    ASSERT_EQ(jobs.size(), 3U);
    EXPECT_EQ(jobs[0].schedule, "@reboot");
    EXPECT_EQ(jobs[0].command, "/home/me/bin/start-sync --quiet");
    EXPECT_EQ(jobs[0].line, 5U);
    EXPECT_TRUE(jobs[0].user.empty());
    EXPECT_EQ(jobs[1].schedule, "*/15 * * * *");
    EXPECT_EQ(jobs[1].command, "/usr/bin/backup.sh > /dev/null 2>&1");
    EXPECT_EQ(jobs[2].schedule, "30 2 * * 1-5"); // a leading tab and a CRLF ending are cron's too
    EXPECT_EQ(jobs[2].command, "cd /srv && make nightly");
    EXPECT_EQ(jobs[2].line, 7U);
}

TEST(CronTabTest, TheSystemTablesNameTheUser)
{
    const std::vector<CronTab::Entry> jobs = parse("SHELL=/bin/sh\n"
                                                   "17 *\t* * *\troot\tcd / && run-parts --report /etc/cron.hourly\n"
                                                   "@reboot root /usr/local/sbin/firstboot\n"
                                                   "@daily\n"          // no user, no command
                                                   "5 4 * * * root\n", // a user but no command
                                                   /*withUser=*/true);
    ASSERT_EQ(jobs.size(), 2U);
    EXPECT_EQ(jobs[0].schedule, "17 * * * *");
    EXPECT_EQ(jobs[0].user, "root");
    EXPECT_EQ(jobs[0].command, "cd / && run-parts --report /etc/cron.hourly");
    EXPECT_EQ(jobs[1].schedule, "@reboot");
    EXPECT_EQ(jobs[1].user, "root");
}

TEST(CronTabTest, LinesThatArentJobsAreSkipped)
{
    EXPECT_TRUE(parse("", false).empty());
    EXPECT_TRUE(parse("# only a comment\n   \n", false).empty());
    EXPECT_TRUE(parse("@everyminute /bin/true\n", false).empty()); // not a cron schedule name
    EXPECT_TRUE(parse("1 2 3\n", false).empty());                  // too short for five fields
    EXPECT_TRUE(parse("1 2 3 4 5\n", false).empty());              // a schedule with no command
    EXPECT_EQ(parse("@hourly /bin/true", false).size(), 1U);       // no trailing newline
}

TEST(CronTabTest, TheScheduleIsWordedForTheStartupTab)
{
    EXPECT_EQ(CronTab::describeSchedule("@reboot"), "at startup");
    EXPECT_EQ(CronTab::describeSchedule("@daily"), "@daily");
    EXPECT_EQ(CronTab::describeSchedule("*/5 * * * *"), "*/5 * * * *");
}

} // namespace
} // namespace Platform
