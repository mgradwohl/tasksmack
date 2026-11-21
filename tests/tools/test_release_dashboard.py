#!/usr/bin/env python3
"""Tests for tools/release-dashboard.py (#4524): the Release Command Center's data.

Everything the generator works out from what the GitHub API returned is tested here without the network, on a
small made-up repository: three milestones (one shipped, one in its freeze, one planned), built by the
helpers below in the API's own shapes.

The same repository is the page's test data. tests/tools/fixtures/release-dashboard/ holds what the generator
writes for it, tools/docs-site-smoke.py serves those files to docs/release/ in a browser, and
FixtureTests fails when the generator's output no longer matches them, so the page is always tested against
the schema the generator really writes. After a deliberate change, rewrite them:

    python -X utf8 tests/tools/test_release_dashboard.py --write-fixtures
"""

import contextlib
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from datetime import UTC, datetime, timedelta
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release-dashboard.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "release-dashboard"
spec = importlib.util.spec_from_file_location("release_dashboard", SCRIPT)
dashboard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dashboard)

REPOSITORY = "mgradwohl/tasksmack"


def at(day: int, hour: int = 0) -> str:
    return f"2026-01-{day:02d}T{hour:02d}:00:00Z"


def when(day: int, hour: int = 0):
    return dashboard.parse_time(at(day, hour))


NOW = when(20, 12)


def pull(
    number: int,
    created: str,
    merged: str | None = None,
    *,
    state: str | None = None,
    draft: bool = False,
    lines: int | None = 100,
    ready: str | None = None,
) -> dict:
    node = {
        "number": number,
        "state": state or ("MERGED" if merged else "OPEN"),
        "isDraft": draft,
        "createdAt": created,
        "mergedAt": merged,
        "closedAt": merged,
    }
    if lines is not None:
        node.update(additions=lines - 10, deletions=10)
    if ready is not None:
        node["ready"] = {"nodes": [{"createdAt": ready}]}
    return node


def issue(
    number: int,
    title: str,
    *,
    labels: tuple[str, ...] = (),
    created: str,
    closed: str | None = None,
    reason: str = "COMPLETED",
    milestoned: tuple[tuple[str, str], ...] = (),
    labeled: tuple[tuple[str, str, str], ...] = (),
    pulls: tuple[dict, ...] = (),
    reopened: int = 0,
) -> dict:
    return {
        "number": number,
        "title": title,
        "state": "CLOSED" if closed else "OPEN",
        "stateReason": reason if closed else None,
        "createdAt": created,
        "closedAt": closed,
        "labels": {"nodes": [{"name": name} for name in labels]},
        "milestoned": {"nodes": [{"createdAt": moment, "milestoneTitle": title} for moment, title in milestoned]},
        "labeled": {
            "nodes": [
                {"__typename": kind, "createdAt": moment, "label": {"name": name}} for kind, moment, name in labeled
            ]
        },
        "reopened": {"totalCount": reopened},
        "closedByPullRequestsReferences": {"nodes": list(pulls)},
    }


def milestone(number: int, title: str, created: str, closed: str | None = None, due: str | None = None) -> dict:
    return {
        "number": number,
        "title": title,
        "state": "closed" if closed else "open",
        "created_at": created,
        "closed_at": closed,
        "due_on": due,
    }


SHIPPED = milestone(1, "v1.0.0", at(1), closed=at(10, 13))
CURRENT = milestone(2, "v1.1.0", at(5), due="2026-02-01T08:00:00Z")
PLANNED = milestone(3, "v1.2.0", at(12))
RELEASES = [{"tag_name": "v1.0.0", "draft": False, "prerelease": False, "published_at": at(10, 12)}]

TRACKER_BODY = """## v1.1.0 release tracker

Read this first. Not a task: - [ ] in the middle of a line.

### Stage 1 · **Systems**
- [x] #105 CI timeouts with headroom
- [ ] #101 Fix the crash on start
- [ ] A signed release dry run on main

### Stage 2 · Features
- [ ] #104 Parity for the thing (see #103)
- [ ] Docs pass (#107)
* [ ] **#999** Something on another milestone

```
- [ ] #101 inside a code block, so not an item
```
"""

READINESS = """**Windows lane: features complete for v1.1.0.**
This is a readiness report, not a freeze declaration; declaring the feature freeze is the maintainer's call."""

TRACKER_COMMENTS = [
    {
        "body": READINESS,
        "authorAssociation": "OWNER",
        "createdAt": at(17),
        "url": f"https://github.com/{REPOSITORY}/issues/100#issuecomment-1",
    },
    {
        "body": "Approved for v1.1.0 release",
        "authorAssociation": "OWNER",
        "createdAt": at(17, 1),
        "url": "https://example.invalid/early",
    },
    {
        "body": "I declare the feature freeze",
        "authorAssociation": "OWNER",
        "createdAt": at(18, 9),
        "url": f"https://github.com/{REPOSITORY}/issues/100#issuecomment-2",
    },
    {
        "body": "Approved",
        "authorAssociation": "OWNER",
        "createdAt": at(18, 10),
        "url": f"https://github.com/{REPOSITORY}/issues/100#issuecomment-3",
    },
]

HOSTILE_TITLE = '[Testing] <img src=x onerror="window.__xss=1"> & "quotes" </script><b>bold</b>'

CURRENT_ISSUES = [
    issue(100, "[RELEASE 1.1] v1.1.0 release tracker", labels=("release",), created=at(5)),
    issue(
        101,
        "[BUG] Crash on start",
        labels=("bug", "sev:high", "pri:high", "agent:Windows"),
        created=at(11),
        closed=at(11, 6),
        milestoned=((at(11), "v1.1.0"),),
        pulls=(pull(201, at(11, 2), at(11, 6), lines=40),),
        reopened=1,
    ),
    issue(
        102,
        "[BUG] Wrong total in the footer",
        labels=("bug", "sev:medium", "pri:medium", "agent:WSL"),
        created=at(19),
        pulls=(pull(202, at(20, 2)),),
    ),
    issue(
        103,
        "[FEATURE] A new column",
        labels=("enhancement", "pri:medium", "agent:Windows"),
        created=at(6),
        closed=at(15),
        pulls=(pull(211, at(13), at(15), lines=600),),
    ),
    issue(
        104,
        "[FEATURE] Parity for the thing",
        labels=("enhancement", "parity", "agent:WSL", "in progress"),
        created=at(12),
        labeled=(("LabeledEvent", at(19, 6), "in progress"), ("LabeledEvent", at(12), "parity")),
    ),
    issue(
        105, "[CI] Timeouts with headroom", labels=("ci", "enhancement", "pri:high", "agent:Windows"), created=at(18)
    ),
    issue(
        106,
        "[BUG] A misaligned label",
        labels=("bug", "code-review", "sev:low", "pri:low", "agent:WSL"),
        created=at(19, 18),
    ),
    issue(
        107,
        "[Docs] An idea that was dropped",
        labels=("documentation",),
        created=at(13),
        closed=at(14),
        reason="NOT_PLANNED",
    ),
    issue(108, "[Testing] Check on real hardware", labels=("testing", "manual close", "agent:Windows"), created=at(16)),
    issue(
        109,
        HOSTILE_TITLE,
        labels=("testing", "agent:WSL", "agent:Windows"),
        created=at(17),
        labeled=(("LabeledEvent", at(17, 1), "in progress"), ("UnlabeledEvent", at(17, 2), "in progress")),
    ),
    issue(
        110,
        "[Refactor] One owner for the cache",
        labels=("refactoring", "agent:Windows"),
        created=at(2),
        closed=at(20, 6),
        milestoned=((at(2), "v1.0.0"), (at(12), "v1.1.0")),
        pulls=(pull(212, at(19), at(20, 6), lines=None),),
    ),
]

RED_ISSUES = [
    {"number": 300, "title": "CI: main is red", "created_at": at(12), "closed_at": at(12, 6)},
    {"number": 301, "title": "CI: main is red", "created_at": at(20), "closed_at": at(20, 2)},
    {"number": 302, "title": "CI: release/1.0 is red", "created_at": at(15), "closed_at": at(16)},
    {"number": 303, "title": "CI: main is red", "created_at": at(14), "closed_at": at(15), "pull_request": {}},
    {"number": 304, "title": "main is on fire", "created_at": at(14), "closed_at": at(15)},
]


def run(
    run_id: int,
    conclusion: str | None,
    updated: str,
    *,
    status: str = "completed",
    sha: str = "a" * 40,
    event: str = "push",
) -> dict:
    return {
        "id": run_id,
        "status": status,
        "conclusion": conclusion,
        "event": event,
        "sha": sha,
        "created_at": updated,
        "updated_at": updated,
    }


def open_pull(
    number: int, title: str, *, draft: bool, checks: str, closes: tuple[int, ...], labels=(), on=None, bot=False
):
    return {
        "number": number,
        "title": title,
        "isDraft": draft,
        "createdAt": at(20, 2),
        "baseRefName": "main",
        "author": {"__typename": "Bot" if bot else "User", "login": "someone"},
        "milestone": {"title": on} if on else None,
        "labels": {"nodes": [{"name": name} for name in labels]},
        "mergeStateStatus": "CLEAN",
        "autoMergeRequest": {"enabledAt": at(20, 3)} if not draft else None,
        "closingIssuesReferences": {"nodes": [{"number": closing} for closing in closes]},
        "reviewThreads": {"nodes": [{"isResolved": True}, {"isResolved": False}]},
        "ready": {"nodes": [] if draft else [{"createdAt": at(20, 3)}]},
        "commits": {"nodes": [{"commit": {"statusCheckRollup": {"state": checks}}}]},
    }


LIVE = {
    "branch": "main",
    "open_pull_requests": [
        open_pull(
            202, "fix: the footer total", draft=False, checks="SUCCESS", closes=(102,), labels=("bug", "agent:WSL")
        ),
        open_pull(250, "chore: bump a pin", draft=True, checks="FAILURE", closes=(400,), on="v1.2.0", bot=True),
    ],
    "latest_runs": [
        # CI's nightly run failed and a later push run passed: main-health.yml keeps the branch red for that.
        {
            "file": "ci.yml",
            "name": "CI",
            "runs": [run(900, "success", at(20, 11)), run(899, "failure", at(20, 4), event="schedule")],
        },
        {"file": "codeql.yml", "name": "CodeQL Security Analysis", "runs": [run(901, "neutral", at(20, 10))]},
        {"file": "arm64.yml", "name": "ARM64", "runs": []},
    ],
    "dry_runs": [
        run(910, None, at(20, 11), status="in_progress"),
        run(909, "cancelled", at(19, 12)),
        run(908, "success", at(19), sha="b" * 40),
    ],
    "dry_run_behind": 3,
}

CI_COUNTS = [
    {"file": "ci.yml", "name": "CI", "success": 90, "failure": 10},
    {"file": "codeql.yml", "name": "CodeQL Security Analysis", "success": 100, "failure": 0},
]

