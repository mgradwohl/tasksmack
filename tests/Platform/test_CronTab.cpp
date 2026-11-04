/// @file test_CronTab.cpp
/// @brief Reading cron tables for the Linux Startup tab (#3425): named and five-field schedules, the system tables'
/// user column, the lines that aren't jobs (comments, blanks, variable assignments, short or unknown lines), each
/// job's PATH and SHELL, and the program a job's shell command starts, read without running it.

#include "Platform/Linux/CronTab.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
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

// #3441 review: a job keeps the PATH (and SHELL) its table set above it, cron's default before any, and quotes
// around the value are dropped as cron drops them.
TEST(CronTabTest, EachJobKeepsThePathAndShellSetAboveIt)
{
    const std::vector<CronTab::Entry> jobs = parse("@daily first\n"
                                                   "PATH=/opt/backup/bin:/usr/bin\n"
                                                   "@daily second\n"
                                                   "SHELL = /bin/bash\n"
                                                   "PATH = \"/opt/other\" \n"
                                                   "@daily third\n",
                                                   /*withUser=*/false);
    ASSERT_EQ(jobs.size(), 3U);
    EXPECT_EQ(jobs[0].path, "/usr/bin:/bin") << "cron's default, crontab(5)";
    EXPECT_EQ(jobs[0].shell, "/bin/sh");
    EXPECT_EQ(jobs[1].path, "/opt/backup/bin:/usr/bin");
    EXPECT_EQ(jobs[1].shell, "/bin/sh");
    EXPECT_EQ(jobs[2].path, "/opt/other");
    EXPECT_EQ(jobs[2].shell, "/bin/bash");
}

/// The program CronTab::program() reads from @p command, its kind, and the inline PATH it found.
struct ProgramCase
{
    const char* command;
    CronTab::Program::Kind kind;
    const char* word;
    std::optional<std::string> path;
};

// #3441 review: a job's command is shell syntax: quotes and backslashes are removed, VAR= prefixes, env and exec are
// skipped, builtins and keywords are told apart, and what only the shell could work out is Unknown.
TEST(CronTabTest, TheProgramIsTheShellCommandsFirstWord)
{
    using Kind = CronTab::Program::Kind;
    const std::vector<ProgramCase> cases = {
        {"'/bin/sh' /home/u/backup.sh", Kind::Executable, "/bin/sh", std::nullopt},
        {R"("/opt/my tools/run" --now)", Kind::Executable, "/opt/my tools/run", std::nullopt},
        {R"(/opt/my\ tools/run)", Kind::Executable, "/opt/my tools/run", std::nullopt},
        {"backup.sh>/dev/null 2>&1", Kind::Executable, "backup.sh", std::nullopt},
        {"run;other", Kind::Executable, "run", std::nullopt},
        {"LANG=C TZ=UTC backup", Kind::Executable, "backup", std::nullopt},
        {"PATH=/opt/x:/bin backup --all", Kind::Executable, "backup", "/opt/x:/bin"},
        {"env -i PATH=/opt/y LANG=C backup", Kind::Executable, "backup", "/opt/y"},
        {"env -u HOME backup", Kind::Executable, "backup", std::nullopt},
        {"exec /usr/bin/backup", Kind::Executable, "/usr/bin/backup", std::nullopt},
        {"date +\\%Y-\\%m-\\%d > /tmp/day", Kind::Executable, "date", std::nullopt},
        {"cd / && run-parts /etc/cron.hourly", Kind::ShellBuiltin, "cd", std::nullopt},
        {"[ -x /usr/sbin/x ] && /usr/sbin/x", Kind::ShellBuiltin, "[", std::nullopt},
        {"test -x /usr/sbin/x && /usr/sbin/x", Kind::ShellBuiltin, "test", std::nullopt},
        {"if true; then x; fi", Kind::ShellBuiltin, "if", std::nullopt},
        {". /etc/profile; run", Kind::ShellBuiltin, ".", std::nullopt},
        {"$HOME/bin/run", Kind::Unknown, "", std::nullopt},
        {"PATH=$PATH:/opt/x run", Kind::Unknown, "", std::nullopt},
        {"\"$HOME/bin/run\"", Kind::Unknown, "", std::nullopt},
        {"`which run`", Kind::Unknown, "", std::nullopt},
        {"~/bin/run", Kind::Unknown, "", std::nullopt},
        {"/opt/run-*.sh", Kind::Unknown, "", std::nullopt},
        {"(cd /tmp && run)", Kind::Unknown, "", std::nullopt},
        {"> /tmp/log run", Kind::Unknown, "", std::nullopt},
        {"2>/dev/null run", Kind::Unknown, "", std::nullopt},
        {"'unterminated", Kind::Unknown, "", std::nullopt},
        {"env -S 'run --x'", Kind::Unknown, "", std::nullopt},
        {"%stdin only", Kind::Unknown, "", std::nullopt},
    };
    for (const ProgramCase& c : cases)
    {
        const CronTab::Program found = CronTab::program(c.command);
        EXPECT_EQ(found.kind, c.kind) << c.command;
        EXPECT_EQ(found.word, c.word) << c.command;
        EXPECT_EQ(found.path, c.path) << c.command;
    }
}

} // namespace
} // namespace Platform