RAW = {
    "v1.0.0": {
        "milestone": SHIPPED,
        "issues": [
            issue(
                50,
                "[BUG] The first bug",
                labels=("bug", "sev:low"),
                created=at(2),
                closed=at(4),
                pulls=(pull(60, at(3), at(4)),),
            ),
        ],
        "pull_requests": [pull(61, at(5), at(5, 12))],
        "tracker": None,
        "states": {},
        "ci_counts": [{"file": "ci.yml", "name": "CI", "success": 40, "failure": 0}],
    },
    "v1.1.0": {
        "milestone": CURRENT,
        "issues": CURRENT_ISSUES,
        "pull_requests": [
            pull(201, at(11, 2), at(11, 6), lines=40, ready=at(11, 3)),
            pull(220, at(14), at(14, 1), lines=200),
            pull(221, at(16), state="CLOSED"),
        ],
        "pr_ci": {"success": 30, "failure": 10, "merged": 8},
        # The linked pull requests that aren't on the milestone: #211 was a draft for a day, #212 never was one.
        "pull_request_ready": {"211": at(14), "212": None},
        "tracker": {
            "number": 100,
            "title": "[RELEASE 1.1] v1.1.0 release tracker",
            "body": TRACKER_BODY,
            "comments": TRACKER_COMMENTS,
        },
        "states": {"999": {"state": "CLOSED", "title": "Elsewhere", "type": "Issue"}},
        "ci_counts": CI_COUNTS,
    },
    "v1.2.0": {
        "milestone": PLANNED,
        "issues": [
            issue(400, "[FEATURE] Later", labels=("enhancement", "agent:Dash", "in progress"), created=at(12)),
            issue(401, "[Refactor] Later too", labels=("refactoring",), created=at(13)),
        ],
        "pull_requests": [],
        "tracker": None,
        "states": {},
        "ci_counts": [],
    },
}
REPOSITORY_RAW = {
    "collected_at": dashboard.iso(NOW),
    "watched": [["ci.yml", "CI"], ["codeql.yml", "CodeQL Security Analysis"]],
    "milestones": [CURRENT, SHIPPED, PLANNED, {**SHIPPED, "number": 9, "title": "Backlog"}],
    "releases": RELEASES,
    "red_issues": RED_ISSUES,
    "live": {"main": LIVE},
}


def published() -> dict:
    return dashboard.published_releases(RELEASES)


def build(title: str, *, now=NOW) -> dict:
    pending = {(1, 1, 0), (1, 2, 0)}
    return dashboard.build_release(
        RAW[title], published(), RED_ISSUES, RAW[title]["ci_counts"], LIVE, REPOSITORY, now, pending
    )


def rows(release: dict) -> dict:
    return {row["id"]: row for row in release["ship_list"]}


class WatchedWorkflowsTests(unittest.TestCase):
    def test_reads_the_real_main_health_workflow(self):
        watched = dashboard.watched_workflows((REPO / dashboard.MAIN_HEALTH).read_text(encoding="utf-8"))
        self.assertIn(("ci.yml", "CI"), watched)
        self.assertIn(("codeql.yml", "CodeQL Security Analysis"), watched)
        self.assertGreaterEqual(len(watched), 5)
        for file, name in watched:
            self.assertTrue((REPO / ".github" / "workflows" / file).is_file(), file)
            self.assertTrue(name)

    def test_stops_at_the_first_line_that_is_not_an_entry(self):
        text = "env:\n  WATCHED: |-\n    ci.yml|CI\n    a-b.yaml|Two Words\n  # next\n    later.yml|Not this\n"
        self.assertEqual(dashboard.watched_workflows(text), [("ci.yml", "CI"), ("a-b.yaml", "Two Words")])

    def test_no_list(self):
        self.assertEqual(dashboard.watched_workflows("name: Main Health\n"), [])


class VersionTests(unittest.TestCase):
    def test_milestone_titles(self):
        self.assertEqual(dashboard.version_of("v0.11.0"), (0, 11, 0))
        for title in ("0.11.0", "v0.11", "v0.11.0-rc1", "Backlog", "v01.2.3", ""):
            self.assertIsNone(dashboard.version_of(title), title)

    def test_a_minor_is_built_on_main_and_a_patch_on_its_branch(self):
        self.assertEqual(dashboard.branch_of((0, 11, 0)), "main")
        self.assertEqual(dashboard.branch_of((0, 9, 1)), "release/0.9")

    def test_published_releases_skip_drafts_and_other_tags(self):
        found = dashboard.published_releases(
            [
                {"tag_name": "v1.0.0", "draft": False, "published_at": at(1)},
                {"tag_name": "v1.1.0", "draft": True, "published_at": None},
                {"tag_name": "nightly", "draft": False, "published_at": at(2)},
            ]
        )
        self.assertEqual(list(found), [(1, 0, 0)])


class CycleWindowTests(unittest.TestCase):
    RELEASED = {
        (0, 9, 0): {"tag_name": "v0.9.0", "published_at": at(2)},
        (0, 9, 1): {"tag_name": "v0.9.1", "published_at": at(6)},
        (0, 10, 0): {"tag_name": "v0.10.0", "published_at": at(9)},
    }

    def window(self, title, pending=()):
        return dashboard.cycle_window(milestone(1, title, at(1)), self.RELEASED, pending)

    def test_a_minor_follows_the_minor_before_it_not_a_patch(self):
        window = self.window("v0.10.0")
        self.assertEqual(window["start"], when(2))
        self.assertEqual(window["start_reason"], "v0.9.0 was published")
        self.assertEqual(window["end"], when(9))
        self.assertEqual(window["release_tag"], "v0.10.0")

    def test_a_patch_follows_the_release_before_it_on_its_line(self):
        window = self.window("v0.9.1")
        self.assertEqual((window["start"], window["end"]), (when(2), when(6)))

    def test_an_unreleased_minor_is_under_way(self):
        window = self.window("v0.11.0", pending={(0, 11, 0), (0, 12, 0)})
        self.assertEqual(window["start"], when(9))
        self.assertIsNone(window["end"])

    def test_a_release_behind_an_unreleased_one_is_planned(self):
        window = self.window("v0.12.0", pending={(0, 11, 0), (0, 12, 0)})
        self.assertIsNone(window["start"])
        self.assertEqual(window["start_reason"], "it starts when v0.11.0 is published")
        patch = self.window("v0.11.1", pending={(0, 11, 0), (0, 11, 1)})
        self.assertIsNone(patch["start"])

    def test_the_first_release_starts_when_its_milestone_opened(self):
        window = dashboard.cycle_window(milestone(1, "v0.1.0", at(3)), {}, set())
        self.assertEqual(window["start"], when(3))
        self.assertEqual(window["start_reason"], "the milestone was opened")


class LabelTests(unittest.TestCase):
    def test_category_follows_the_queue_order(self):
        cases = [
            ({"bug", "security", "ci"}, "Security"),
            ({"bug", "ci", "testing"}, "Bug"),
            # Severity beats kind (docs/dev/labels.md): a sev:high or sev:medium item is taken with the bugs.
            ({"ci", "sev:medium"}, "Bug"),
            ({"testing", "sev:high"}, "Bug"),
            ({"testing", "sev:low"}, "Testing"),
            ({"security", "sev:high"}, "Security"),
            ({"enhancement", "ci", "release"}, "CI"),
            ({"enhancement", "release"}, "Tooling"),
            ({"dependencies"}, "Tooling"),
            ({"enhancement", "performance"}, "Perf"),
            ({"testing", "documentation"}, "Testing"),
            ({"documentation", "refactoring"}, "Docs"),
            ({"refactoring", "enhancement"}, "Refactor"),
            ({"enhancement", "parity"}, "Parity"),
            ({"enhancement", "windows"}, "Feature"),
            ({"parity"}, "Other"),
            (set(), "Other"),
        ]
        for labels, expected in cases:
            self.assertEqual(dashboard.category(labels), expected, labels)

    def test_lane(self):
        self.assertEqual(dashboard.lane({"agent:Windows", "bug"}), "Windows")
        self.assertEqual(dashboard.lane({"agent:WSL"}), "WSL")
        self.assertEqual(dashboard.lane({"agent:WSL", "agent:Windows"}), "Shared")
        self.assertEqual(dashboard.lane({"agent:Dash"}), "Dash")
        self.assertEqual(dashboard.lane({"bug"}), "None")

    def test_level(self):
        self.assertEqual(dashboard.level({"sev:high", "pri:low"}, "sev"), "high")
        self.assertEqual(dashboard.level({"sev:high", "pri:low"}, "pri"), "low")
        self.assertIsNone(dashboard.level({"bug"}, "sev"))


class ReadIssueTests(unittest.TestCase):
    def read(self, number):
        node = next(node for node in CURRENT_ISSUES if node["number"] == number)
        return dashboard.read_issue(node, "v1.1.0", 100)

    def test_closed_by_a_merged_pull_request(self):
        found = self.read(101)
        self.assertFalse(found["open"])
        self.assertTrue(found["completed"])
        self.assertEqual(found["phase"], "done")
        self.assertEqual((found["joined"], found["closed"], found["done"]), (when(11), when(11, 6), when(11, 6)))
        self.assertEqual(
            (found["started_pr"], found["started_label"], found["started"]), (when(11, 2), None, when(11, 2))
        )
        self.assertEqual(
            (found["category"], found["lane"], found["sev"], found["pri"]), ("Bug", "Windows", "high", "high")
        )

    def test_pull_request_size_and_reopening(self):
        found = self.read(101)
        self.assertTrue(found["reopened"])
        self.assertEqual(found["pull_requests"][0]["lines"], 40)
        self.assertFalse(self.read(102)["reopened"])
        self.assertIsNone(self.read(110)["pull_requests"][0]["lines"])
        bare = {key: value for key, value in CURRENT_ISSUES[1].items() if key != "reopened"}
        self.assertFalse(dashboard.read_issue(bare, "v1.1.0", 100)["reopened"])  # an older dump has no such field

    def test_an_open_ready_pull_request_is_review(self):
        self.assertEqual(self.read(102)["phase"], "review")

    def test_a_draft_pull_request_is_progress(self):
        node = issue(1, "x", created=at(1), pulls=(pull(2, at(2), draft=True),))
        self.assertEqual(dashboard.read_issue(node, "v1.1.0", None)["phase"], "progress")

    def test_the_label_starts_an_issue_at_the_time_it_was_added(self):
        found = self.read(104)
        self.assertEqual(found["phase"], "progress")
        self.assertEqual(
            (found["started_label"], found["started_pr"], found["started"]), (when(19, 6), None, when(19, 6))
        )

    def test_a_label_that_was_taken_off_again_starts_nothing(self):
        found = self.read(109)
        self.assertEqual(found["phase"], "todo")
        self.assertIsNone(found["started"])
        self.assertEqual(found["lane"], "Shared")

    def test_a_label_with_no_event_still_counts_as_started(self):
        node = issue(1, "x", labels=("in progress",), created=at(1))
        found = dashboard.read_issue(node, "v1.1.0", None)
        self.assertEqual((found["phase"], found["started"]), ("progress", None))

    def test_the_earlier_of_the_label_and_the_pull_request_is_the_start(self):
        node = issue(
            1,
            "x",
            labels=("in progress",),
            created=at(1),
            labeled=(("LabeledEvent", at(5), "in progress"),),
            pulls=(pull(2, at(3)),),
        )
        found = dashboard.read_issue(node, "v1.1.0", None)
        self.assertEqual((found["started"], found["started_label"], found["started_pr"]), (when(3), when(5), when(3)))

    def test_joined_is_when_it_was_last_put_on_this_milestone(self):
        found = self.read(110)
        self.assertEqual((found["created"], found["joined"]), (when(2), when(12)))

    def test_joined_is_never_before_it_was_opened_and_falls_back_to_then(self):
        self.assertEqual(self.read(103)["joined"], when(6))
        node = issue(1, "x", created=at(9), milestoned=((at(3), "v1.1.0"),))
        self.assertEqual(dashboard.read_issue(node, "v1.1.0", None)["joined"], when(9))

    def test_closed_before_it_joined_counts_as_done_when_it_joined(self):
        node = issue(1, "x", created=at(1), closed=at(2), milestoned=((at(8), "v1.1.0"),))
        self.assertEqual(dashboard.read_issue(node, "v1.1.0", None)["done"], when(8))

    def test_not_planned_is_closed_but_not_completed(self):
        found = self.read(107)
        self.assertEqual((found["open"], found["completed"], found["phase"]), (False, False, "done"))

    def test_the_tracker_is_marked(self):
        self.assertTrue(self.read(100)["tracker"])
        self.assertFalse(self.read(101)["tracker"])

    def test_find_tracker(self):
        self.assertEqual(dashboard.find_tracker(CURRENT_ISSUES), 100)
        self.assertIsNone(dashboard.find_tracker([issue(1, "[RELEASE 1.1] Release notes", created=at(1))]))
        both = [
            issue(7, "[RELEASE 1.1] Tracker: second", created=at(1)),
            issue(3, "[release 1.1] v1.1.0 release tracker", created=at(1)),
        ]
        self.assertEqual(dashboard.find_tracker(both), 3)

    def test_pull_requests_of_the_release(self):
        issues = [dashboard.read_issue(node, "v1.1.0", 100) for node in CURRENT_ISSUES]
        found = dashboard.attributed_pull_requests(issues, RAW["v1.1.0"]["pull_requests"])
        self.assertEqual([pr["n"] for pr in found], [201, 202, 211, 212, 220, 221])
        self.assertEqual(sum(1 for pr in found if pr["merged"]), 4)


class ChecklistTests(unittest.TestCase):
    def test_sections_items_and_references(self):
        sections = dashboard.parse_checklist(TRACKER_BODY)
        self.assertEqual([section["title"] for section in sections], ["Stage 1 · Systems", "Stage 2 · Features"])
        first, second = sections
        self.assertEqual([item["checked"] for item in first["items"]], [True, False, False])
        self.assertEqual([item["lead"] for item in first["items"]], [105, 101, None])
        self.assertEqual([item["lead"] for item in second["items"]], [104, None, 999])
        self.assertEqual([item["refs"] for item in second["items"]], [[104, 103], [107], [999]])
        self.assertEqual(second["items"][2]["text"], "#999 Something on another milestone")

    def test_items_before_any_heading_and_long_text(self):
        sections = dashboard.parse_checklist("- [ ] " + "word " * 80 + "\n- [X] done\n")
        self.assertEqual(sections[0]["title"], "")
        self.assertLessEqual(len(sections[0]["items"][0]["text"]), 200)
        self.assertTrue(sections[0]["items"][0]["text"].endswith("…"))
        self.assertTrue(sections[0]["items"][1]["checked"])

    def test_not_references(self):
        (section,) = dashboard.parse_checklist("- [ ] see a#1, b/c#2, &#39; and ##3, but #4 and (#5)\n")
        self.assertEqual(section["items"][0]["refs"], [4, 5])
        self.assertIsNone(section["items"][0]["lead"])

    def test_a_number_too_long_for_an_issue_is_not_a_reference(self):
        # A workflow run's id written with a # would be refused as a GraphQL Int and fail the whole lookup.
        (section,) = dashboard.parse_checklist("- [ ] #40434773436 dry run green, see #123456789 and #1234567890\n")
        self.assertEqual((section["items"][0]["refs"], section["items"][0]["lead"]), ([123456789], None))

    def test_empty(self):
        self.assertEqual(dashboard.parse_checklist(""), [])
        self.assertEqual(dashboard.parse_checklist(None), [])

    def test_an_item_takes_its_leading_issue_state_unless_ticked(self):
        states = {101: "done", 104: "progress", 105: "todo", 107: "done", 999: "done"}
        resolved = dashboard.resolve_checklist(dashboard.parse_checklist(TRACKER_BODY), states)
        self.assertEqual([item["state"] for item in resolved[0]["items"]], ["done", "done", "todo"])
        # "Docs pass (#107)" doesn't start with its issue, so the closed #107 doesn't tick it.
        self.assertEqual([item["state"] for item in resolved[1]["items"]], ["progress", "todo", "done"])
        self.assertEqual(resolved[1]["items"][0]["refs"], [{"n": 104, "state": "progress"}, {"n": 103, "state": None}])
        self.assertEqual([item["ticked"] for item in resolved[0]["items"]], [True, False, False])

    def test_an_unknown_leading_issue_is_not_done(self):
        resolved = dashboard.resolve_checklist(dashboard.parse_checklist("- [ ] #5 x\n"), {})
        self.assertEqual(resolved[0]["items"][0]["state"], "todo")


class SignalTests(unittest.TestCase):
    def comments(self, *texts, author="OWNER"):
        return [
            {"body": text, "createdAt": at(1, hour), "url": f"u{hour}", "authorAssociation": author}
            for hour, text in enumerate(texts)
        ]

    def test_the_v0_10_0_declaration(self):
        found = dashboard.find_freeze(TRACKER_COMMENTS)
        self.assertEqual((found["at"], found["text"]), (when(18, 9), "I declare the feature freeze"))

    def test_what_is_not_a_declaration(self):
        for text in (
            READINESS,
            "This is a readiness report, not a freeze declaration",
            "Ready for the feature freeze?",
            "No feature freeze until #5 merges",
            "Windows lane: ready for feature freeze",
            "Windows lane: all features merged for the feature freeze",  # mentions it, doesn't declare it
            "The feature freeze is close",
            "feature freeze " + "x" * 120,
            "freeze",
            "",
        ):
            self.assertIsNone(dashboard.find_freeze(self.comments(text)), text)

    def test_declarations(self):
        for text in (
            "feature freeze",
            "Feature freeze.",
            "I declare the feature freeze",  # v0.10.0, on #3223
            "Feature Freeze Declared. Our Features are LOCKED for v0.11.0.",  # v0.11.0, on #3397
            "  Feature Freeze declared  ",
        ):
            self.assertIsNotNone(dashboard.find_freeze(self.comments(text)), text)

    def test_only_someone_who_can_write_to_the_repository_declares_or_approves(self):
        # Anyone can comment on a public repository's issue.
        for author in ("NONE", "CONTRIBUTOR", "FIRST_TIME_CONTRIBUTOR", None):
            self.assertIsNone(
                dashboard.find_freeze(self.comments("I declare the feature freeze", author=author)), author
            )
            outsiders = self.comments("Approved for v0.10.0 release", author=author)
            self.assertIsNone(dashboard.find_approval(outsiders, "v0.10.0", None), author)
        for author in ("OWNER", "MEMBER", "COLLABORATOR"):
            self.assertIsNotNone(
                dashboard.find_freeze(self.comments("I declare the feature freeze", author=author)), author
            )
        bare = [{"body": "I declare the feature freeze", "createdAt": at(1), "url": "u"}]  # an older dump: no author
        self.assertIsNone(dashboard.find_freeze(bare))

    def test_the_first_declaration_counts(self):
        found = dashboard.find_freeze(self.comments("nothing", "feature freeze", "Feature freeze, again"))
        self.assertEqual(found["url"], "u1")

    def test_approval_needs_the_freeze_first_and_names_what_it_approves(self):
        # "Approved for v1.1.0 release" at 17:01 came before the freeze, and "Approved" alone could be an exception.
        freeze = dashboard.find_freeze(TRACKER_COMMENTS)
        self.assertIsNone(dashboard.find_approval(TRACKER_COMMENTS, "v1.1.0", freeze["at"]))
        self.assertIsNotNone(dashboard.find_approval(TRACKER_COMMENTS, "v1.1.0", None))

    def test_approvals(self):
        for text in (
            "Approved for v0.10.0 release",
            "I approve the candidate",
            "Candidate f4bffd6d approved",
            "Approved: 0.10.0",
        ):
            self.assertIsNotNone(dashboard.find_approval(self.comments(text), "v0.10.0", None), text)
        for text in (
            "Approved",
            "Not approved for release yet",
            "Is the candidate approved?",
            "Approved for v0.9.9",
            "Approved: #4600 exception for the v0.10.0 release",  # an exception, not the candidate
            "Exception approved for the v0.10.0 release",
        ):
            self.assertIsNone(dashboard.find_approval(self.comments(text), "v0.10.0", None), text)


class RedTests(unittest.TestCase):
    def test_periods_are_one_branchs_issues(self):
        periods = dashboard.red_periods(RED_ISSUES, "main")
        self.assertEqual([period["n"] for period in periods], [300, 301])
        self.assertEqual([period["n"] for period in dashboard.red_periods(RED_ISSUES, "release/1.0")], [302])

    def test_an_open_issue_has_no_end(self):
        periods = dashboard.red_periods(
            [{"number": 1, "title": "CI: main is red", "created_at": at(3), "closed_at": None}], "main"
        )
        self.assertIsNone(periods[0]["end"])
        self.assertEqual(dashboard.red_stats(periods, when(1), when(4))["hours"], 24.0)

    def test_stats_are_clipped_to_the_window(self):
        periods = dashboard.red_periods(RED_ISSUES, "main")
        self.assertEqual(
            dashboard.red_stats(periods, when(10, 12), NOW),
            {"count": 2, "hours": 8.0, "longest_hours": 6.0, "percent": 3.3},
        )
        self.assertEqual(dashboard.red_stats(periods, when(12, 3), when(12, 5))["hours"], 2.0)
        self.assertEqual(
            dashboard.red_stats(periods, when(13), when(19)),
            {"count": 0, "hours": 0.0, "longest_hours": 0.0, "percent": 0.0},
        )


class FigureTests(unittest.TestCase):
    def test_percentile_interpolates(self):
        self.assertEqual(dashboard.percentile([1.0], 0.9), 1.0)
        self.assertEqual(dashboard.percentile([1.0, 3.0], 0.5), 2.0)
        self.assertAlmostEqual(dashboard.percentile([0.0, 10.0, 20.0, 30.0, 40.0], 0.9), 36.0)

    def test_spread(self):
        self.assertIsNone(dashboard.spread([]))
        self.assertIsNone(dashboard.spread([-1.0]))
        self.assertEqual(dashboard.spread([4.0, 2.0, -3.0, 6.0]), {"n": 3, "median": 4.0, "p90": 5.6})

    def issues(self):
        return [dashboard.read_issue(node, "v1.1.0", 100) for node in CURRENT_ISSUES if node["number"] != 100]

    def test_moved(self):
        # Closed in the week: #103, #107 and #110, and #110 in the last day. Joined in the week: #102, #105, #106,
        # #108 and #109, and #106 in the last day (opened on the 19th at 18:00).
        self.assertEqual(
            dashboard.moved(self.issues(), NOW), {"closed_24h": 1, "closed_7d": 3, "added_24h": 1, "added_7d": 5}
        )
        self.assertEqual(dashboard.moved(self.issues(), NOW + timedelta(hours=7))["added_24h"], 0)

    def test_pace(self):
        one, three, seven = dashboard.pace(self.issues(), NOW)
        self.assertEqual((one["days"], three["days"], seven["days"]), (1, 3, 7))
        # Last day: #110 closed and #106 joined, so the six open issues aren't going down.
        self.assertEqual(
            one, {"days": 1, "closed_per_day": 1.0, "added_per_day": 1.0, "net_per_day": 0.0, "days_left": None}
        )
        # Without #106, one a day is closing: five days for the five left.
        earlier = dashboard.pace([issue for issue in self.issues() if issue["n"] != 106], NOW)[0]
        self.assertEqual((earlier["net_per_day"], earlier["days_left"]), (1.0, 5.0))
        # Last three days: #110 closed; #102, #105 and #106 joined.
        self.assertEqual((three["closed_per_day"], three["added_per_day"], three["net_per_day"]), (0.3, 1.0, -0.7))
        self.assertIsNone(three["days_left"])

    def test_pace_with_nothing_open(self):
        closed = [issue for issue in self.issues() if not issue["open"]]
        self.assertEqual([rate["days_left"] for rate in dashboard.pace(closed, NOW)], [0.0, 0.0, 0.0])

    def test_release_figures(self):
        figures = build("v1.1.0")["figures"]
        self.assertEqual(figures["days"], 10.0)
        self.assertEqual((figures["partial"], figures["tracked_days"]), (False, 10.0))
        self.assertEqual(
            (figures["issues"], figures["issues_open"], figures["issues_closed"], figures["issues_not_planned"]),
            (10, 6, 4, 1),
        )
        self.assertEqual((figures["carried_in"], figures["added_during"]), (1, 9))  # only #103 was there at the start
        self.assertEqual(figures["pull_requests_merged"], 4)
        self.assertEqual(figures["pull_requests_unmerged"], 1)  # #221 was closed without merging
        # Lines changed: #201 40, #211 600, #220 200. #212 came back with no size.
        self.assertEqual(figures["pull_request_lines"], {"n": 3, "median": 200.0, "p90": 520.0})
        self.assertEqual(figures["reopened"], 1)
        self.assertEqual(
            figures["severities"],
            {
                "high": {"found": 1, "fixed": 1, "open": 0},
                "medium": {"found": 1, "fixed": 0, "open": 1},
                "low": {"found": 1, "fixed": 0, "open": 1},
            },
        )
        self.assertEqual(figures["closed_per_day"], 0.4)
        # Opened to closed: #101 6 h, #103 216 h, #110 438 h. #107 wasn't completed.
        self.assertEqual(figures["lead_hours"], {"n": 3, "median": 216.0, "p90": 393.6})
        # Joined to first pull request: #101 2 h, #103 168 h, #110 168 h.
        self.assertEqual(figures["queue_hours"], {"n": 3, "median": 168.0, "p90": 168.0})
        # First pull request to closed: #101 4 h, #103 48 h, #110 30 h.
        self.assertEqual(figures["active_hours"], {"n": 3, "median": 30.0, "p90": 44.4})
        # Opened to merged: #201 4 h, #211 48 h, #212 30 h, #220 1 h.
        self.assertEqual(figures["merge_hours"], {"n": 4, "median": 17.0, "p90": 42.6})
        self.assertEqual(
            figures["bug_lead_hours"], {"high": {"n": 1, "median": 6.0, "p90": 6.0}, "medium": None, "low": None}
        )
        self.assertEqual(figures["red"], {"count": 2, "hours": 8.0, "longest_hours": 6.0, "percent": 3.3})
        self.assertEqual(
            (figures["ci"]["success"], figures["ci"]["failure"], figures["ci"]["pass_percent"]), (190, 10, 95.0)
        )

    def test_a_bug_from_before_the_cycle_was_not_found_in_it(self):
        old = issue(1, "x", labels=("bug", "sev:high"), created=at(3), closed=at(12))
        figures = dashboard.release_figures(
            [dashboard.read_issue(old, "v1.1.0", None)], [], {"start": when(10), "end": None}, [], [], NOW
        )
        self.assertEqual(figures["severities"]["high"], {"found": 0, "fixed": 1, "open": 0})

    def test_breakdown_by_lane(self):
        by_lane = {entry["name"]: entry for entry in dashboard.breakdown(self.issues(), "lane", NOW)}
        self.assertEqual(sorted(by_lane), ["None", "Shared", "WSL", "Windows"])
        # Windows: #105 and #108 are open and not started; #103 and #110 closed in the week, #110 in the day.
        self.assertEqual(
            by_lane["Windows"],
            {
                "name": "Windows",
                "open": 2,
                "todo": 2,
                "progress": 0,
                "review": 0,
                "closed_24h": 1,
                "closed_7d": 2,
                "added_24h": 0,
                "added_7d": 2,
            },
        )
        # WSL: #102 is in review, #104 in progress and #106 not started.
        self.assertEqual(
            (by_lane["WSL"]["open"], by_lane["WSL"]["todo"], by_lane["WSL"]["progress"], by_lane["WSL"]["review"]),
            (3, 1, 1, 1),
        )
        # No lane: only the dropped #107, closed in the week.
        self.assertEqual((by_lane["None"]["open"], by_lane["None"]["closed_7d"]), (0, 1))

    def test_breakdown_leaves_out_a_group_with_nothing_open_or_moving(self):
        old = issue(1, "x", labels=("bug", "agent:WSL"), created=at(1), closed=at(2))
        self.assertEqual(dashboard.breakdown([dashboard.read_issue(old, "v1.1.0", None)], "lane", NOW), [])

    def test_breakdown_by_kind(self):
        by_kind = {entry["name"]: entry["open"] for entry in dashboard.breakdown(self.issues(), "category", NOW)}
        self.assertEqual(
            by_kind, {"Bug": 2, "CI": 1, "Docs": 0, "Feature": 0, "Parity": 1, "Refactor": 0, "Testing": 2}
        )

    def test_label_gaps(self):
        gaps = dashboard.label_gaps(self.issues())
        # Open with no priority: #104, #108 and #109. Every open issue has a lane, and every open bug a severity.
        self.assertEqual(gaps, {"no_lane": [], "no_priority": [104, 108, 109], "no_severity": []})
        loose = issue(7, "x", labels=("bug",), created=at(1))
        self.assertEqual(
            dashboard.label_gaps([dashboard.read_issue(loose, "v1.1.0", None)]),
            {"no_lane": [7], "no_priority": [7], "no_severity": [7]},
        )

    def test_time_in_review_and_pull_request_ci(self):
        figures = build("v1.1.0")["figures"]
        # Ready to merged: #201 was marked ready at 03:00 and merged at 06:00 (3 h), and #211 a day after it was
        # opened (24 h, not the 48 h since it was opened). #212 and #220 never were drafts: 30 h and 1 h.
        self.assertEqual(figures["review_hours"], {"n": 4, "median": 13.5, "p90": 28.2})
        self.assertEqual(
            figures["pr_ci"], {"success": 30, "failure": 10, "merged": 8, "pass_percent": 75.0, "runs_per_merged": 5.0}
        )
        self.assertIsNone(build("v1.0.0")["figures"]["pr_ci"])  # no counts for it
        self.assertEqual(dashboard._pr_ci_figures({"success": 0, "failure": 0, "merged": 0})["pass_percent"], None)
        self.assertEqual(dashboard._pr_ci_figures({"success": 3, "failure": 1, "merged": 0})["runs_per_merged"], None)
        self.assertIsNone(dashboard._pr_ci_figures({}))

    def test_when_a_pull_request_became_reviewable(self):
        def request(**changes):
            return {"n": 7, "draft": False, "state": "merged", "created": when(3), "ready": None, **changes}

        self.assertEqual(dashboard.review_start(request(), {}), when(3))  # never a draft
        self.assertEqual(dashboard.review_start(request(ready=when(4)), {}), when(4))
        self.assertEqual(dashboard.review_start(request(), {7: when(5)}), when(5))  # known from the milestone's list
        self.assertIsNone(dashboard.review_start(request(draft=True, state="open"), {7: when(5)}))  # still a draft
        # Closed unmerged: abandoned, so no review started with it, draft or not.
        self.assertIsNone(dashboard.review_start(request(draft=True, state="closed"), {}))
        self.assertIsNone(dashboard.review_start(request(state="closed", ready=when(4)), {7: when(5)}))

    def test_an_abandoned_pull_request_is_not_a_review(self):
        # A draft closed unmerged on the 2nd, then a pull request opened ready on the 10th and still open.
        node = issue(
            1,
            "x",
            created=at(1),
            pulls=(pull(2, at(1), state="CLOSED", draft=True), pull(3, at(10))),
        )
        release = dashboard.build_release(
            {**RAW["v1.1.0"], "issues": [node], "tracker": None, "pull_requests": [], "pull_request_ready": {}},
            published(),
            RED_ISSUES,
            CI_COUNTS,
            None,
            REPOSITORY,
            NOW,
            {(1, 1, 0)},
        )
        self.assertEqual(release["counts"]["phases"], {"todo": 0, "progress": 0, "review": 1, "done": 0})
        # It started with its first pull request, on the 1st. It's in review from the second one, on the 10th: not
        # from the abandoned draft.
        self.assertEqual(release["events"]["flow_review"], [dashboard.epoch(when(10))])

    def test_ready_times_are_asked_for_forty_at_a_time(self):
        asked = []

        class Fake:
            def graphql(self, query, *, partial=False, **variables):
                asked.append((query, partial))
                numbers = [int(number) for number in re.findall(r"p(\d+):pullRequest", query)]
                found = {
                    f"p{number}": {"timelineItems": {"nodes": [{"createdAt": at(4)}] if number % 2 else []}}
                    for number in numbers
                }
                found["p7"] = None  # a number that names no pull request
                return {"repository": found}

        found = dashboard.collect_ready_times(Fake(), list(range(1, 46)) + [3, 3])
        self.assertEqual(len(asked), 2)  # 45 numbers, 40 to a query
        self.assertTrue(all(partial for _, partial in asked))
        self.assertIn("p1:pullRequest(number:1){timelineItems(itemTypes:[READY_FOR_REVIEW_EVENT],last:1)", asked[0][0])
        self.assertEqual((found["1"], found["2"], found["7"], found["45"]), (at(4), None, None, at(4)))
        self.assertEqual(len(found), 45)
        self.assertEqual(dashboard.collect_ready_times(Fake(), []), {})

    def test_the_flow_lists_nest(self):
        release = build("v1.1.0")
        events = release["events"]
        # Ten issues joined and four are done. Started: the four done, #102 (in review) and #104 (in progress).
        # In review: the four done and #102. #109's label came off again and #105's never came, so neither started.
        self.assertEqual([len(events[key]) for key in ("joined", "flow_started", "flow_review", "done")], [10, 6, 5, 4])
        for earlier, later in (("joined", "flow_started"), ("flow_started", "flow_review"), ("flow_review", "done")):
            # At every moment, no more issues have reached the later state than the earlier one.
            for moment in sorted(set(events[earlier]) | set(events[later])):
                reached = [sum(1 for stamp in events[key] if stamp <= moment) for key in (earlier, later)]
                self.assertGreaterEqual(reached[0], reached[1], (earlier, later, moment))

    def test_flow_times_stay_in_order_for_one_issue(self):
        def flow(**changes):
            base = {"joined": when(5), "done": None, "open": True, "phase": "todo", "started": None, "review": None}
            return dashboard.flow_events([{**base, **changes}])

        self.assertEqual(flow(), {"started": [], "review": []})
        # Its pull request is older than its place on the milestone: it started when it joined.
        early = flow(phase="review", started=when(2), review=when(3))
        self.assertEqual(early, {"started": [dashboard.epoch(when(5))], "review": [dashboard.epoch(when(5))]})
        # In progress now, with a pull request that was once ready and is a draft again: not in review.
        self.assertEqual(
            flow(phase="progress", started=when(6), review=when(7)),
            {"started": [dashboard.epoch(when(6))], "review": []},
        )
        # In progress by its label, with no time recorded for it: since it joined.
        self.assertEqual(flow(phase="progress"), {"started": [dashboard.epoch(when(5))], "review": []})
        self.assertEqual(
            flow(phase="review", started=when(6)),
            {"started": [dashboard.epoch(when(6))], "review": [dashboard.epoch(when(6))]},
        )
        # A pull request closed unmerged put it back to not started.
        self.assertEqual(flow(started=when(6), review=when(7)), {"started": [], "review": []})
        # Closed with nothing recorded: it passes through both states at its close.
        closed = flow(open=False, phase="done", done=when(9))
        self.assertEqual(closed, {"started": [dashboard.epoch(when(9))], "review": [dashboard.epoch(when(9))]})
        # Closed before its review time (a late pull request): the times are pulled back to the close.
        late = flow(open=False, phase="done", done=when(9), started=when(8), review=when(12))
        self.assertEqual(late, {"started": [dashboard.epoch(when(8))], "review": [dashboard.epoch(when(9))]})

    def test_no_event_is_later_than_the_run(self):
        # An issue filed while the run was reading the API: it's in the counts, so it's in the charts, at "now".
        filed_late = issue(120, "[BUG] Filed during the run", labels=("bug",), created=at(20, 13))
        raw = {**RAW["v1.1.0"], "issues": [*CURRENT_ISSUES, filed_late]}
        release = dashboard.build_release(raw, published(), RED_ISSUES, CI_COUNTS, LIVE, REPOSITORY, NOW, {(1, 1, 0)})
        self.assertEqual(release["counts"]["issues"], 11)
        self.assertEqual(len(release["events"]["joined"]), 11)
        self.assertEqual(max(release["events"]["joined"]), dashboard.epoch(NOW))

    def test_a_milestone_opened_late_is_partial(self):
        issues = self.issues()
        window = {"start": when(1), "end": when(11)}
        figures = dashboard.release_figures(issues, [], window, [], [], NOW, tracked_from=when(9))
        self.assertEqual((figures["days"], figures["partial"], figures["tracked_days"]), (10.0, True, 2.0))
        figures = dashboard.release_figures(issues, [], window, [], [], NOW, tracked_from=when(2))
        self.assertFalse(figures["partial"])

    def test_no_runs_is_no_pass_rate(self):
        self.assertIsNone(build("v1.2.0")["figures"].get("ci"))
        figures = dashboard.release_figures([], [], {"start": when(1), "end": when(2)}, [], [], NOW)
        self.assertIsNone(figures["ci"]["pass_percent"])
        self.assertIsNone(figures["lead_hours"])


class ShipListTests(unittest.TestCase):
    def test_the_release_in_its_freeze(self):
        release = build("v1.1.0")
        found = rows(release)
        self.assertEqual(
            list(found),
            [
                "scope",
                "severity",
                "features",
                "freeze",
                "review",
                "performance",
                "checklist",
                "maintainer",
                "pull_requests",
                "main",
                "dry_run",
                "candidate",
                "published",
            ],
        )
        self.assertEqual((found["scope"]["state"], found["scope"]["summary"]), ("active", "6 open of 10"))
        self.assertEqual(found["scope"]["moved"], {"closed_24h": 1, "closed_7d": 3, "added_24h": 1, "added_7d": 5})
        # #101 (high) is closed, so only the medium #102 is left: it doesn't hold the release.
        self.assertEqual(
            (found["severity"]["state"], found["severity"]["summary"], found["severity"]["issues"]),
            ("active", "0 high, 1 medium open", [102]),
        )
        # #104 is a parity feature still open after the freeze. #105 is CI work though it's labelled enhancement.
        self.assertEqual(
            (found["features"]["state"], found["features"]["summary"], found["features"]["issues"]),
            ("blocked", "1 feature still open", [104]),
        )
        self.assertEqual(
            (found["freeze"]["state"], found["freeze"]["summary"]), ("done", "declared 2026-01-18T09:00:00Z")
        )
        self.assertEqual(found["freeze"]["link"], f"https://github.com/{REPOSITORY}/issues/100#issuecomment-2")
        self.assertEqual(
            (found["review"]["state"], found["review"]["summary"], found["review"]["issues"]),
            ("active", "1 open of 1", [106]),
        )
        self.assertEqual((found["checklist"]["state"], found["checklist"]["summary"]), ("active", "3 of 6 done"))
        self.assertEqual((found["maintainer"]["state"], found["maintainer"]["issues"]), ("active", [108]))
        self.assertEqual(
            (found["pull_requests"]["state"], found["pull_requests"]["summary"]),
            ("active", "1 open: 1 ready (longest 9 h), 0 draft"),
        )
        # Main went green at 02:00 and it's 12:00: ten of the 24 hours.
        self.assertEqual((found["main"]["state"], found["main"]["summary"]), ("active", "green for 10 h"))
        self.assertEqual(
            (found["dry_run"]["state"], found["dry_run"]["summary"]),
            ("active", "passed 2026-01-19T00:00:00Z, 3 commits behind"),
        )
        self.assertEqual((found["candidate"]["state"], found["published"]["state"]), ("todo", "todo"))
        self.assertEqual(release["stage"], "freeze")

    def case(self, *, issues=(), signals=None, live=None, end=None):
        read = [dashboard.read_issue(node, "v1.1.0", None) for node in issues]
        window = {"start": when(10, 12), "start_reason": "x", "end": end, "release_tag": None}
        return {row["id"]: row for row in dashboard.ship_list(read, [], signals or {}, live, window, False, NOW)}

    def live(self, *, main, dry_run=None, pulls=(), branch="main"):
        return {"branch": branch, "pull_requests": list(pulls), "main": main, "dry_run": dry_run}

    def test_open_performance_issues_are_their_own_line(self):
        found = self.case(
            issues=[
                issue(1, "x", labels=("performance",), created=at(11)),
                issue(2, "y", labels=("performance", "bug"), created=at(11), closed=at(12)),
            ]
        )
        self.assertEqual(
            (found["performance"]["state"], found["performance"]["summary"], found["performance"]["issues"]),
            ("active", "1 open of 2", [1]),
        )
        self.assertEqual(self.case()["performance"]["state"], "done")

    def test_the_longest_ready_pull_request_is_in_the_summary(self):
        pulls = [
            {"in_scope": True, "draft": False, "checks": "SUCCESS", "ready_since": when(20, 7)},
            {"in_scope": True, "draft": False, "checks": "SUCCESS", "ready_since": when(18, 12)},
            {"in_scope": True, "draft": True, "checks": None, "ready_since": None},
            {"in_scope": False, "draft": False, "checks": "SUCCESS", "ready_since": when(1)},  # another release's
        ]
        found = self.case(live=self.live(main={"state": "green", "hours": 1.0}, pulls=pulls))
        self.assertEqual(found["pull_requests"]["summary"], "3 open: 2 ready (longest 2 days), 1 draft")

    def test_what_a_pull_request_is_waiting_on(self):
        def waiting(**changes):
            base = {"draft": False, "checks": "SUCCESS", "merge_state": "CLEAN", "threads": 0}
            return dashboard.waiting_on({**base, **changes})

        self.assertEqual(waiting(), "ready to merge")
        self.assertEqual(waiting(draft=True, checks="FAILURE"), "a draft")
        self.assertEqual(waiting(checks="FAILURE", merge_state="BEHIND", threads=2), "checks failing")
        self.assertEqual(waiting(checks="ERROR"), "checks failing")
        self.assertEqual(waiting(merge_state="DIRTY", threads=1), "conflicts with its base")
        self.assertEqual(waiting(threads=1), "1 review thread open")
        self.assertEqual(waiting(threads=3, merge_state="BEHIND"), "3 review threads open")
        self.assertEqual(waiting(merge_state="BEHIND", checks="PENDING"), "behind its base: needs a catch-up")
        self.assertEqual(waiting(checks="PENDING", merge_state="BLOCKED"), "checks running")
        self.assertEqual(waiting(checks=None, merge_state="UNKNOWN"), "no checks yet")
        self.assertEqual(waiting(merge_state="BLOCKED"), "a required check or review")
        self.assertEqual(waiting(merge_state="UNKNOWN"), "ready to merge")

    def test_an_open_high_severity_bug_blocks(self):
        found = self.case(
            issues=[
                issue(1, "x", labels=("bug", "sev:high"), created=at(11)),
                issue(2, "y", labels=("bug", "sev:medium"), created=at(11)),
            ]
        )
        self.assertEqual(
            (found["severity"]["state"], found["severity"]["summary"], found["severity"]["issues"]),
            ("blocked", "1 high, 1 medium open", [1, 2]),
        )

    def test_everything_closed(self):
        found = self.case(issues=[issue(1, "x", labels=("bug", "sev:high"), created=at(11), closed=at(12))])
        self.assertEqual(
            [found[key]["state"] for key in ("scope", "severity", "features", "review", "maintainer")], ["done"] * 5
        )
        self.assertNotIn("checklist", found)  # no tracker, no row

    def test_open_features_before_the_freeze_are_just_in_progress(self):
        found = self.case(issues=[issue(1, "x", labels=("enhancement",), created=at(11))])
        self.assertEqual((found["features"]["state"], found["freeze"]["state"]), ("active", "todo"))

    def test_main_red_blocks_and_a_day_green_passes(self):
        red = self.case(live=self.live(main={"state": "red", "hours": 0.3, "issue_url": "u"}))
        self.assertEqual(
            (red["main"]["state"], red["main"]["summary"], red["main"]["link"]), ("blocked", "red for 18 min", "u")
        )
        day = self.case(live=self.live(main={"state": "green", "hours": 24.0}))
        self.assertEqual((day["main"]["state"], day["main"]["summary"]), ("done", "green for 24 h"))
        never = self.case(live=self.live(main={"state": "green", "hours": None}))
        self.assertEqual((never["main"]["state"], never["main"]["summary"]), ("done", "green"))
        long = self.case(live=self.live(main={"state": "green", "hours": 100.0}))
        self.assertEqual(long["main"]["summary"], "green for 4 days")

    def test_dry_run_states(self):
        green = {"state": "green", "hours": 30.0}
        self.assertEqual(self.case(live=self.live(main=green))["dry_run"]["state"], "todo")
        # A patch is dry-run on its own branch, and the row says so.
        patch = self.case(live=self.live(main=green, branch="release/1.0"))["dry_run"]
        self.assertEqual(patch["title"], "Signed release dry run on release/1.0")
        self.assertEqual(patch["note"], "gh workflow run release.yml --ref release/1.0 -f dry_run=true")
        at_head = {"conclusion": "success", "at": when(19), "url": "u", "behind": 0}
        found = self.case(live=self.live(main=green, dry_run=at_head))["dry_run"]
        self.assertEqual((found["state"], found["summary"]), ("done", "passed 2026-01-19T00:00:00Z at the head"))
        failed = {"conclusion": "failure", "at": when(19), "url": "u", "behind": 1}
        found = self.case(live=self.live(main=green, dry_run=failed))["dry_run"]
        self.assertEqual(
            (found["state"], found["summary"]), ("blocked", "failure 2026-01-19T00:00:00Z, 1 commit behind")
        )

    def test_pull_requests_in_scope(self):
        pulls = [
            {"in_scope": True, "draft": True, "checks": "FAILURE"},
            {"in_scope": True, "draft": False, "checks": "SUCCESS"},
            {"in_scope": False, "draft": False, "checks": "ERROR"},
        ]
        found = self.case(live=self.live(main={"state": "green", "hours": 1.0}, pulls=pulls))
        self.assertEqual(found["pull_requests"]["summary"], "2 open: 1 ready, 1 draft, 1 failing checks")
        self.assertEqual(
            self.case(live=self.live(main={"state": "green", "hours": 1.0}))["pull_requests"]["state"], "done"
        )

    def test_approved_and_published(self):
        signals = {"freeze": {"at": when(18), "url": "f"}, "approval": {"at": when(19), "url": "a"}}
        found = self.case(signals=signals, end=when(20))
        self.assertEqual((found["candidate"]["state"], found["candidate"]["link"]), ("done", "a"))
        self.assertEqual(
            (found["published"]["state"], found["published"]["summary"]), ("done", "published 2026-01-20T00:00:00Z")
        )

    def test_stage(self):
        window = {"start": when(1), "end": None}
        self.assertEqual(dashboard.stage({}, window), "building")
        freeze, approval = {"at": when(2)}, {"at": when(3)}
        self.assertEqual(dashboard.stage({"freeze": freeze, "approval": None}, window), "freeze")
        self.assertEqual(dashboard.stage({"freeze": freeze, "approval": approval}, window), "candidate")
        self.assertEqual(
            dashboard.stage({"freeze": freeze, "approval": approval}, {"start": when(1), "end": when(2)}), "released"
        )
        self.assertEqual(dashboard.stage({}, {"start": None, "end": None}), "planned")


class BuildTests(unittest.TestCase):
    def test_the_release_under_way(self):
        release = build("v1.1.0")
        self.assertEqual((release["schema"], release["final"], release["branch"]), (dashboard.SCHEMA, False, "main"))
        self.assertEqual(release["generated_at"], "2026-01-20T12:00:00Z")
        self.assertEqual(
            release["cycle"],
            {"start": "2026-01-10T12:00:00Z", "start_reason": "v1.0.0 was published", "end": None, "release_tag": None},
        )
        self.assertEqual(release["tracker"], {"number": 100, "title": "[RELEASE 1.1] v1.1.0 release tracker"})
        self.assertEqual(
            release["counts"], {"issues": 10, "open": 6, "phases": {"todo": 4, "progress": 1, "review": 1, "done": 4}}
        )
        self.assertEqual([entry["n"] for entry in release["open_issues"]], [102, 104, 105, 106, 108, 109])
        self.assertEqual(release["open_issues"][0]["pull_requests"], [202])
        self.assertEqual([entry["n"] for entry in release["recently_closed"]], [110])  # the last 24 hours
        self.assertEqual([entry["name"] for entry in release["by_lane"]], ["None", "Shared", "WSL", "Windows"])
        self.assertEqual(release["label_gaps"]["no_priority"], [104, 108, 109])
        self.assertEqual(
            {key: len(times) for key, times in release["events"].items()},
            {"joined": 10, "done": 4, "started": 5, "merged": 4, "flow_started": 6, "flow_review": 5},
        )
        for times in release["events"].values():
            self.assertEqual(times, sorted(times))
        self.assertEqual([period["n"] for period in release["red_periods"]], [300, 301])
        self.assertEqual(release["milestone"]["due_on"], "2026-02-01T08:00:00Z")
        json.dumps(release)  # every value is JSON

    def test_no_freeze_means_no_approval(self):
        # "Approved for v1.1.0 release" alone, with no freeze declared: nothing can have been approved yet.
        tracker = {**RAW["v1.1.0"]["tracker"], "comments": [TRACKER_COMMENTS[1]]}
        raw = {**RAW["v1.1.0"], "tracker": tracker}
        release = dashboard.build_release(raw, published(), RED_ISSUES, CI_COUNTS, LIVE, REPOSITORY, NOW, {(1, 1, 0)})
        self.assertEqual((release["signals"], release["stage"]), ({"freeze": None, "approval": None}, "building"))
        self.assertIsNotNone(dashboard.find_approval(tracker["comments"], "v1.1.0", None))

    def test_the_tracker_is_not_scope(self):
        release = build("v1.1.0")
        self.assertNotIn(100, [entry["n"] for entry in release["open_issues"]])

    def test_live(self):
        live = build("v1.1.0")["live"]
        scoped, other = live["pull_requests"]
        self.assertEqual(
            (scoped["n"], scoped["in_scope"], scoped["draft"], scoped["checks"], scoped["closes"]),
            (202, True, False, "SUCCESS", [102]),
        )
        self.assertEqual(
            (scoped["threads"], scoped["auto_merge"], scoped["lane"], scoped["bot"]), (1, True, "WSL", False)
        )
        self.assertEqual(
            (other["n"], other["in_scope"], other["milestone"], other["bot"], other["ready"]),
            (250, False, "v1.2.0", True, None),
        )
        self.assertEqual(
            (live["main"]["state"], live["main"]["since"], live["main"]["hours"]),
            ("green", "2026-01-20T02:00:00Z", 10.0),
        )
        # CI is red for its failed nightly run though a later push run passed; neutral passes; no run is neither.
        self.assertEqual([workflow["passing"] for workflow in live["main"]["workflows"]], [False, True, None])
        self.assertEqual(
            live["main"]["workflows"][0]["runs"],
            [
                {"event": "push", "conclusion": "success", "at": "2026-01-20T11:00:00Z", "run": 900},
                {"event": "schedule", "conclusion": "failure", "at": "2026-01-20T04:00:00Z", "run": 899},
            ],
        )
        # The newest finished dry run: the one in progress and the cancelled one are passed over.
        self.assertEqual(
            (
                live["dry_run"]["run"],
                live["dry_run"]["conclusion"],
                live["dry_run"]["behind"],
                live["dry_run"]["running"],
            ),
            (908, "success", 3, True),
        )

    def test_live_pull_requests_say_what_they_wait_on(self):
        scoped, other = build("v1.1.0")["live"]["pull_requests"]
        # #202 was marked ready at 03:00 and has an unresolved review thread; #250 is a draft.
        self.assertEqual((scoped["waiting"], scoped["ready_since"]), ("1 review thread open", "2026-01-20T03:00:00Z"))
        self.assertEqual((other["waiting"], other["ready_since"]), ("a draft", None))

    def test_main_red_now(self):
        red = [*RED_ISSUES, {"number": 310, "title": "CI: main is red", "created_at": at(20, 11), "closed_at": None}]
        release = dashboard.build_release(
            RAW["v1.1.0"], published(), red, CI_COUNTS, LIVE, REPOSITORY, NOW, {(1, 1, 0)}
        )
        main = release["live"]["main"]
        self.assertEqual((main["state"], main["hours"], main["issue"]), ("red", 1.0, 310))
        self.assertEqual(main["issue_url"], f"https://github.com/{REPOSITORY}/issues/310")
        self.assertEqual(rows(release)["main"]["state"], "blocked")

    def test_the_shipped_release(self):
        release = build("v1.0.0")
        self.assertEqual(
            (release["stage"], release["final"], release["live"], release["pace"]), ("released", True, None, None)
        )
        self.assertEqual(release["cycle"]["start_reason"], "the milestone was opened")
        self.assertEqual(release["cycle"]["release_tag"], "v1.0.0")
        self.assertEqual(release["figures"]["pull_requests_merged"], 2)
        self.assertEqual(rows(release)["published"]["state"], "done")
        self.assertNotIn("main", rows(release))

    def test_a_release_is_final_a_day_after_its_milestone_closes(self):
        self.assertFalse(build("v1.0.0", now=when(11, 12))["final"])
        self.assertTrue(build("v1.0.0", now=when(11, 14))["final"])

    def test_the_planned_release(self):
        release = build("v1.2.0")
        self.assertEqual(
            (release["stage"], release["live"], release["pace"], release["red_periods"]), ("planned", None, None, [])
        )
        self.assertEqual(
            release["ship_list"],
            [
                {
                    "id": "scope",
                    "title": "Planned scope",
                    "state": "todo",
                    "summary": "2 issues",
                    "note": "It starts when v1.1.0 is published.",
                }
            ],
        )
        self.assertEqual(release["figures"], {"issues": 2, "issues_open": 2, "issues_closed": 0})
        self.assertEqual([entry["name"] for entry in release["by_lane"]], ["Dash", "None"])
        # #400 carries the label with no event for it: it's in progress in the counts, so it's started in the chart.
        self.assertEqual(release["counts"]["phases"]["progress"], 1)
        self.assertEqual(len(release["events"]["flow_started"]), 1)
        self.assertEqual(release["open_issues"][0]["lane"], "Dash")

    def test_index(self):
        index = dashboard.build_index([build("v1.2.0"), build("v1.0.0"), build("v1.1.0")], REPOSITORY, NOW)
        self.assertEqual((index["schema"], index["current"], index["repo"]), (dashboard.SCHEMA, "v1.1.0", REPOSITORY))
        self.assertEqual([entry["title"] for entry in index["releases"]], ["v1.0.0", "v1.1.0", "v1.2.0"])
        self.assertEqual([entry["stage"] for entry in index["releases"]], ["released", "freeze", "planned"])
        self.assertEqual((index["releases"][1]["open"], index["releases"][1]["issues"]), (6, 10))

    def test_index_opens_on_the_minor_main_is_building_not_on_an_open_patch(self):
        def stub(title, stage):
            return {
                "milestone": {"title": title, "state": "closed" if stage == "released" else "open"},
                "stage": stage,
                "cycle": {"start": None, "end": None},
                "counts": {"open": 0, "issues": 0},
                "figures": {},
            }

        # v0.11.0 shipped, then a v0.11.1 patch milestone was opened while main builds v0.12.0.
        releases = [stub("v0.11.0", "released"), stub("v0.11.1", "building"), stub("v0.12.0", "building")]
        self.assertEqual(dashboard.build_index(releases, REPOSITORY, NOW)["current"], "v0.12.0")
        # With no minor under way, the patch is the page's release.
        releases = [stub("v0.11.0", "released"), stub("v0.11.1", "freeze"), stub("v0.12.0", "planned")]
        self.assertEqual(dashboard.build_index(releases, REPOSITORY, NOW)["current"], "v0.11.1")

    def test_index_with_nothing_under_way_opens_on_the_newest(self):
        index = dashboard.build_index([build("v1.0.0"), build("v1.2.0")], REPOSITORY, NOW)
        self.assertEqual(index["current"], "v1.2.0")
        self.assertIsNone(dashboard.build_index([], REPOSITORY, NOW)["current"])


class FakeGitHub:
    """Answers the run-count calls from a list of run times, and stops counting at a ceiling as GitHub does."""

    def __init__(self, runs, ceiling):
        self.runs = runs
        self.ceiling = ceiling
        self.calls = 0

    def rest(self, path, **params):
        self.calls += 1
        start, end = (dashboard.parse_time(text) for text in params["created"].split(".."))
        return {"total_count": min(self.ceiling, sum(1 for moment in self.runs if start <= moment <= end))}


class RunListing:
    """Answers the run listings: the branch's last runs of an event, and one workflow's own."""

    def __init__(self, listings, own=None):
        self.listings = listings
        self.own = own or {}
        self.asked = []

    def rest(self, path, **params):
        self.asked.append((path, params["event"]))
        if path == "actions/runs":
            return {"workflow_runs": self.listings[params["event"]]}
        return {"workflow_runs": self.own.get((path, params["event"]), [])}


def listed(run_id, file, conclusion, created, event="push"):
    return {
        "id": run_id,
        "path": f".github/workflows/{file}",
        "status": "completed",
        "conclusion": conclusion,
        "event": event,
        "head_sha": "c" * 40,
        "created_at": created,
        "updated_at": created,
    }


class LatestRunsTests(unittest.TestCase):
    WATCHED = (("ci.yml", "CI"), ("pre-commit.yml", "Pre-commit"))

    def test_two_listings_answer_for_every_workflow(self):
        gh = RunListing(
            {
                "push": [
                    listed(5, "ci.yml", "cancelled", at(20, 11)),  # says nothing
                    listed(4, "pre-commit.yml", "success", at(20, 10)),
                    listed(3, "ci.yml", "failure", at(20, 9)),
                    listed(2, "ci.yml", "success", at(20, 8)),
                    listed(1, "other.yml", "failure", at(20, 7)),
                ],
                "schedule": [listed(9, "ci.yml@refs/heads/main", "success", at(20, 4), event="schedule")],
            }
        )
        found = dashboard.latest_runs(gh, self.WATCHED, "main", NOW)
        self.assertEqual(
            [(entry["file"], [run["id"] for run in entry["runs"]]) for entry in found],
            [("ci.yml", [3, 9]), ("pre-commit.yml", [4])],
        )
        self.assertEqual(found[0]["runs"][0]["conclusion"], "failure")
        # Short listings hold everything there is, so nothing more is asked: Pre-commit has no nightly run.
        self.assertEqual(gh.asked, [("actions/runs", "push"), ("actions/runs", "schedule")])

    def test_a_workflow_missing_from_a_short_reach_is_asked_for_by_itself(self):
        # A hundred push runs in the last two hours, none of them Pre-commit's.
        recent = [listed(1000 + index, "ci.yml", "success", at(20, 10)) for index in range(100)]
        own = {("actions/workflows/pre-commit.yml/runs", "push"): [listed(7, "pre-commit.yml", "failure", at(18))]}
        gh = RunListing({"push": recent, "schedule": []}, own)
        found = dashboard.latest_runs(gh, self.WATCHED, "main", NOW)
        self.assertEqual([run["id"] for run in found[1]["runs"]], [7])
        self.assertIn(("actions/workflows/pre-commit.yml/runs", "push"), gh.asked)
        self.assertEqual(len(gh.asked), 3)

    def test_a_workflow_missing_from_a_week_of_runs_has_none(self):
        # A hundred nightly runs reaching back three weeks, none of them Pre-commit's: it has no schedule.
        nightly = [
            listed(2000 + index, "ci.yml", "success", at(20 - index // 5, 4), event="schedule") for index in range(100)
        ]
        gh = RunListing({"push": [listed(1, "pre-commit.yml", "success", at(20, 10))], "schedule": nightly})
        found = dashboard.latest_runs(gh, self.WATCHED, "main", NOW)
        self.assertEqual([run["event"] for run in found[1]["runs"]], ["push"])
        self.assertEqual(len(gh.asked), 2)


class PullRequestCiTests(unittest.TestCase):
    def test_counts_the_cycles_pull_request_runs_and_merges(self):
        asked = []

        class Fake:
            repo = REPOSITORY

            def rest(self, path, **params):
                asked.append((path, params))
                if path == "/search/issues":
                    return {"total_count": 12}
                return {"total_count": 40 if params["status"] == "success" else 8}

        found = dashboard.collect_pr_ci(Fake(), when(10), when(20))
        self.assertEqual(found, {"success": 40, "failure": 8, "merged": 12})
        search = asked[0][1]["q"]
        self.assertEqual(search, f"repo:{REPOSITORY} is:pr is:merged merged:2026-01-10T00:00:00Z..2026-01-20T00:00:00Z")
        runs = asked[1]
        self.assertEqual(runs[0], "actions/workflows/ci.yml/runs")
        self.assertEqual((runs[1]["event"], runs[1]["branch"], runs[1]["status"]), ("pull_request", None, "success"))

    def test_a_path_from_the_api_root(self):
        gh = dashboard.GitHub(REPOSITORY, "token", api="https://api.invalid")
        seen = []
        gh._call = lambda what, url, body, check: seen.append(url) or {}
        gh.rest("/search/issues", q="a b", per_page=1)
        gh.rest("milestones", state="all", page=None)
        self.assertEqual(
            seen,
            [
                "https://api.invalid/search/issues?q=a+b&per_page=1",
                f"https://api.invalid/repos/{REPOSITORY}/milestones?state=all",
            ],
        )


class BudgetTests(unittest.TestCase):
    def test_the_log_line_and_the_warning(self):
        gh = dashboard.GitHub(REPOSITORY, "token")
        gh.rest_calls, gh.graphql_calls, gh.graphql_cost = 7, 20, 48
        self.assertEqual(gh.budget(), ("7 REST calls, 20 GraphQL calls (48 points)", []))
        gh.rest_budget, gh.graphql_budget = (900, 1000), (240, 1000)
        line, low = gh.budget()
        self.assertEqual(
            line,
            "7 REST calls, 20 GraphQL calls (48 points), REST: 900 of 1000 left this hour, GraphQL: 240 of 1000 left this hour",
        )
        self.assertEqual(low, ["GraphQL budget is low: 240 of 1000 left this hour"])
        gh.graphql_budget = (250, 1000)
        self.assertEqual(gh.budget()[1], [])


class CountRunsTests(unittest.TestCase):
    def test_a_count_under_the_ceiling_is_one_call(self):
        gh = FakeGitHub([when(1, hour) for hour in range(20)], dashboard.COUNT_CEILING)
        self.assertEqual(dashboard.count_runs(gh, "ci.yml", "main", "success", when(1), when(2)), 20)
        self.assertEqual(gh.calls, 1)

    def test_a_count_at_the_ceiling_is_split_by_date(self):
        runs = [when(1) + timedelta(minutes=minute) for minute in range(0, 2 * 24 * 60, 1)]
        gh = FakeGitHub(runs, dashboard.COUNT_CEILING)
        self.assertEqual(dashboard.count_runs(gh, "ci.yml", "main", "success", when(1), when(3)), len(runs))
        self.assertGreater(gh.calls, 1)

    def test_a_single_hour_is_never_split(self):
        gh = FakeGitHub([when(1)] * 5000, dashboard.COUNT_CEILING)
        self.assertEqual(
            dashboard.count_runs(gh, "ci.yml", "main", "success", when(1), when(1, 1)), dashboard.COUNT_CEILING
        )
        self.assertEqual(gh.calls, 1)


def write_raw(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "repository.json").write_text(json.dumps(REPOSITORY_RAW), encoding="utf-8")
    for title, raw in RAW.items():
        (folder / f"{title}.json").write_text(json.dumps(raw), encoding="utf-8")


def generate(out: Path, *extra: str) -> tuple[int, str]:
    """Runs the generator on the made-up repository; returns its exit code and what it printed."""
    printed = io.StringIO()
    with tempfile.TemporaryDirectory() as raw, contextlib.redirect_stdout(printed), contextlib.redirect_stderr(printed):
        write_raw(Path(raw))
        code = dashboard.main(["--out", str(out), "--from-raw", raw, "--repo", REPOSITORY, *extra])
    return code, printed.getvalue()


class RunTests(unittest.TestCase):
    def test_writes_every_release_and_the_index(self):
        with tempfile.TemporaryDirectory() as out:
            code, printed = generate(Path(out))
            self.assertEqual(code, 0, printed)
            self.assertEqual(
                sorted(path.name for path in Path(out).iterdir()),
                ["index.json", "v1.0.0.json", "v1.1.0.json", "v1.2.0.json"],
            )
            index = json.loads((Path(out) / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["current"], "v1.1.0")
            self.assertIn("v1.1.0: freeze, 6 open of 10 issues", printed)
            self.assertTrue((Path(out) / "index.json").read_bytes().endswith(b"}\n"))
            self.assertNotIn(b"\r", (Path(out) / "v1.1.0.json").read_bytes())

    def test_a_final_release_is_copied_from_the_last_run(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            generate(Path(first))
            kept = json.loads((Path(first) / "v1.0.0.json").read_text(encoding="utf-8"))
            kept["marker"] = "from the last run"
            (Path(first) / "v1.0.0.json").write_text(json.dumps(kept), encoding="utf-8")
            (Path(first) / "v1.1.0.json").write_text("{not json", encoding="utf-8")
            code, printed = generate(Path(second), "--previous", first)
            self.assertEqual(code, 0, printed)
            self.assertIn("v1.0.0: final, copied from the last run", printed)
            self.assertEqual(
                json.loads((Path(second) / "v1.0.0.json").read_text(encoding="utf-8"))["marker"], "from the last run"
            )
            self.assertEqual(json.loads((Path(second) / "v1.1.0.json").read_text(encoding="utf-8"))["stage"], "freeze")

    def test_the_earlier_file_is_read_only_in_this_schema(self):
        with tempfile.TemporaryDirectory() as previous:
            path = Path(previous) / "v1.0.0.json"
            path.write_text(json.dumps({"schema": dashboard.SCHEMA + 1, "final": True}), encoding="utf-8")
            self.assertIsNone(dashboard.earlier(Path(previous), "v1.0.0"))
            path.write_text("{not json", encoding="utf-8")
            self.assertIsNone(dashboard.earlier(Path(previous), "v1.0.0"))
            path.write_text(json.dumps([dashboard.SCHEMA]), encoding="utf-8")
            self.assertIsNone(dashboard.earlier(Path(previous), "v1.0.0"))
            path.write_text(json.dumps({"schema": dashboard.SCHEMA, "final": True}), encoding="utf-8")
            self.assertEqual(dashboard.earlier(Path(previous), "v1.0.0"), {"schema": dashboard.SCHEMA, "final": True})
            self.assertIsNone(dashboard.earlier(Path(previous), "v9.9.9"))
            self.assertIsNone(dashboard.earlier(None, "v1.0.0"))

    def test_what_stands_for_the_next_run(self):
        under_way, planned = {"start": when(10), "end": None}, {"start": None, "end": None}
        self.assertFalse(dashboard.reusable(None, under_way, NOW))
        # A finished release always does.
        self.assertTrue(dashboard.reusable({"final": True, "stage": "released"}, under_way, NOW))
        # A release under way never does: it's what the page is for.
        fresh = dashboard.iso(NOW - timedelta(minutes=10))
        self.assertFalse(dashboard.reusable({"final": False, "stage": "freeze", "generated_at": fresh}, under_way, NOW))
        # A planned one does for under an hour, while it's still planned.
        self.assertTrue(dashboard.reusable({"final": False, "stage": "planned", "generated_at": fresh}, planned, NOW))
        stale = dashboard.iso(NOW - timedelta(minutes=56))
        self.assertFalse(dashboard.reusable({"final": False, "stage": "planned", "generated_at": stale}, planned, NOW))
        # The release before it shipped since: it has started, so it's read afresh.
        self.assertFalse(
            dashboard.reusable({"final": False, "stage": "planned", "generated_at": fresh}, under_way, NOW)
        )
        for broken in (
            {"stage": "planned"},
            {"stage": "planned", "generated_at": "yesterday"},
            {"stage": "planned", "generated_at": 7},
        ):
            self.assertFalse(dashboard.reusable(broken, planned, NOW), broken)

    def test_pass_counts_are_reused_for_under_an_hour(self):
        window = {"start": when(10, 12), "end": None}
        counts = [{"file": "ci.yml", "name": "CI", "success": 1, "failure": 0}]
        pr_counts = {"success": 5, "failure": 1, "merged": 2}
        found = {
            "ci_counts": counts,
            "pr_ci_counts": pr_counts,
            "ci_counted_at": dashboard.iso(NOW - timedelta(minutes=30)),
            "cycle": {"start": "2026-01-10T12:00:00Z"},
        }
        self.assertEqual(dashboard.earlier_ci_counts(found, window, NOW), (counts, pr_counts))
        self.assertEqual(
            dashboard.earlier_ci_counts({**found, "pr_ci_counts": {}}, window, NOW), (counts, {})
        )  # a patch
        self.assertIsNone(dashboard.earlier_ci_counts(None, window, NOW))
        for changed in (
            {"ci_counted_at": dashboard.iso(NOW - timedelta(minutes=55))},
            {"ci_counted_at": dashboard.iso(NOW + timedelta(minutes=5))},
            {"ci_counts": []},
            {"pr_ci_counts": None},  # written before the page counted pull request CI
            {"cycle": {"start": "2026-01-09T12:00:00Z"}},
        ):
            self.assertIsNone(dashboard.earlier_ci_counts({**found, **changed}, window, NOW), changed)
        without = {key: value for key, value in found.items() if key != "ci_counted_at"}
        self.assertIsNone(dashboard.earlier_ci_counts(without, window, NOW))

    def test_a_planned_release_is_copied_while_it_is_fresh(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            generate(Path(first))
            code, printed = generate(Path(second), "--previous", first)
            self.assertEqual(code, 0, printed)
            self.assertIn("v1.2.0: planned, copied from the last run", printed)
            self.assertIn("v1.1.0: freeze, 6 open of 10 issues", printed)  # the release under way is always read
            later = dashboard.iso(NOW + timedelta(hours=2))
            code, printed = generate(Path(second), "--previous", first, "--now", later)
            self.assertIn("v1.2.0: planned, 2 open of 2 issues", printed)

    def test_the_release_file_carries_its_pass_counts_for_the_next_run(self):
        release = build("v1.1.0")
        self.assertEqual(release["ci_counts"], CI_COUNTS)
        self.assertEqual(release["ci_counted_at"], "2026-01-20T12:00:00Z")
        counted = dashboard.build_release(
            {**RAW["v1.1.0"], "ci_counted_at": "2026-01-20T11:30:00Z"},
            published(),
            RED_ISSUES,
            CI_COUNTS,
            LIVE,
            REPOSITORY,
            NOW,
            {(1, 1, 0)},
        )
        self.assertEqual(counted["ci_counted_at"], "2026-01-20T11:30:00Z")

    def test_one_milestone(self):
        with tempfile.TemporaryDirectory() as out:
            code, printed = generate(Path(out), "--milestone", "v1.1.0")
            self.assertEqual(code, 0, printed)
            self.assertEqual(sorted(path.name for path in Path(out).iterdir()), ["index.json", "v1.1.0.json"])

    def test_a_dump_that_cannot_be_read_writes_nothing(self):
        with tempfile.TemporaryDirectory() as out, tempfile.TemporaryDirectory() as raw:
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(printed):
                code = dashboard.main(["--out", out, "--from-raw", raw])
            self.assertEqual(code, 1)
            self.assertIn("::error title=Release dashboard::", printed.getvalue())
            self.assertEqual(list(Path(out).iterdir()), [])

    def test_bad_repo_is_a_usage_error(self):
        with self.assertRaises(SystemExit) as raised, contextlib.redirect_stderr(io.StringIO()):
            dashboard.main(["--out", "x", "--repo", "not a repo"])
        self.assertEqual(raised.exception.code, 2)


class BudgetNoteTests(unittest.TestCase):
    """#4726: with the API budget gone nothing can be read, so the last data goes out again as it is, with the
    time the budget returns in index.json for the page to show. No request is made for that."""

    UNTIL = 1_800_003_600  # as GitHub's rate_limit gives a reset: seconds since 1970
    UNTIL_ISO = dashboard.iso(datetime.fromtimestamp(UNTIL, UTC))

    def note(self, out: Path, *args: str) -> tuple[int, str]:
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(printed):
            code = dashboard.main(["--out", str(out), "--repo", REPOSITORY, *args])
        return code, printed.getvalue()

    def test_the_last_data_goes_out_again_with_the_time_and_nothing_else_changed(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            generate(Path(first))
            # The branch holds a README beside the data: it isn't data, and doesn't ride along.
            (Path(first) / "README.md").write_text("# Release dashboard data\n", encoding="utf-8")
            code, printed = self.note(Path(second), "--previous", first, "--budget-until", str(self.UNTIL))
            self.assertEqual(code, 0, printed)
            self.assertEqual(
                sorted(path.name for path in Path(second).iterdir()),
                ["index.json", "v1.0.0.json", "v1.1.0.json", "v1.2.0.json"],
            )
            before = json.loads((Path(first) / "index.json").read_text(encoding="utf-8"))
            after = json.loads((Path(second) / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(after.pop("budget_until"), self.UNTIL_ISO)
            # generated_at above all: the page's "updated" time and the workflow's "is this run covered" rule
            # read it, and this isn't new data.
            self.assertEqual(after, before)
            for name in ("v1.0.0.json", "v1.1.0.json", "v1.2.0.json"):
                self.assertEqual((Path(second) / name).read_bytes(), (Path(first) / name).read_bytes(), name)
            self.assertIn(f"The API budget is used up until {self.UNTIL_ISO}", printed)
            self.assertTrue((Path(second) / "index.json").read_bytes().endswith(b"}\n"))

    def test_no_request_is_made_and_no_token_is_looked_for(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            generate(Path(first))
            refuse = AssertionError("the budget note reached for GitHub")
            with mock.patch.object(dashboard, "GitHub", side_effect=refuse), mock.patch.object(dashboard, "find_token", side_effect=refuse):
                code, printed = self.note(Path(second), "--previous", first, "--budget-until", str(self.UNTIL))
            self.assertEqual(code, 0, printed)

    def test_a_later_note_replaces_an_earlier_one(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second, tempfile.TemporaryDirectory() as third:
            generate(Path(first))
            self.note(Path(second), "--previous", first, "--budget-until", str(self.UNTIL))
            code, printed = self.note(Path(third), "--previous", second, "--budget-until", str(self.UNTIL + 3600))
            self.assertEqual(code, 0, printed)
            index = json.loads((Path(third) / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["budget_until"], dashboard.iso(datetime.fromtimestamp(self.UNTIL + 3600, UTC)))

    def test_the_next_run_that_reads_writes_an_index_without_the_note(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second, tempfile.TemporaryDirectory() as third:
            generate(Path(first))
            self.note(Path(second), "--previous", first, "--budget-until", str(self.UNTIL))
            code, printed = generate(Path(third), "--previous", second)
            self.assertEqual(code, 0, printed)
            self.assertNotIn("budget_until", json.loads((Path(third) / "index.json").read_text(encoding="utf-8")))
            # The finished release is still copied from the run before, as if the note had not been between.
            self.assertIn("v1.0.0: final, copied from the last run", printed)

    def test_with_nothing_to_repeat_it_fails_and_writes_nothing(self):
        def broken(change) -> str:  # noqa: ANN001 - a function that spoils a copy of the last run
            folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
            generate(folder)
            change(folder)
            return str(folder)

        def rewrite(folder: Path, edit) -> None:  # noqa: ANN001
            index = json.loads((folder / "index.json").read_text(encoding="utf-8"))
            edit(index)
            (folder / "index.json").write_text(json.dumps(index), encoding="utf-8")

        good = broken(lambda folder: None)
        cases = {
            "no --previous": ["--budget-until", str(self.UNTIL)],
            "an empty --previous": ["--previous", self.enterContext(tempfile.TemporaryDirectory()), "--budget-until", str(self.UNTIL)],
            "an index that isn't JSON": ["--previous", broken(lambda folder: (folder / "index.json").write_text("{not json", encoding="utf-8")), "--budget-until", str(self.UNTIL)],
            "an index in another schema": ["--previous", broken(lambda folder: rewrite(folder, lambda index: index.update(schema=dashboard.SCHEMA + 1))), "--budget-until", str(self.UNTIL)],
            "an index that names no release": ["--previous", broken(lambda folder: rewrite(folder, lambda index: index.update(releases=[]))), "--budget-until", str(self.UNTIL)],
            "a release that isn't a milestone's name": ["--previous", broken(lambda folder: rewrite(folder, lambda index: index["releases"][0].update(title="../v1.0.0"))), "--budget-until", str(self.UNTIL)],
            "a release whose file is gone": ["--previous", broken(lambda folder: (folder / "v1.1.0.json").unlink()), "--budget-until", str(self.UNTIL)],
            "a time in words": ["--previous", good, "--budget-until", "soon"],
            "a negative time": ["--previous", good, "--budget-until=-5"],
            "no time": ["--previous", good, "--budget-until", ""],
            "a time with a command after it": ["--previous", good, "--budget-until", "1800003600; echo hello"],
        }
        for name, args in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as parent:
                out = Path(parent) / "out"
                code, printed = self.note(out, *args)
                self.assertEqual(code, 1, printed)
                self.assertIn("::error title=Release dashboard::The budget note couldn't be written", printed)
                self.assertFalse(out.exists() and any(out.iterdir()), "nothing is written")


class FixtureTests(unittest.TestCase):
    """The page's test data is what the generator writes today, so the page can't be tested against a stale schema."""

    def test_the_committed_fixtures_are_the_generators_output(self):
        with tempfile.TemporaryDirectory() as out:
            code, printed = generate(Path(out))
            self.assertEqual(code, 0, printed)
            names = sorted(path.name for path in Path(out).iterdir())
            self.assertEqual(sorted(path.name for path in FIXTURES.glob("*.json")), names)
            for name in names:
                self.assertEqual(
                    json.loads((FIXTURES / name).read_text(encoding="utf-8")),
                    json.loads((Path(out) / name).read_text(encoding="utf-8")),
                    f"{name} is stale: python -X utf8 tests/tools/test_release_dashboard.py --write-fixtures",
                )

    def test_the_fixtures_hold_the_hostile_title(self):
        release = json.loads((FIXTURES / "v1.1.0.json").read_text(encoding="utf-8"))
        self.assertIn(HOSTILE_TITLE, [entry["title"] for entry in release["open_issues"]])


if __name__ == "__main__":
    if "--write-fixtures" in sys.argv:
        for stale in FIXTURES.glob("*.json"):
            stale.unlink()
        exit_code, output = generate(FIXTURES)
        print(output, end="")
        sys.exit(exit_code)
    unittest.main()
