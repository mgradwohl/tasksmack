#!/usr/bin/env python3
"""
tools/release-dashboard.py -- Generate the Release Command Center's data from the GitHub API (#4524).

Usage:
    python -I -X utf8 tools/release-dashboard.py --out DIR [--previous DIR] [--milestone vX.Y.Z ...]
    python -I -X utf8 tools/release-dashboard.py --out DIR --dump-raw DIR     # also save what the API returned
    python -I -X utf8 tools/release-dashboard.py --out DIR --from-raw DIR     # rebuild from a dump, no network
    python -I -X utf8 tools/release-dashboard.py --out DIR --previous DIR --budget-until SECONDS   # see below

docs/release/ on the docs site shows, for one release milestone, what still has to happen before it ships,
where each of those things stands and how fast it's moving. This writes the JSON that page reads: one
`<milestone>.json` for every `vX.Y.Z` milestone, and an `index.json` that lists them and holds the figures
the page compares between releases. .github/workflows/release-dashboard.yml runs it when an issue or main
changes and on a schedule (#4604, #4700), and publishes DIR to the data-only `dashboard-data` branch; nothing
generated is committed to main.

Nothing here is a second tracking system. Every figure is read from what the release process already keeps
(docs/dev/release-process.md, docs/dev/labels.md):

  - scope       the milestone's issues: when each joined the milestone, started and closed;
  - started     the `in progress` label's time, or else the first linked pull request's open time;
  - checklist   the task list in the release tracker issue (`[RELEASE X.Y] ... tracker`). An item that starts
                with an issue number is done when that issue closes, whether or not its box is ticked;
  - freeze      the first one-line tracker comment that says "feature freeze" ("I declare the feature freeze");
  - approval    the first one-line tracker comment, after the freeze, that approves the candidate or release
                ("Approved for v0.10.0 release");
  - main        `main-health.yml`'s `ci-red-main` issues: each one's open time is a red period;
  - CI          the watched workflows' latest runs and pass counts (main-health.yml's WATCHED list);
  - dry run     release.yml's latest manual run on the branch.

Three figures need a note:

  - time in review    from when a pull request was last marked ready for review (or opened, if it never was a
                      draft) to its merge;
  - pull request CI   ci.yml's pull request runs in the cycle, passed and failed, and those runs over the pull
                      requests merged in that time. Both are the whole repository's, and the runs include those
                      of pull requests still open, so it's runs per merge, not the runs one merged change took;
  - the flow lists    when each issue started and went to review, so the page can chart the open work by
                      where it stood.

What a run costs matters, because the workflow's token shares one hourly budget with every other workflow
in the repository. A closed milestone whose release is published is written once, marked `final`, and copied
from --previous on later runs. A planned release and the cycle's pass counts are read once an hour: 14 calls
for the branch's, and for a minor one search and two or more counts for the pull requests'. The latest runs
come from two listings, not a call per workflow. So a usual run is about 7 REST calls and 50 GraphQL points,
and an hour's runs, 12 at most, take about 100 and 600 of each budget's 5,000 (#4604, #4620: every run but
a manual one waits until the published data is five minutes old). Every run prints what it used and what's
left, and warns when that's under a quarter. The workflow doesn't start this script at all when under 100
REST calls or 150 GraphQL points are left: other workflows share the budgets, and it could only fail at its
first read (#4617).

When the budget is gone the page should say so, and when it returns (#4726). --budget-until does that with no
request at all: it writes --previous out again as it is, with the time in index.json's `budget_until`. Every
other field stays, `generated_at` too, so nothing takes it for new data. It writes nothing when --previous
already names that time (the page is told once for each hour), and exits 1 with a warning when --previous
can't be repeated or the time is over 75 minutes ahead (an hour's budget returns within the hour). The next
run that reads GitHub writes an index without the field.

The token is GH_TOKEN or GITHUB_TOKEN, or else `gh auth token`; it needs read access only. Needs nothing
beyond the standard library. Exits 1 when the API can't be read (nothing is written then, so a failed run
never publishes half a picture), 2 on bad usage.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Iterable, Iterator, Sequence
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

# Bump it when a field changes meaning or goes away: every finished release is then generated again, not
# copied. A new field needs no bump as long as docs/release/dashboard.js copes with a file that lacks it.
SCHEMA = 2  # 2: time in review, pull request CI and the flow lists, for every release (#4524 phase 2)
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REPO = "mgradwohl/tasksmack"
API = "https://api.github.com"
USER_AGENT = "release-dashboard"

MILESTONE_TITLE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
TRACKER_TITLE = re.compile(r"^\[RELEASE\b[^\]]*\]", re.IGNORECASE)
IN_PROGRESS = "in progress"
# The label's first day (maintainer, 2026-11-19): no earlier event can name it, so none is fetched.
IN_PROGRESS_SINCE = "2026-11-19T00:00:00Z"
RED_LABEL = "ci-red-main"
RED_TITLE = re.compile(r"^CI: (\S+) is red$")
RELEASE_WORKFLOW = "release.yml"
# The workflow whose pull request runs are "CI" to a contributor: every pull request runs it.
PR_CI_WORKFLOW = "ci.yml"
MAIN_HEALTH = Path(".github/workflows/main-health.yml")
SOAK_HOURS = 24  # docs/dev/release-process.md, stage 6
# How main-health.yml judges a watched workflow: by these events' runs, and these conclusions pass.
JUDGED_EVENTS = ("push", "schedule")
PASSING = ("success", "neutral")
# A listing of a branch's last 100 runs of one event that reaches back this far holds every workflow that
# runs on that event at all.
LISTING_REACH = timedelta(days=7)
# What changes slowly is read once an hour, not on every run: the pass counts for the whole cycle (14 calls),
# and a planned release, which nobody is working on yet.
SLOW_REFRESH = timedelta(minutes=55)
# Say so in the run's log when a run leaves less than this share of the hour's API budget: the workflow's
# token shares it with every other workflow in the repository (main-health.yml above all).
BUDGET_WARNING = 0.25
# The furthest ahead a budget can be said to return (--budget-until): GitHub's budgets are hourly, so a later
# time is a mistake, and the page would hold its warning about old data back until then (#4726).
BUDGET_NOTE_AHEAD = timedelta(minutes=75)
# Whose one-line tracker comments can declare the freeze or approve the candidate: anyone can comment on a
# public repository's issue, so an outsider's line is never read as either.
TRUSTED_AUTHORS = ("OWNER", "MEMBER", "COLLABORATOR")
# GitHub's list endpoints stop counting at a round number (2,500 seen on 2026-11-19); a count this high is
# split by date instead of trusted.
COUNT_CEILING = 1000

# docs/dev/labels.md's queue order, as one category per issue. The first label that matches decides.
CATEGORIES: tuple[tuple[str, Callable[[set[str]], bool]], ...] = (
    ("Security", lambda labels: "security" in labels),
    # "Severity beats kind": a sev:high or sev:medium item is taken with the bugs whatever else it's labelled.
    ("Bug", lambda labels: "bug" in labels or bool(labels & {"sev:high", "sev:medium"})),
    ("CI", lambda labels: "ci" in labels),
    ("Tooling", lambda labels: bool(labels & {"tooling", "release", "dependencies"})),
    ("Perf", lambda labels: "performance" in labels),
    ("Testing", lambda labels: "testing" in labels),
    ("Docs", lambda labels: "documentation" in labels),
    ("Refactor", lambda labels: "refactoring" in labels),
    ("Parity", lambda labels: "enhancement" in labels and "parity" in labels),
    ("Feature", lambda labels: "enhancement" in labels),
)
FEATURE_CATEGORIES = ("Parity", "Feature")
MAINTAINER_LABELS = ("question", "manual close")

# ------------------------------------------------------------------------------------------------
# Time
# ------------------------------------------------------------------------------------------------


def parse_time(text: str | None) -> datetime | None:
    if not text:
        return None
    return datetime.fromisoformat(text.replace("Z", "+00:00")).astimezone(UTC)


def iso(moment: datetime | None) -> str | None:
    return None if moment is None else moment.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def epoch(moment: datetime) -> int:
    return int(moment.timestamp())


def hours_between(start: datetime, end: datetime) -> float:
    return (end - start).total_seconds() / 3600.0


# ------------------------------------------------------------------------------------------------
# The GitHub API
# ------------------------------------------------------------------------------------------------


class GitHubError(RuntimeError):
    pass


class GitHub:
    """Read-only REST and GraphQL calls, each tried up to 4 times with backoff."""

    TRIES = 4

    def __init__(self, repo: str, token: str, api: str = API) -> None:
        self.repo = repo
        self.owner, self.name = repo.split("/", 1)
        self.token = token
        self.api = api
        self.rest_calls = 0
        self.graphql_calls = 0
        self.graphql_cost = 0
        # What's left of each hourly budget after the last call, as (remaining, limit); None until one answers.
        self.rest_budget: tuple[int, int] | None = None
        self.graphql_budget: tuple[int, int] | None = None

    def _send(self, url: str, body: dict[str, Any] | None) -> Any:
        data = None if body is None else json.dumps(body).encode("utf-8")
        # Always the GitHub API: self.api, with a path built here.
        request = urllib.request.Request(url, data=data, method="GET" if data is None else "POST")  # noqa: S310
        request.add_header("Authorization", f"Bearer {self.token}")
        request.add_header("Accept", "application/vnd.github+json")
        request.add_header("X-GitHub-Api-Version", "2022-11-28")
        request.add_header("User-Agent", USER_AGENT)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=60) as response:  # noqa: S310
            remaining, limit = response.headers.get("X-RateLimit-Remaining"), response.headers.get("X-RateLimit-Limit")
            if body is None and remaining and limit and remaining.isdigit() and limit.isdigit():
                self.rest_budget = (int(remaining), int(limit))
            return json.loads(response.read().decode("utf-8"))

    def _call(self, what: str, url: str, body: dict[str, Any] | None, check: Callable[[Any], Any]) -> Any:
        delay = 3.0
        problem = ""
        for attempt in range(1, self.TRIES + 1):
            try:
                return check(self._send(url, body))
            except urllib.error.HTTPError as error:
                problem = f"HTTP {error.code}"
                retry_after = error.headers.get("Retry-After") if error.headers else None
                limited = error.code in (403, 429) and (
                    retry_after is not None or (error.headers and error.headers.get("X-RateLimit-Remaining") == "0")
                )
                if not (error.code >= 500 or limited):
                    raise GitHubError(f"{what}: {problem}") from error
                if retry_after and retry_after.isdigit():
                    delay = max(delay, min(float(retry_after), 120.0))
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, GitHubError) as error:
                problem = str(error) or type(error).__name__
            if attempt < self.TRIES:
                print(f"{what}: try {attempt} of {self.TRIES} failed ({problem}); retrying", file=sys.stderr)
                time.sleep(delay)
                delay *= 2
        raise GitHubError(f"{what}: {problem}")

    def rest(self, path: str, **params: Any) -> Any:
        query = urllib.parse.urlencode({key: value for key, value in params.items() if value is not None})
        # A path from the API's root starts with a slash (the search endpoint); the rest are the repository's.
        base = f"{self.api}{path}" if path.startswith("/") else f"{self.api}/repos/{self.repo}/{path}"
        url = base + (f"?{query}" if query else "")
        self.rest_calls += 1
        return self._call(f"GET {path}", url, None, lambda reply: reply)

    def rest_list(self, path: str, **params: Any) -> list[dict[str, Any]]:
        """Every page of a REST list."""
        found: list[dict[str, Any]] = []
        page = 1
        while True:
            batch = self.rest(path, per_page=100, page=page, **params)
            found += batch
            if len(batch) < 100:
                return found
            page += 1

    def graphql(self, query: str, *, partial: bool = False, **variables: Any) -> dict[str, Any]:
        """One GraphQL query's data. @p partial takes an answer that has data and errors both: a field that
        names nothing comes back null with a NOT_FOUND error beside it, and the rest of the answer is good."""

        def check(reply: Any) -> Any:
            # GitHub answers some valid queries with "Something went wrong" (2026-11-19: Milestone.pullRequests
            # with orderBy), and a timeout the same way, so an error is retried and then reported.
            if reply.get("data") is None or (reply.get("errors") and not partial):
                messages = "; ".join(str(error.get("message", error)) for error in reply.get("errors") or [])
                raise GitHubError(messages or "no data")
            return reply["data"]

        variables = {"owner": self.owner, "name": self.name, **variables}
        self.graphql_calls += 1
        data = self._call("GraphQL", f"{self.api}/graphql", {"query": query, "variables": variables}, check)
        spent = data.get("rateLimit") or {}
        self.graphql_cost += spent.get("cost", 0)
        if isinstance(spent.get("remaining"), int) and isinstance(spent.get("limit"), int):
            self.graphql_budget = (spent["remaining"], spent["limit"])
        return data

    def budget(self) -> tuple[str, list[str]]:
        """What the run used and what's left this hour, as a line for the log, and a warning for each budget
        the run left under BUDGET_WARNING of."""
        parts = [f"{self.rest_calls} REST calls", f"{self.graphql_calls} GraphQL calls ({self.graphql_cost} points)"]
        low = []
        for name, budget in (("REST", self.rest_budget), ("GraphQL", self.graphql_budget)):
            if budget is None:
                continue
            remaining, limit = budget
            parts.append(f"{name}: {remaining} of {limit} left this hour")
            if limit and remaining < BUDGET_WARNING * limit:
                low.append(f"{name} budget is low: {remaining} of {limit} left this hour")
        return ", ".join(parts), low

    def graphql_nodes(
        self, query: str, path: Sequence[str], *, page: int = 50, **variables: Any
    ) -> Iterator[dict[str, Any]]:
        """Every node of the connection at @p path, @p page at a time. The query takes $first and $cursor.
        A page that keeps failing is asked for again at half the size, since a large one can time out."""
        cursor = None
        first = page
        while True:
            try:
                data = self.graphql(query, first=first, cursor=cursor, **variables)
            except GitHubError:
                if first <= 10:
                    raise
                first //= 2
                continue
            connection: Any = data
            for key in path:
                connection = connection[key]
            yield from connection["nodes"]
            if not connection["pageInfo"]["hasNextPage"]:
                return
            cursor = connection["pageInfo"]["endCursor"]


def find_token() -> str:
    for name in ("GH_TOKEN", "GITHUB_TOKEN"):
        if os.environ.get(name):
            return os.environ[name]
    gh = shutil.which("gh")
    if gh:
        result = subprocess.run([gh, "auth", "token"], capture_output=True, text=True, check=False)  # noqa: S603
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    raise GitHubError("no token: set GH_TOKEN, or sign in with `gh auth login`")


ISSUES_QUERY = """
query($owner:String!,$name:String!,$number:Int!,$first:Int!,$cursor:String,$since:DateTime!){
  rateLimit{cost remaining limit}
  repository(owner:$owner,name:$name){milestone(number:$number){issues(first:$first,after:$cursor){
    pageInfo{hasNextPage endCursor}
    nodes{
      number title state stateReason createdAt closedAt
      labels(first:40){nodes{name}}
      milestoned:timelineItems(itemTypes:[MILESTONED_EVENT],last:10){nodes{
        ... on MilestonedEvent{createdAt milestoneTitle}}}
      labeled:timelineItems(itemTypes:[LABELED_EVENT,UNLABELED_EVENT],since:$since,last:40){nodes{
        __typename
        ... on LabeledEvent{createdAt label{name}}
        ... on UnlabeledEvent{createdAt label{name}}}}
      reopened:timelineItems(itemTypes:[REOPENED_EVENT],first:1){totalCount}
      closedByPullRequestsReferences(first:10,includeClosedPrs:true){nodes{
        number state isDraft createdAt mergedAt closedAt additions deletions}}
    }}}}
}"""

# No orderBy: with one, GitHub answers "Something went wrong" (2026-11-19).
PULL_REQUESTS_QUERY = """
query($owner:String!,$name:String!,$number:Int!,$first:Int!,$cursor:String){
  rateLimit{cost remaining limit}
  repository(owner:$owner,name:$name){milestone(number:$number){pullRequests(first:$first,after:$cursor){
    pageInfo{hasNextPage endCursor}
    nodes{number state isDraft createdAt mergedAt closedAt additions deletions
      ready:timelineItems(itemTypes:[READY_FOR_REVIEW_EVENT],last:1){nodes{
        ... on ReadyForReviewEvent{createdAt}}}}}}}
}"""

OPEN_PULL_REQUESTS_QUERY = """
query($owner:String!,$name:String!,$first:Int!,$cursor:String){
  rateLimit{cost remaining limit}
  repository(owner:$owner,name:$name){
    pullRequests(states:OPEN,first:$first,after:$cursor,orderBy:{field:CREATED_AT,direction:ASC}){
    pageInfo{hasNextPage endCursor}
    nodes{
      number title isDraft createdAt baseRefName
      author{__typename login}
      milestone{title}
      labels(first:40){nodes{name}}
      mergeStateStatus
      autoMergeRequest{enabledAt}
      closingIssuesReferences(first:10){nodes{number}}
      reviewThreads(first:100){nodes{isResolved}}
      ready:timelineItems(itemTypes:[READY_FOR_REVIEW_EVENT],last:1){nodes{
        ... on ReadyForReviewEvent{createdAt}}}
      commits(last:1){nodes{commit{statusCheckRollup{state}}}}
    }}}
}"""

TRACKER_QUERY = """
query($owner:String!,$name:String!,$number:Int!,$first:Int!,$cursor:String){
  rateLimit{cost remaining limit}
  repository(owner:$owner,name:$name){issue(number:$number){
    number title body
    comments(first:$first,after:$cursor){
      pageInfo{hasNextPage endCursor}
      nodes{body createdAt url authorAssociation}}}}
}"""


def collect_tracker(gh: GitHub, number: int) -> dict[str, Any]:
    head = gh.graphql(TRACKER_QUERY, number=number, first=1, cursor=None)["repository"]["issue"]
    comments = list(gh.graphql_nodes(TRACKER_QUERY, ("repository", "issue", "comments"), number=number))
    return {"number": head["number"], "title": head["title"], "body": head["body"], "comments": comments}


def collect_states(gh: GitHub, numbers: Iterable[int]) -> dict[str, dict[str, Any]]:
    """Whether each issue or pull request in @p numbers is open, by number (as text: it's a JSON key)."""
    wanted = sorted(set(numbers))
    found: dict[str, dict[str, Any]] = {}
    for start in range(0, len(wanted), 40):
        chunk = wanted[start : start + 40]
        fields = " ".join(
            f"n{number}:issueOrPullRequest(number:{number}){{__typename "
            "... on Issue{state title} ... on PullRequest{state title}}"
            for number in chunk
        )
        query = (
            "query($owner:String!,$name:String!){rateLimit{cost} repository(owner:$owner,name:$name){" + fields + "}}"
        )
        # A number that names nothing (a typo in the tracker) comes back null, and the checklist shows it unknown.
        repository = gh.graphql(query, partial=True).get("repository") or {}
        for number in chunk:
            node = repository.get(f"n{number}")
            if node:
                found[str(number)] = {"state": node["state"], "title": node["title"], "type": node["__typename"]}
    return found


def collect_ready_times(gh: GitHub, numbers: Iterable[int]) -> dict[str, str | None]:
    """When each of @p numbers' pull requests was last marked ready for review, by number (as text: it's a JSON
    key); None for one that never was a draft. Forty to a query, at about a point each."""
    wanted = sorted(set(numbers))
    found: dict[str, str | None] = {}
    ready = "timelineItems(itemTypes:[READY_FOR_REVIEW_EVENT],last:1){nodes{... on ReadyForReviewEvent{createdAt}}}"
    for start in range(0, len(wanted), 40):
        chunk = wanted[start : start + 40]
        fields = " ".join(f"p{number}:pullRequest(number:{number}){{{ready}}}" for number in chunk)
        query = (
            "query($owner:String!,$name:String!){rateLimit{cost remaining limit} "
            "repository(owner:$owner,name:$name){" + fields + "}}"
        )
        repository = gh.graphql(query, partial=True).get("repository") or {}
        for number in chunk:
            nodes = ((repository.get(f"p{number}") or {}).get("timelineItems") or {}).get("nodes") or []
            found[str(number)] = (nodes[-1] or {}).get("createdAt") if nodes else None
    return found


def count_runs(
    gh: GitHub,
    workflow: str,
    branch: str | None,
    status: str,
    start: datetime,
    end: datetime,
    event: str | None = None,
) -> int:
    """How many of @p workflow's runs, created in [start, end], ended with @p status: on @p branch, or of
    @p event on any branch."""
    created = f"{iso(start)}..{iso(end)}"
    total = gh.rest(
        f"actions/workflows/{workflow}/runs", branch=branch, event=event, status=status, created=created, per_page=1
    )["total_count"]
    if total < COUNT_CEILING or end - start <= timedelta(hours=1):
        return total
    middle = start + (end - start) / 2
    return count_runs(gh, workflow, branch, status, start, middle, event) + count_runs(
        gh, workflow, branch, status, middle + timedelta(seconds=1), end, event
    )


def collect_pr_ci(gh: GitHub, start: datetime, end: datetime) -> dict[str, int]:
    """What CI cost the cycle's pull requests: how many of ci.yml's pull request runs passed and failed (a
    cancelled one was superseded by a push, and says nothing), and how many pull requests merged, all in
    [start, end]. Runs over merges is the CI the cycle spent for each change it landed.

    Both counts are the whole repository's, since a run doesn't say which branch its pull request targets. So
    this is asked for a minor only: a patch's cycle runs alongside the next minor's work, and the figure would
    be that work's."""
    merged = gh.rest("/search/issues", q=f"repo:{gh.repo} is:pr is:merged merged:{iso(start)}..{iso(end)}", per_page=1)[
        "total_count"
    ]
    return {
        "success": count_runs(gh, PR_CI_WORKFLOW, None, "success", start, end, "pull_request"),
        "failure": count_runs(gh, PR_CI_WORKFLOW, None, "failure", start, end, "pull_request"),
        "merged": merged,
    }


def collect_ci_counts(
    gh: GitHub, watched: Sequence[tuple[str, str]], branch: str, start: datetime, end: datetime
) -> list[dict[str, Any]]:
    return [
        {
            "file": file,
            "name": name,
            "success": count_runs(gh, file, branch, "success", start, end),
            "failure": count_runs(gh, file, branch, "failure", start, end),
        }
        for file, name in watched
    ]


def latest_runs(gh: GitHub, watched: Sequence[tuple[str, str]], branch: str, now: datetime) -> list[dict[str, Any]]:
    """Each watched workflow's latest finished push run and latest finished schedule run on @p branch, as
    main-health.yml's judge_branch judges a branch (#2131): the two apart, since some jobs only run nightly and
    a later push run would hide a red one. A cancelled or skipped run says nothing, and a manual dispatch isn't
    judged. (Its per-job rule for CI's Sunday unity build, WATCHED_JOBS, isn't repeated: the red-main issue
    carries that.)

    One listing per event covers every workflow, where one call per workflow and event took fourteen. A
    workflow missing from a listing that doesn't reach back LISTING_REACH is asked for by itself; missing from
    one that does, it has no such runs (Pre-commit and Sanitizers never run on a schedule)."""
    listings: dict[str, list[dict[str, Any]]] = {}
    reaches_back: dict[str, bool] = {}
    for event in JUDGED_EVENTS:
        found = gh.rest("actions/runs", branch=branch, event=event, status="completed", per_page=100)["workflow_runs"]
        listings[event] = found
        oldest = parse_time(found[-1]["created_at"]) if found else None
        reaches_back[event] = len(found) < 100 or (oldest is not None and now - oldest >= LISTING_REACH)
    latest = []
    for file, name in watched:
        runs = []
        for event in JUDGED_EVENTS:
            mine = [run for run in listings[event] if (run.get("path") or "").split("@")[0].endswith(f"/{file}")]
            if not mine and not reaches_back[event]:
                mine = gh.rest(
                    f"actions/workflows/{file}/runs", branch=branch, event=event, status="completed", per_page=10
                )["workflow_runs"]
            run = next((run for run in mine if run["conclusion"] not in ("cancelled", "skipped")), None)
            if run is not None:
                runs.append(_run_summary(run))
        latest.append({"file": file, "name": name, "runs": runs})
    return latest


def collect_live(gh: GitHub, watched: Sequence[tuple[str, str]], branch: str, now: datetime) -> dict[str, Any]:
    """What only an open release needs: the open pull requests, each watched workflow's latest finished runs
    on @p branch, and release.yml's manual runs there (a dry run: a real release runs on a tag)."""
    latest = latest_runs(gh, watched, branch, now)
    dispatched = gh.rest(
        f"actions/workflows/{RELEASE_WORKFLOW}/runs", branch=branch, event="workflow_dispatch", per_page=10
    )["workflow_runs"]
    dry_runs = [_run_summary(run) for run in dispatched]
    finished = next(
        (run for run in dry_runs if run["status"] == "completed" and run["conclusion"] != "cancelled"), None
    )
    behind = None
    if finished is not None:
        compared = gh.rest(f"compare/{finished['sha']}...{urllib.parse.quote(branch, safe='')}")
        behind = compared.get("ahead_by")
    return {
        "branch": branch,
        "head": branch_head(gh, branch),
        "open_pull_requests": list(gh.graphql_nodes(OPEN_PULL_REQUESTS_QUERY, ("repository", "pullRequests"))),
        "latest_runs": latest,
        "dry_runs": dry_runs,
        "dry_run_behind": behind,
    }


def branch_head(gh: GitHub, branch: str) -> dict[str, Any] | None:
    """@p branch's head commit and when it landed there, which the soak counts from (#4869): the first push run
    for that commit on the branch, since a merge queue's commit or a cherry-pick is dated before it lands, and the
    committer date only when no push run has started yet. None when the branch is gone (a retired release/X.Y)."""
    try:
        head = gh.rest(f"commits/{urllib.parse.quote(branch, safe='')}")
    except GitHubError as error:
        if "HTTP 404" in str(error) or "HTTP 422" in str(error):
            return None
        raise
    runs = gh.rest("actions/runs", branch=branch, event="push", head_sha=head["sha"], per_page=100)["workflow_runs"]
    landed = min((run["created_at"] for run in runs), default=None)
    return {"sha": head["sha"], "at": landed or head["commit"]["committer"]["date"]}


def _run_summary(run: dict[str, Any] | None) -> dict[str, Any] | None:
    if run is None:
        return None
    return {
        "id": run["id"],
        "status": run["status"],
        "conclusion": run["conclusion"],
        "event": run["event"],
        "sha": run["head_sha"],
        "created_at": run["created_at"],
        "updated_at": run["updated_at"],
    }


def collect_milestone(gh: GitHub, milestone: dict[str, Any]) -> dict[str, Any]:
    number = milestone["number"]
    issues = list(
        gh.graphql_nodes(ISSUES_QUERY, ("repository", "milestone", "issues"), number=number, since=IN_PROGRESS_SINCE)
    )
    # A flat query: a hundred a page costs one point, where the issues' nested lists cost three for fifty.
    pull_requests = list(
        gh.graphql_nodes(PULL_REQUESTS_QUERY, ("repository", "milestone", "pullRequests"), page=100, number=number)
    )
    tracker_number = find_tracker(issues)
    tracker = collect_tracker(gh, tracker_number) if tracker_number is not None else None
    known = {node["number"] for node in issues}
    referenced = {
        ref
        for section in parse_checklist(tracker["body"] if tracker else "")
        for item in section["items"]
        for ref in item["refs"]
    }
    # About a fifth of the pull requests that close a milestone's issues aren't on the milestone, and the list
    # under an issue doesn't say when each was marked ready: ask for those (merged or open; an abandoned one
    # isn't a review).
    on_milestone = {node["number"] for node in pull_requests}
    linked = {
        pr["number"]
        for node in issues
        for pr in node["closedByPullRequestsReferences"]["nodes"]
        if pr["state"] != "CLOSED"
    }
    return {
        "milestone": milestone,
        "issues": issues,
        "pull_requests": pull_requests,
        "pull_request_ready": collect_ready_times(gh, linked - on_milestone),
        "tracker": tracker,
        "states": collect_states(gh, referenced - known),
    }


# ------------------------------------------------------------------------------------------------
# Reading what was collected (no network below this line)
# ------------------------------------------------------------------------------------------------


def watched_workflows(main_health: str) -> list[tuple[str, str]]:
    """main-health.yml's WATCHED list, as (workflow file, display name): the workflows whose runs decide
    whether main is green, so the dashboard and the red-main issue judge the same set."""
    lines = main_health.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"^\s*WATCHED:\s*\|-\s*$", line):
            found = []
            for entry in lines[index + 1 :]:
                match = re.match(r"^\s+([\w.-]+\.ya?ml)\|(.+?)\s*$", entry)
                if match is None:
                    break
                found.append((match.group(1), match.group(2)))
            return found
    return []


def version_of(title: str) -> tuple[int, int, int] | None:
    match = MILESTONE_TITLE.match(title)
    return None if match is None else (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def branch_of(version: tuple[int, int, int]) -> str:
    """Where a release is built: a minor on main, a patch on its release branch (AGENTS.md, "Branches")."""
    return "main" if version[2] == 0 else f"release/{version[0]}.{version[1]}"


def published_releases(releases: Iterable[dict[str, Any]]) -> dict[tuple[int, int, int], dict[str, Any]]:
    found = {}
    for release in releases:
        version = version_of(release.get("tag_name") or "")
        if version is not None and not release.get("draft") and release.get("published_at"):
            found[version] = release
    return found


def cycle_window(
    milestone: dict[str, Any],
    releases: dict[tuple[int, int, int], dict[str, Any]],
    pending: Iterable[tuple[int, int, int]] = (),
) -> dict[str, Any]:
    """A release's cycle: from the release it follows to its own. A minor follows the minor before it, since
    main moves on to it at that cut; a patch follows the release before it on its line. With no earlier
    release, the cycle starts when the milestone was opened. @p pending is the versions with a milestone and
    no release yet: a release that follows one of those hasn't started, so it has no start (it's planned)."""
    version = version_of(milestone["title"])
    assert version is not None

    def precedes(found: tuple[int, int, int]) -> bool:
        if version[2] > 0:
            return found[:2] == version[:2] and found < version
        return found[2] == 0 and found < version

    release = releases.get(version)
    earlier = [found for found in {*releases, *pending} if precedes(found)]
    start: datetime | None
    if not earlier:
        start = parse_time(milestone["created_at"])
        reason = "the milestone was opened"
    else:
        before = max(earlier)
        name = f"v{before[0]}.{before[1]}.{before[2]}"
        if before in releases:
            start = parse_time(releases[before]["published_at"])
            reason = f"{name} was published"
        else:
            start = None
            reason = f"it starts when {name} is published"
    return {
        "start": start,
        "start_reason": reason,
        "end": parse_time(release["published_at"]) if release else None,
        "release_tag": release["tag_name"] if release else None,
    }


def category(labels: set[str]) -> str:
    for name, matches in CATEGORIES:
        if matches(labels):
            return name
    return "Other"


def lane(labels: set[str]) -> str:
    lanes = sorted(label.removeprefix("agent:") for label in labels if label.startswith("agent:"))
    if not lanes:
        return "None"
    return lanes[0] if len(lanes) == 1 else "Shared"


def level(labels: set[str], prefix: str) -> str | None:
    for name in ("high", "medium", "low"):
        if f"{prefix}:{name}" in labels:
            return name
    return None


def find_tracker(issues: Iterable[dict[str, Any]]) -> int | None:
    numbers = [
        issue["number"]
        for issue in issues
        if TRACKER_TITLE.match(issue["title"]) and "tracker" in issue["title"].lower()
    ]
    return min(numbers) if numbers else None


def _pull_request(node: dict[str, Any]) -> dict[str, Any]:
    return {
        "n": node["number"],
        "state": node["state"].lower(),  # open, merged or closed
        "draft": bool(node["isDraft"]),
        "created": parse_time(node["createdAt"]),
        "merged": parse_time(node["mergedAt"]),
        # When it was last marked ready for review; None if it never was a draft, or the query didn't ask.
        "ready": parse_time((((node.get("ready") or {}).get("nodes") or [{}])[-1] or {}).get("createdAt")),
        # Lines added and removed: the project asks for small pull requests (CONTRIBUTING.md).
        "lines": None if node.get("additions") is None else node["additions"] + (node.get("deletions") or 0),
    }


def review_start(pr: dict[str, Any], marked: dict[int, datetime]) -> datetime | None:
    """When a pull request became reviewable: when it was last marked ready for review, or when it was opened if
    it never was a draft. None while it's still a draft, and None for one closed unmerged: it was abandoned,
    and neither it nor its issue was "in review" on its account. @p marked holds the marked-ready times by
    number, for the pull requests whose own record doesn't carry one (those listed under an issue)."""
    if pr["state"] == "closed" or (pr["draft"] and pr["state"] == "open"):
        return None
    return marked.get(pr["n"]) or pr.get("ready") or pr["created"]


def flow_events(issues: Iterable[dict[str, Any]]) -> dict[str, list[int]]:
    """When each issue started and when it went to review, as sorted Unix seconds, for the page's chart of the
    open work over time. With the joined and done lists they nest: an issue is in a later list only if it's in
    the earlier ones, at a time no earlier, so the chart can stack the differences between them.

    A closed issue passes through every state, at its close if nothing recorded it sooner. An open one counts
    as started or in review only if it is so now: a pull request closed unmerged put it back, and these lists
    can't take an issue back out."""
    started: list[int] = []
    review: list[int] = []
    for issue in issues:
        joined, done = issue["joined"], issue["done"]
        if issue["open"]:
            # In progress with no time recorded (the label's event wasn't among those fetched): since it joined,
            # so the chart's last point agrees with the page's count of what's in progress.
            began = (issue["started"] or joined) if issue["phase"] in ("progress", "review") else None
            reviewed = (issue.get("review") or began) if issue["phase"] == "review" else None
        else:
            began = issue["started"] or done
            reviewed = issue.get("review") or done
        if began is None:
            continue
        began = max(began, joined)  # its pull request can be older than its place on this milestone
        if done is not None:
            began = min(began, done)
        started.append(epoch(began))
        if reviewed is None:
            continue
        reviewed = max(reviewed, began)
        if done is not None:
            reviewed = min(reviewed, done)
        review.append(epoch(reviewed))
    return {"started": sorted(started), "review": sorted(review)}


def _label_time(events: Iterable[dict[str, Any]], label: str) -> datetime | None:
    """When @p label was last added and not removed again."""
    added = None
    for event in sorted(events, key=lambda event: event.get("createdAt") or ""):
        if (event.get("label") or {}).get("name") != label:
            continue
        added = parse_time(event["createdAt"]) if event["__typename"] == "LabeledEvent" else None
    return added


def read_issue(node: dict[str, Any], milestone_title: str, tracker: int | None) -> dict[str, Any]:
    labels = {label["name"] for label in node["labels"]["nodes"]}
    created = parse_time(node["createdAt"])
    assert created is not None
    joined_events = [
        parse_time(event["createdAt"])
        for event in node["milestoned"]["nodes"]
        if event.get("milestoneTitle") == milestone_title
    ]
    joined = max([created, *[moment for moment in joined_events if moment is not None][-1:]])
    closed = parse_time(node["closedAt"]) if node["state"] == "CLOSED" else None
    pull_requests = [_pull_request(pr) for pr in node["closedByPullRequestsReferences"]["nodes"]]
    started_label = _label_time(node["labeled"]["nodes"], IN_PROGRESS) if IN_PROGRESS in labels else None
    started_pr = min((pr["created"] for pr in pull_requests), default=None)
    started = min((moment for moment in (started_label, started_pr) if moment is not None), default=None)
    open_prs = [pr for pr in pull_requests if pr["state"] == "open"]
    if closed is not None:
        phase = "done"
    elif any(not pr["draft"] for pr in open_prs):
        phase = "review"
    elif open_prs or IN_PROGRESS in labels:
        phase = "progress"
    else:
        phase = "todo"
    return {
        "n": node["number"],
        "title": node["title"],
        "open": closed is None,
        "completed": closed is not None and (node.get("stateReason") or "COMPLETED") == "COMPLETED",
        "created": created,
        "joined": joined,
        "closed": closed,
        # When it stopped being remaining work for this milestone.
        "done": None if closed is None else max(closed, joined),
        "labels": labels,
        "category": category(labels),
        "lane": lane(labels),
        "sev": level(labels, "sev"),
        "pri": level(labels, "pri"),
        "pull_requests": pull_requests,
        "started_label": started_label,
        "started_pr": started_pr,
        "started": started,
        "phase": phase,
        "reopened": ((node.get("reopened") or {}).get("totalCount") or 0) > 0,
        "tracker": node["number"] == tracker,
    }


def attributed_pull_requests(
    issues: Iterable[dict[str, Any]], on_milestone: Iterable[dict[str, Any]]
) -> list[dict[str, Any]]:
    """The release's pull requests: those on the milestone, and those that close one of its issues (about a
    fifth of the merged ones carry no milestone of their own)."""
    found = {node["number"]: _pull_request(node) for node in on_milestone}
    for issue in issues:
        for pr in issue["pull_requests"]:
            found.setdefault(pr["n"], pr)
    return sorted(found.values(), key=lambda pr: pr["n"])


# --- the tracker ---------------------------------------------------------------------------------

_HEADING = re.compile(r"^#{1,6}\s+(.*\S)\s*$")
_TASK = re.compile(r"^\s*[-*]\s+\[([ xX])\]\s+(.*\S)\s*$")
# Up to nine digits: an issue number. A longer one is something else (a workflow run's id), and as a GraphQL
# Int it would be refused and fail the whole lookup.
_REF = re.compile(r"(?<![\w/&#])#(\d{1,9})\b")
_LEAD_REF = re.compile(r"^[\s*_~`(\[]*#(\d{1,9})\b")
_FENCE = re.compile(r"^\s*(```|~~~)")


def _plain(text: str, limit: int = 200) -> str:
    text = re.sub(r"\*\*|__", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= limit else text[: limit - 1].rstrip() + "…"


def parse_checklist(body: str) -> list[dict[str, Any]]:
    """The task-list items in a tracker's body, grouped under the heading each follows."""
    sections: list[dict[str, Any]] = []
    heading = ""
    current: dict[str, Any] | None = None
    fenced = False
    for line in (body or "").splitlines():
        if _FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = _HEADING.match(line)
        if match:
            heading = _plain(match.group(1), 80)
            current = None
            continue
        match = _TASK.match(line)
        if match is None:
            continue
        if current is None:
            current = {"title": heading, "items": []}
            sections.append(current)
        text = match.group(2)
        lead = _LEAD_REF.match(text)
        current["items"].append(
            {
                "text": _plain(text),
                "checked": match.group(1) != " ",
                "refs": list(dict.fromkeys(int(number) for number in _REF.findall(text))),
                "lead": int(lead.group(1)) if lead else None,
            }
        )
    return sections


def resolve_checklist(sections: list[dict[str, Any]], states: dict[int, str]) -> list[dict[str, Any]]:
    """Each item's state. A ticked box is done. An item that starts with an issue number takes that issue's
    state (closed is done), so the tracker doesn't have to be ticked by hand; any other item is as its box
    says. @p states maps a number to done, review, progress or todo."""
    resolved = []
    for section in sections:
        items = []
        for item in section["items"]:
            state = "done" if item["checked"] else "todo"
            if not item["checked"] and item["lead"] is not None:
                state = states.get(item["lead"], "todo")
            items.append(
                {
                    "text": item["text"],
                    "state": state,
                    "ticked": item["checked"],
                    "lead": item["lead"],
                    "refs": [{"n": ref, "state": states.get(ref)} for ref in item["refs"][:8]],
                }
            )
        resolved.append({"title": section["title"], "items": items})
    return resolved


_SIGNAL_LIMIT = 120
_HEDGE = re.compile(
    r"\b(not|no|never|isn't|aren't|won't|don't|can't|before|after|until|when|once|if|ready|readiness|pending|"
    r"awaiting|waiting|should|could|would)\b|\?",
    re.IGNORECASE,
)
_FREEZE = re.compile(r"\bfeature freeze\b", re.IGNORECASE)
_DECLARES = re.compile(r"\bdeclare[ds]?\b|^\W*feature freeze\b", re.IGNORECASE)
_EXCEPTION = re.compile(r"#\d|\bexceptions?\b", re.IGNORECASE)
_APPROVAL = re.compile(r"\bapprove[ds]?\b", re.IGNORECASE)
_APPROVAL_OF = re.compile(r"\b(candidate|release)\b", re.IGNORECASE)


def _one_line_comments(comments: Iterable[dict[str, Any]]) -> Iterator[tuple[datetime, dict[str, Any], str]]:
    for comment in sorted(comments, key=lambda comment: comment["createdAt"]):
        text = (comment.get("body") or "").strip()
        moment = parse_time(comment["createdAt"])
        trusted = comment.get("authorAssociation") in TRUSTED_AUTHORS
        if moment is None or not trusted or not text or "\n" in text or len(text) > _SIGNAL_LIMIT:
            continue
        if not _HEDGE.search(text):
            yield moment, comment, text


def find_freeze(comments: Iterable[dict[str, Any]]) -> dict[str, Any] | None:
    """The freeze declaration: the first one-line comment, from someone who can write to the repository, that
    declares the feature freeze ("I declare the feature freeze") or opens with it ("Feature freeze."), and
    says nothing that hedges it. The lanes' readiness reports are long and say they aren't a declaration, and
    a line that only mentions the freeze ("all features merged for the feature freeze") isn't one either."""
    for moment, comment, text in _one_line_comments(comments):
        if _FREEZE.search(text) and _DECLARES.search(text):
            return {"at": moment, "url": comment.get("url"), "text": text}
    return None


def find_approval(comments: Iterable[dict[str, Any]], title: str, after: datetime | None) -> dict[str, Any] | None:
    """The candidate's approval: the first one-line comment after the freeze that approves the candidate, the
    release or this version. A bare "Approved" isn't one, and nor is a line that names an issue or an
    exception: the tracker also carries exception requests ("Approved: #4600 exception for v0.11.0")."""
    version = title.removeprefix("v")
    for moment, comment, text in _one_line_comments(comments):
        if after is not None and moment <= after:
            continue
        if _EXCEPTION.search(text):
            continue
        if _APPROVAL.search(text) and (_APPROVAL_OF.search(text) or version in text):
            return {"at": moment, "url": comment.get("url"), "text": text}
    return None


# --- main's health -------------------------------------------------------------------------------


def red_periods(red_issues: Iterable[dict[str, Any]], branch: str) -> list[dict[str, Any]]:
    """When @p branch was red: each `ci-red-main` issue for it, from opened to closed."""
    periods = []
    for issue in red_issues:
        match = RED_TITLE.match(issue.get("title") or "")
        start = parse_time(issue.get("created_at"))
        if match is None or match.group(1) != branch or start is None or "pull_request" in issue:
            continue
        periods.append({"n": issue["number"], "start": start, "end": parse_time(issue.get("closed_at"))})
    return sorted(periods, key=lambda period: period["start"])


def red_stats(periods: Iterable[dict[str, Any]], start: datetime, end: datetime) -> dict[str, Any]:
    """Red time inside [start, end]: how often, how long in all, and the longest stretch."""
    clipped = []
    for period in periods:
        begin = max(period["start"], start)
        finish = min(period["end"] or end, end)
        if finish > begin:
            clipped.append(hours_between(begin, finish))
    span = hours_between(start, end)
    total = sum(clipped)
    return {
        "count": len(clipped),
        "hours": round(total, 1),
        "longest_hours": round(max(clipped, default=0.0), 1),
        "percent": round(100.0 * total / span, 1) if span > 0 else 0.0,
    }


# --- figures -------------------------------------------------------------------------------------


def percentile(values: Sequence[float], fraction: float) -> float:
    """Linear interpolation between the closest ranks; @p values is sorted and not empty."""
    position = (len(values) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return values[low] + (values[high] - values[low]) * (position - low)


def spread(values: Iterable[float]) -> dict[str, Any] | None:
    """Count, median and 90th percentile (of hours, or of a pull request's lines). Medians, not means: a few
    issues carried for months would otherwise decide the figure. Negative values are left out."""
    ordered = sorted(value for value in values if value >= 0)
    if not ordered:
        return None
    return {"n": len(ordered), "median": round(percentile(ordered, 0.5), 1), "p90": round(percentile(ordered, 0.9), 1)}


def moved(items: Iterable[dict[str, Any]], now: datetime) -> dict[str, int]:
    """How a group moved lately: how many of it closed, and how many joined, in the last day and week."""
    counts = {"closed_24h": 0, "closed_7d": 0, "added_24h": 0, "added_7d": 0}
    day, week = now - timedelta(hours=24), now - timedelta(days=7)
    for item in items:
        if item["done"] is not None and item["done"] > week:
            counts["closed_7d"] += 1
            counts["closed_24h"] += item["done"] > day
        if item["joined"] > week:
            counts["added_7d"] += 1
            counts["added_24h"] += item["joined"] > day
    return counts


def breakdown(issues: Sequence[dict[str, Any]], key: str, now: datetime) -> list[dict[str, Any]]:
    """The issues grouped by @p key (lane or category): what's open in each group and where it stands, and
    how many closed and joined lately. A group with nothing open and nothing moving in a week is left out."""
    groups: dict[str, list[dict[str, Any]]] = {}
    for issue in issues:
        groups.setdefault(issue[key], []).append(issue)
    found = []
    for name, group in groups.items():
        entry = {
            "name": name,
            "open": sum(1 for issue in group if issue["open"]),
            **{phase: sum(1 for issue in group if issue["phase"] == phase) for phase in ("todo", "progress", "review")},
            **moved(group, now),
        }
        if entry["open"] or entry["closed_7d"] or entry["added_7d"]:
            found.append(entry)
    return sorted(found, key=lambda entry: entry["name"])


def label_gaps(issues: Iterable[dict[str, Any]]) -> dict[str, list[int]]:
    """Open issues missing a label docs/dev/labels.md requires: a lane, a priority (every open issue on a
    release milestone carries one), or a severity on a bug."""
    opened = [issue for issue in issues if issue["open"]]
    return {
        "no_lane": [issue["n"] for issue in opened if issue["lane"] == "None"],
        "no_priority": [issue["n"] for issue in opened if issue["pri"] is None],
        "no_severity": [issue["n"] for issue in opened if "bug" in issue["labels"] and issue["sev"] is None],
    }


def pace(issues: Sequence[dict[str, Any]], now: datetime, windows: Sequence[int] = (1, 3, 7)) -> list[dict[str, Any]]:
    """The net rate at which open issues are going away, over each of the last @p windows days, and how long
    the rest would take at that rate. It's arithmetic on the recent past, not a forecast: scope added later
    moves it, and a review sweep that files a hundred findings in a day turns it negative."""
    remaining = sum(1 for issue in issues if issue["open"])
    rates = []
    for days in windows:
        since = now - timedelta(days=days)
        closed = sum(1 for issue in issues if issue["done"] is not None and issue["done"] > since)
        added = sum(1 for issue in issues if issue["joined"] > since)
        net = (closed - added) / days
        rates.append(
            {
                "days": days,
                "closed_per_day": round(closed / days, 1),
                "added_per_day": round(added / days, 1),
                "net_per_day": round(net, 1),
                "days_left": 0.0 if not remaining else (round(remaining / net, 1) if net > 0 else None),
            }
        )
    return rates


def release_figures(
    issues: Sequence[dict[str, Any]],
    pull_requests: Sequence[dict[str, Any]],
    window: dict[str, Any],
    periods: Sequence[dict[str, Any]],
    ci_counts: Sequence[dict[str, Any]],
    now: datetime,
    tracked_from: datetime | None = None,
    pr_ci: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """One release's figures for the comparison table. "Started" is the first linked pull request's open time
    for every release, the `in progress` label's time for none: releases before the label have no other.
    @p tracked_from is when the milestone was opened; `partial` says it was opened well into the cycle, so
    the work before it isn't on the milestone and the counts understate the release (v0.9.0)."""
    done = [issue for issue in issues if not issue["open"]]
    if window["start"] is None:  # planned: nothing has happened yet
        return {"issues": len(issues), "issues_open": len(issues) - len(done), "issues_closed": len(done)}
    start, end = window["start"], window["end"] or now
    completed = [issue for issue in done if issue["completed"]]
    merged = [pr for pr in pull_requests if pr["merged"] is not None]
    days = hours_between(start, end) / 24.0
    bugs: dict[str, list[float]] = {"high": [], "medium": [], "low": []}
    for issue in completed:
        if issue["sev"] in bugs:
            bugs[issue["sev"]].append(hours_between(issue["created"], issue["closed"]))
    severities = {
        severity: {
            "found": sum(1 for issue in issues if issue["sev"] == severity and issue["created"] > start),
            "fixed": sum(1 for issue in completed if issue["sev"] == severity),
            "open": sum(1 for issue in issues if issue["sev"] == severity and issue["open"]),
        }
        for severity in bugs
    }
    success = sum(entry["success"] for entry in ci_counts)
    failure = sum(entry["failure"] for entry in ci_counts)
    late = hours_between(start, tracked_from) / 24.0 if tracked_from is not None else 0.0
    return {
        "days": round(days, 1),
        "partial": days > 0 and late > 0.25 * days,
        "tracked_days": round(days - max(0.0, late), 1),
        "issues": len(issues),
        "issues_open": len(issues) - len(done),
        "issues_closed": len(done),
        "issues_not_planned": len(done) - len(completed),
        "carried_in": sum(1 for issue in issues if issue["joined"] <= start),
        "added_during": sum(1 for issue in issues if issue["joined"] > start),
        "pull_requests_merged": len(merged),
        "pull_requests_unmerged": sum(1 for pr in pull_requests if pr["state"] == "closed"),
        "pull_request_lines": spread(pr["lines"] for pr in merged if pr["lines"] is not None),
        "reopened": sum(1 for issue in issues if issue["reopened"]),
        "severities": severities,
        "closed_per_day": round(len(done) / days, 1) if days > 0 else None,
        "lead_hours": spread(hours_between(issue["created"], issue["closed"]) for issue in completed),
        "queue_hours": spread(
            max(0.0, hours_between(issue["joined"], issue["started_pr"]))
            for issue in completed
            if issue["started_pr"] is not None
        ),
        "active_hours": spread(
            max(0.0, hours_between(issue["started_pr"], issue["closed"]))
            for issue in completed
            if issue["started_pr"] is not None
        ),
        "merge_hours": spread(hours_between(pr["created"], pr["merged"]) for pr in merged),
        # Ready for review to merged: the wait a finished change has, which opened-to-merged hides in the work.
        "review_hours": spread(
            hours_between(pr["review"], pr["merged"]) for pr in merged if pr.get("review") is not None
        ),
        "pr_ci": _pr_ci_figures(pr_ci),
        "bug_lead_hours": {severity: spread(values) for severity, values in bugs.items()},
        "red": red_stats(periods, start, end),
        "ci": {
            "success": success,
            "failure": failure,
            "pass_percent": round(100.0 * success / (success + failure), 1) if success + failure else None,
            "workflows": [
                {"name": entry["name"], "success": entry["success"], "failure": entry["failure"]} for entry in ci_counts
            ],
        },
    }


def _pr_ci_figures(counts: dict[str, Any] | None) -> dict[str, Any] | None:
    """Pull request CI for the cycle: its pass rate, and its runs per merge. The runs are every pull request's,
    merged or not, so pull requests held open through a freeze raise it: it isn't judged better or worse."""
    if not counts:
        return None
    success, failure, merged = counts.get("success", 0), counts.get("failure", 0), counts.get("merged", 0)
    return {
        "success": success,
        "failure": failure,
        "merged": merged,
        "pass_percent": round(100.0 * success / (success + failure), 1) if success + failure else None,
        "runs_per_merged": round((success + failure) / merged, 1) if merged else None,
    }


# --- the ship list -------------------------------------------------------------------------------


def _row(
    key: str,
    title: str,
    state: str,
    summary: str,
    *,
    note: str | None = None,
    issues: Iterable[dict[str, Any]] | None = None,
    moved_from: Iterable[dict[str, Any]] | None = None,
    now: datetime | None = None,
    link: str | None = None,
) -> dict[str, Any]:
    row: dict[str, Any] = {"id": key, "title": title, "state": state, "summary": summary}
    if note:
        row["note"] = note
    if issues is not None:
        row["issues"] = [issue["n"] for issue in issues]
    if moved_from is not None and now is not None:
        row["moved"] = moved(moved_from, now)
    if link:
        row["link"] = link
    return row


def _count(number: int, one: str, many: str | None = None) -> str:
    return f"{number} {one if number == 1 else (many or one + 's')}"


def ship_list(
    issues: Sequence[dict[str, Any]],
    checklist: Sequence[dict[str, Any]],
    signals: dict[str, Any],
    live: dict[str, Any] | None,
    window: dict[str, Any],
    has_tracker: bool,
    now: datetime,
) -> list[dict[str, Any]]:
    """What has to be true before the release ships, in the order docs/dev/release-process.md reaches it.
    Each row's state is done, active, todo or blocked."""
    opened = [issue for issue in issues if issue["open"]]
    rows = []
    if window["start"] is None:
        reason = window["start_reason"]
        note = reason[:1].upper() + reason[1:] + "."
        return [_row("scope", "Planned scope", "todo", _count(len(issues), "issue"), note=note)]

    rows.append(
        _row(
            "scope",
            "Milestone issues closed",
            "done" if not opened else "active",
            f"{len(opened)} open of {len(issues)}",
            moved_from=issues,
            now=now,
        )
    )

    severe = [issue for issue in issues if issue["sev"] in ("high", "medium")]
    severe_open = [issue for issue in severe if issue["open"]]
    high = sum(1 for issue in severe_open if issue["sev"] == "high")
    rows.append(
        _row(
            "severity",
            "No high-severity bug open",
            "blocked" if high else ("active" if severe_open else "done"),
            f"{high} high, {len(severe_open) - high} medium open",
            note="A high-severity bug holds the release. Medium ones may land after the freeze.",
            issues=sorted(severe_open, key=lambda issue: (issue["sev"] != "high", issue["n"])),
            moved_from=severe,
            now=now,
        )
    )

    features = [issue for issue in issues if issue["category"] in FEATURE_CATEGORIES]
    features_open = [issue for issue in features if issue["open"]]
    frozen = signals.get("freeze")
    rows.append(
        _row(
            "features",
            "Every feature merged, moved or cut",
            "done" if not features_open else ("blocked" if frozen else "active"),
            "none open" if not features_open else f"{_count(len(features_open), 'feature')} still open",
            note="The freeze can be declared once this reaches zero."
            if not frozen
            else "The freeze is declared: an open feature needs an exception, or moves to the next minor.",
            issues=features_open,
            moved_from=features,
            now=now,
        )
    )

    rows.append(
        _row(
            "freeze",
            "Feature freeze declared",
            "done" if frozen else "todo",
            f"declared {iso(frozen['at'])}" if frozen else "not declared",
            note=None if frozen else "Only the maintainer declares it, in a comment on the tracker.",
            link=frozen["url"] if frozen else None,
        )
    )

    review = [issue for issue in issues if "code-review" in issue["labels"]]
    review_open = [issue for issue in review if issue["open"]]
    rows.append(
        _row(
            "review",
            "Code-review findings closed",
            "done" if not review_open else "active",
            f"{len(review_open)} open of {len(review)}",
            note="A blocking finding stays on the milestone. The rest move to the next minor.",
            issues=review_open,
            moved_from=review,
            now=now,
        )
    )

    perf = [issue for issue in issues if "performance" in issue["labels"]]
    perf_open = [issue for issue in perf if issue["open"]]
    rows.append(
        _row(
            "performance",
            "Performance issues closed",
            "done" if not perf_open else "active",
            f"{len(perf_open)} open of {len(perf)}",
            note="Issues labelled `performance`. A regression against the cycle's baselines is a bug for this "
            "milestone; the others may move to the next minor.",
            issues=perf_open,
            moved_from=perf,
            now=now,
        )
    )

    items = [item for section in checklist for item in section["items"]]
    if has_tracker and items:
        finished = sum(1 for item in items if item["state"] == "done")
        rows.append(
            _row(
                "checklist",
                "Tracker checklist",
                "done" if finished == len(items) else ("active" if finished else "todo"),
                f"{finished} of {len(items)} done",
            )
        )

    waiting = [issue for issue in opened if issue["labels"] & set(MAINTAINER_LABELS)]
    rows.append(
        _row(
            "maintainer",
            "Nothing waiting on the maintainer",
            "done" if not waiting else "active",
            "nothing waiting" if not waiting else f"{_count(len(waiting), 'issue')} waiting",
            note="Open issues labelled `question` or `manual close`.",
            issues=waiting,
        )
    )

    if live is not None:
        scoped = [pr for pr in live["pull_requests"] if pr["in_scope"]]
        failing = sum(1 for pr in scoped if pr["checks"] in ("FAILURE", "ERROR"))
        drafts = sum(1 for pr in scoped if pr["draft"])
        summary = "none open"
        if scoped:
            waits = [hours_between(pr["ready_since"], now) for pr in scoped if pr.get("ready_since") is not None]
            longest = f" (longest {_duration(max(waits))})" if waits else ""
            summary = f"{len(scoped)} open: {len(scoped) - drafts} ready{longest}, {drafts} draft"
            if failing:
                summary += f", {failing} failing checks"
        rows.append(_row("pull_requests", "Pull requests merged", "done" if not scoped else "active", summary))

        health = live["main"]
        if health["state"] == "red":
            rows.append(
                _row(
                    "main",
                    f"{live['branch']} green for {SOAK_HOURS} hours",
                    "blocked",
                    f"red for {_duration(health['hours'])}",
                    note="The candidate needs every push check and nightly job green for a day, Heavy Checks' nightly run included.",
                    link=health.get("issue_url"),
                )
            )
        else:
            # Unknown is not soaked (#4869): a row with no start time stays active.
            soaked = health["hours"] is not None and health["hours"] >= SOAK_HOURS
            rows.append(
                _row(
                    "main",
                    f"{live['branch']} green for {SOAK_HOURS} hours",
                    "done" if soaked else "active",
                    "green, start unknown" if health["hours"] is None else f"green for {_duration(health['hours'])}",
                    note="The candidate needs every push check and nightly job green for a day, Heavy Checks' nightly run included.",
                )
            )

        dry = live["dry_run"]
        if dry is None:
            rows.append(
                _row(
                    "dry_run",
                    f"Signed release dry run on {live['branch']}",
                    "todo",
                    "none this cycle",
                    note=f"gh workflow run release.yml --ref {live['branch']} -f dry_run=true",
                )
            )
        else:
            behind = dry.get("behind")
            where = (
                "" if behind is None else (" at the head" if behind == 0 else f", {_count(behind, 'commit')} behind")
            )
            passed = dry["conclusion"] == "success"
            rows.append(
                _row(
                    "dry_run",
                    f"Signed release dry run on {live['branch']}",
                    "done" if passed and behind == 0 else ("active" if passed else "blocked"),
                    f"{'passed' if passed else dry['conclusion']} {iso(dry['at'])}{where}",
                    note="The candidate needs one on its exact commit.",
                    link=dry["url"],
                )
            )

    approved = signals.get("approval")
    rows.append(
        _row(
            "candidate",
            "Candidate approved",
            "done" if approved else "todo",
            f"approved {iso(approved['at'])}" if approved else "not approved",
            note=None if approved else "The maintainer approves one exact commit on the tracker.",
            link=approved["url"] if approved else None,
        )
    )

    released = window["end"]
    rows.append(
        _row(
            "published",
            "Release published",
            "done" if released else "todo",
            f"published {iso(released)}" if released else "not published",
        )
    )
    return rows


def _duration(hours: float | None) -> str:
    if hours is None:
        return "an unknown time"
    if hours < 1:
        return f"{max(1, round(hours * 60))} min"
    if hours < 48:
        return f"{round(hours)} h"
    return f"{round(hours / 24)} days"


def stage(signals: dict[str, Any], window: dict[str, Any]) -> str:
    if window["end"]:
        return "released"
    if window["start"] is None:
        return "planned"
    if signals.get("approval"):
        return "candidate"
    if signals.get("freeze"):
        return "freeze"
    return "building"


# --- putting a release together ------------------------------------------------------------------


def read_live(
    raw: dict[str, Any],
    issue_numbers: set[int],
    title: str,
    periods: Sequence[dict[str, Any]],
    repo: str,
    now: datetime,
) -> dict[str, Any]:
    pull_requests = []
    for node in raw["open_pull_requests"]:
        labels = {label["name"] for label in node["labels"]["nodes"]}
        closes = [closing["number"] for closing in node["closingIssuesReferences"]["nodes"]]
        commit = (node["commits"]["nodes"] or [{}])[-1].get("commit") or {}
        ready = (node["ready"]["nodes"] or [{}])[-1].get("createdAt")
        author = node.get("author") or {}
        pull_requests.append(
            {
                "n": node["number"],
                "title": node["title"],
                "draft": bool(node["isDraft"]),
                "created": parse_time(node["createdAt"]),
                "ready": parse_time(ready),
                "base": node["baseRefName"],
                "lane": lane(labels),
                "milestone": (node.get("milestone") or {}).get("title"),
                "closes": closes,
                "checks": (commit.get("statusCheckRollup") or {}).get("state"),
                "threads": sum(1 for thread in node["reviewThreads"]["nodes"] if not thread["isResolved"]),
                "auto_merge": node.get("autoMergeRequest") is not None,
                "merge_state": node.get("mergeStateStatus"),
                "bot": author.get("__typename") == "Bot",
                "in_scope": (node.get("milestone") or {}).get("title") == title
                or any(number in issue_numbers for number in closes),
            }
        )
        latest = pull_requests[-1]
        latest["waiting"] = waiting_on(latest)
        # Ready since it was marked ready, or since it was opened if it never was a draft.
        latest["ready_since"] = None if latest["draft"] else (latest["ready"] or latest["created"])

    open_period = next((period for period in reversed(periods) if period["end"] is None), None)
    if open_period is not None:
        main = {
            "state": "red",
            "since": open_period["start"],
            "hours": round(hours_between(open_period["start"], now), 1),
            "issue": open_period["n"],
            "issue_url": f"https://github.com/{repo}/issues/{open_period['n']}",
        }
    else:
        # Green since the later of the last red period's end and the head commit: a soak is on the candidate,
        # so a branch that was never red, or a new commit after a red period, starts the count again (#4869).
        ends = [period["end"] for period in periods if period["end"] is not None]
        head = raw.get("head")
        if head is not None:
            # A clock that ran fast can date a commit after now: count from now, never from the future.
            ends.append(min(parse_time(head["at"]), now))
        last = max(ends, default=None)
        main = {
            "state": "green",
            "since": last,
            "hours": None if last is None else round(hours_between(last, now), 1),
            "issue": None,
        }
    main["head"] = None if raw.get("head") is None else raw["head"]["sha"]
    main["workflows"] = [
        {
            "name": entry["name"],
            # Red while either its latest push run or its latest nightly run failed; none yet is neither.
            "passing": all(run["conclusion"] in PASSING for run in entry["runs"]) if entry["runs"] else None,
            "runs": [
                {"event": run["event"], "conclusion": run["conclusion"], "at": run["updated_at"], "run": run["id"]}
                for run in entry["runs"]
            ],
        }
        for entry in raw["latest_runs"]
    ]

    finished = next(
        (run for run in raw["dry_runs"] if run["status"] == "completed" and run["conclusion"] != "cancelled"), None
    )
    dry_run = None
    if finished is not None:
        dry_run = {
            "conclusion": finished["conclusion"],
            "at": parse_time(finished["updated_at"]),
            "sha": finished["sha"],
            "run": finished["id"],
            "url": f"https://github.com/{repo}/actions/runs/{finished['id']}",
            "behind": raw.get("dry_run_behind"),
            "running": any(run["status"] != "completed" for run in raw["dry_runs"]),
        }
    return {"branch": raw["branch"], "pull_requests": pull_requests, "main": main, "dry_run": dry_run}


def waiting_on(pr: dict[str, Any]) -> str:
    """What an open pull request is waiting on: the first of these that holds. A pull request that sits ready for
    hours is invisible until someone opens it, so the page says which kind of waiting it is."""
    if pr["draft"]:
        return "a draft"
    if pr["checks"] in ("FAILURE", "ERROR"):
        return "checks failing"
    if pr["merge_state"] == "DIRTY":
        return "conflicts with its base"
    if pr["threads"]:
        return _count(pr["threads"], "review thread") + " open"
    if pr["merge_state"] == "BEHIND":
        return "behind its base: needs a catch-up"
    if pr["checks"] in ("PENDING", "EXPECTED"):
        return "checks running"
    if pr["checks"] is None:
        return "no checks yet"
    if pr["merge_state"] == "BLOCKED":
        return "a required check or review"
    return "ready to merge"


def _stamps(moments: Iterable[datetime], now: datetime) -> list[int]:
    """@p moments as sorted Unix seconds, none later than @p now."""
    limit = epoch(now)
    return sorted(min(epoch(moment), limit) for moment in moments)


def _jsonable(value: Any) -> Any:
    if isinstance(value, datetime):
        return iso(value)
    if isinstance(value, dict):
        return {key: _jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    if isinstance(value, set):
        return sorted(value)
    return value


def build_release(
    raw: dict[str, Any],
    releases: dict[tuple[int, int, int], dict[str, Any]],
    red_issues: Sequence[dict[str, Any]],
    ci_counts: Sequence[dict[str, Any]],
    live_raw: dict[str, Any] | None,
    repo: str,
    now: datetime,
    pending: Iterable[tuple[int, int, int]] = (),
) -> dict[str, Any]:
    """One milestone's page data, from what collect_milestone() and the repository-wide calls returned."""
    milestone = raw["milestone"]
    title = milestone["title"]
    version = version_of(title)
    assert version is not None
    branch = branch_of(version)
    window = cycle_window(milestone, releases, pending)
    planned = window["start"] is None
    tracker = raw.get("tracker")
    tracker_number = tracker["number"] if tracker else None

    everything = [read_issue(node, title, tracker_number) for node in raw["issues"]]
    issues = [issue for issue in everything if not issue["tracker"]]
    pull_requests = attributed_pull_requests(issues, raw["pull_requests"])
    periods = red_periods(red_issues, branch)

    states: dict[int, str] = {issue["n"]: issue["phase"] for issue in everything}
    for number, state in (raw.get("states") or {}).items():
        states.setdefault(int(number), "todo" if state["state"] == "OPEN" else "done")
    checklist = resolve_checklist(parse_checklist(tracker["body"] if tracker else ""), states)

    comments = tracker["comments"] if tracker else []
    freeze = find_freeze(comments)
    # No approval without a freeze: a candidate can't be approved before one, so an earlier line isn't it.
    signals = {"freeze": freeze, "approval": find_approval(comments, title, freeze["at"]) if freeze else None}

    closed = milestone["state"] == "closed"
    live = None
    if live_raw is not None and not window["end"] and not planned:
        live = read_live(live_raw, {issue["n"] for issue in issues}, title, periods, repo, now)

    # When each pull request became reviewable, and so when each issue first went to review.
    marked = {pr["n"]: pr["ready"] for pr in pull_requests if pr["ready"] is not None}
    for number, moment in (raw.get("pull_request_ready") or {}).items():
        if moment:
            marked.setdefault(int(number), parse_time(moment))
    for pr in (live or {}).get("pull_requests", []):
        if pr["ready"] is not None:
            marked.setdefault(pr["n"], pr["ready"])
    for pr in pull_requests:
        pr["review"] = review_start(pr, marked)
    for issue in issues:
        reviews = [review_start(pr, marked) for pr in issue["pull_requests"]]
        issue["review"] = min((moment for moment in reviews if moment is not None), default=None)
    flow = flow_events(issues)

    end = window["end"] or now
    opened = sorted((issue for issue in issues if issue["open"]), key=lambda issue: issue["n"])
    recent = sorted(
        (issue for issue in issues if issue["done"] is not None and issue["done"] > now - timedelta(hours=24)),
        key=lambda issue: issue["done"],
        reverse=True,
    )
    closed_at = parse_time(milestone.get("closed_at"))
    return _jsonable(
        {
            "schema": SCHEMA,
            "generated_at": now,
            # Written once and then copied: a shipped release whose milestone has been closed for a day.
            "final": bool(closed and window["end"] and closed_at and now - closed_at > timedelta(days=1)),
            "repo": repo,
            "milestone": {
                "title": title,
                "number": milestone["number"],
                "state": milestone["state"],
                "due_on": parse_time(milestone.get("due_on")),
                "created_at": parse_time(milestone["created_at"]),
                "closed_at": closed_at,
            },
            "branch": branch,
            "stage": stage(signals, window),
            "cycle": {
                "start": window["start"],
                "start_reason": window["start_reason"],
                "end": window["end"],
                "release_tag": window["release_tag"],
            },
            "tracker": {"number": tracker_number, "title": tracker["title"]} if tracker else None,
            "signals": signals,
            "ship_list": ship_list(issues, checklist, signals, live, window, tracker is not None, now),
            "checklist": checklist,
            "pace": pace(issues, now) if not window["end"] and not planned else None,
            "counts": {
                "issues": len(issues),
                "open": len(opened),
                "phases": {
                    phase: sum(1 for issue in issues if issue["phase"] == phase)
                    for phase in ("todo", "progress", "review", "done")
                },
            },
            "by_lane": breakdown(issues, "lane", now),
            "by_category": breakdown(issues, "category", now),
            "label_gaps": label_gaps(issues),
            "open_issues": [
                {
                    "n": issue["n"],
                    "title": issue["title"],
                    "category": issue["category"],
                    "lane": issue["lane"],
                    "sev": issue["sev"],
                    "pri": issue["pri"],
                    "phase": issue["phase"],
                    "created": issue["created"],
                    "joined": issue["joined"],
                    "started": issue["started"],
                    "pull_requests": [pr["n"] for pr in issue["pull_requests"] if pr["state"] == "open"],
                }
                for issue in opened
            ],
            "recently_closed": [
                {"n": issue["n"], "title": issue["title"], "category": issue["category"], "at": issue["done"]}
                for issue in recent
            ],
            # Unix seconds, sorted: the page draws the burn-up and the daily bars from these, in local time.
            # None is later than the run's own time: an issue filed while the run was reading is in the counts
            # above, so it's in the charts too, or the two would disagree by it.
            "events": {
                "joined": _stamps((issue["joined"] for issue in issues), now),
                "done": _stamps((issue["done"] for issue in issues if issue["done"] is not None), now),
                "started": _stamps((issue["started"] for issue in issues if issue["started"] is not None), now),
                "merged": _stamps((pr["merged"] for pr in pull_requests if pr["merged"] is not None), now),
                # Nested with joined and done, for the chart of the open work: see flow_events().
                "flow_started": [min(moment, epoch(now)) for moment in flow["started"]],
                "flow_review": [min(moment, epoch(now)) for moment in flow["review"]],
            },
            "red_periods": [
                {"n": period["n"], "start": period["start"], "end": period["end"]}
                for period in periods
                if not planned and (period["end"] or now) > window["start"] and period["start"] < end
            ],
            "live": live,
            # The pass counts as counted, and when: the next run reuses them for SLOW_REFRESH.
            "ci_counts": list(ci_counts),
            "pr_ci_counts": raw.get("pr_ci"),
            "ci_counted_at": raw.get("ci_counted_at") or now,
            "figures": release_figures(
                issues,
                pull_requests,
                window,
                periods,
                ci_counts,
                now,
                parse_time(milestone["created_at"]),
                raw.get("pr_ci"),
            ),
        }
    )


def build_index(releases: Sequence[dict[str, Any]], repo: str, now: datetime) -> dict[str, Any]:
    """The list of releases, the one the page opens on, and each one's figures for the comparison table."""
    ordered = sorted(releases, key=lambda release: version_of(release["milestone"]["title"]) or (0, 0, 0))
    under_way = [release for release in ordered if release["stage"] not in ("released", "planned")]
    # The minor main is building comes first: an open patch milestone sorts lower and would otherwise take the
    # page for as long as it stays open. A patch is the page's release only when no minor is under way.
    minors = [release for release in under_way if (version_of(release["milestone"]["title"]) or (0, 0, 1))[2] == 0]
    current = (minors or under_way or ordered[-1:] or [None])[0]
    return {
        "schema": SCHEMA,
        "generated_at": iso(now),
        "repo": repo,
        "current": current["milestone"]["title"] if current else None,
        "releases": [
            {
                "title": release["milestone"]["title"],
                "state": release["milestone"]["state"],
                "stage": release["stage"],
                "start": release["cycle"]["start"],
                "end": release["cycle"]["end"],
                "open": release["counts"]["open"],
                "issues": release["counts"]["issues"],
                "figures": release["figures"],
            }
            for release in ordered
        ],
    }


# ------------------------------------------------------------------------------------------------
# The run
# ------------------------------------------------------------------------------------------------


def _load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _save(path: Path, value: Any, *, pretty: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(
        value, ensure_ascii=False, indent=1 if pretty else None, separators=None if pretty else (",", ":")
    )
    path.write_text(text + "\n", encoding="utf-8", newline="\n")


def republish_with_budget_note(previous: Path, out: Path, until: str, now: datetime) -> int:
    """The API budget is used up until @p until (seconds since 1970, as GitHub's rate_limit gives a reset), so
    nothing can be read: write the last run's files again as they are, with that time in index.json for the
    page to show (#4726). Makes no request.

    Writes nothing, and returns 0, when the last run's index already names that time: the page is told once
    for each hour the budget is gone. Returns 1 with a warning, having written nothing, when the last run
    can't be repeated or the time is further ahead than a budget's hour. That's a warning, not an error: the
    run it happens in has only a note to add, and the page is no worse off without it. The files go out as
    they are, whatever schema they are in, as long as they agree with each other: what is written here passes
    the workflow's "Check what was written", so a note never fails a run there either."""
    try:
        if not re.fullmatch(r"\d{9,11}", until):
            raise ValueError(f"--budget-until is seconds since 1970, not {until!r}")
        when = datetime.fromtimestamp(int(until), UTC)
        if when - now > BUDGET_NOTE_AHEAD:
            ahead = int(BUDGET_NOTE_AHEAD.total_seconds() // 60)
            raise ValueError(f"--budget-until {iso(when)} is over {ahead} minutes ahead, and a budget returns within its hour")
        index = _load(previous / "index.json")
        if not isinstance(index, dict) or not isinstance(index.get("releases"), list):
            raise ValueError(f"{previous / 'index.json'} isn't an index of releases")
        titles = [release["title"] for release in index["releases"]]
        # Only the files the index names, and only names a milestone can have: nothing else rides along.
        if not titles or any(not isinstance(title, str) or version_of(title) is None for title in titles):
            raise ValueError(f"{previous / 'index.json'} names no releases, or one that isn't vX.Y.Z")
        missing = [title for title in titles if not (previous / f"{title}.json").is_file()]
        if missing:
            raise ValueError(f"{previous} has no file for {', '.join(missing)}")
        # What the workflow's check holds every publish to. Data that would fail it is refused here, where
        # that is a warning.
        if index.get("current") not in titles:
            raise ValueError(f"{previous / 'index.json'} names no current release among its own")
        for title in titles:
            release = _load(previous / f"{title}.json")
            named = release.get("milestone") if isinstance(release, dict) else None
            if not isinstance(named, dict) or named.get("title") != title or release.get("schema") != index.get("schema"):
                raise ValueError(f"{previous / (title + '.json')} doesn't match index.json")
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        print(f"::warning title=Release dashboard::The budget note couldn't be written: {error}", file=sys.stderr)
        return 1
    if index.get("budget_until") == iso(when):
        print(f"The published data already says the API budget is used up until {iso(when)}: nothing to write again.")
        return 0
    out.mkdir(parents=True, exist_ok=True)
    for title in titles:
        shutil.copyfile(previous / f"{title}.json", out / f"{title}.json")
    index["budget_until"] = iso(when)
    _save(out / "index.json", index)
    print(f"The API budget is used up until {iso(when)}: the data of {index.get('generated_at')} goes out again with that note.")
    return 0


def earlier(previous: Path | None, title: str) -> dict[str, Any] | None:
    """The earlier run's file for @p title, when there is one in this schema."""
    if previous is None:
        return None
    try:
        found = _load(previous / f"{title}.json")
    except (OSError, ValueError):
        return None
    return found if isinstance(found, dict) and found.get("schema") == SCHEMA else None


def _recent(found: dict[str, Any], key: str, now: datetime) -> bool:
    value = found.get(key)
    try:
        then = parse_time(value) if isinstance(value, str) else None
    except ValueError:
        return False
    return then is not None and timedelta(0) <= now - then < SLOW_REFRESH


def reusable(found: dict[str, Any] | None, window: dict[str, Any], now: datetime) -> bool:
    """Whether the earlier run's file stands for this run too: a finished release always, and a planned one
    for SLOW_REFRESH, as long as it's still planned (the release before it hasn't shipped since)."""
    if found is None:
        return False
    if found.get("final") is True:
        return True
    return found.get("stage") == "planned" and window["start"] is None and _recent(found, "generated_at", now)


def earlier_ci_counts(
    found: dict[str, Any] | None, window: dict[str, Any], now: datetime
) -> tuple[list[dict[str, Any]], dict[str, Any]] | None:
    """The earlier run's pass counts, the branch's and the pull requests', when both were counted within
    SLOW_REFRESH for the same cycle."""
    if found is None or not _recent(found, "ci_counted_at", now):
        return None
    counts, pr_counts = found.get("ci_counts"), found.get("pr_ci_counts")
    same_cycle = (found.get("cycle") or {}).get("start") == iso(window["start"])
    if same_cycle and isinstance(counts, list) and counts and isinstance(pr_counts, dict):
        return counts, pr_counts
    return None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").strip().splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="where to write index.json and <milestone>.json")
    parser.add_argument("--previous", type=Path, help="the last run's output: finished releases are copied from it")
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or DEFAULT_REPO, help="owner/name")
    parser.add_argument("--milestone", action="append", default=[], help="only this milestone (repeatable)")
    parser.add_argument("--dump-raw", type=Path, help="also save what the API returned, for --from-raw")
    parser.add_argument("--from-raw", type=Path, help="build from a --dump-raw folder; no network")
    parser.add_argument("--now", help="the time to compute against, ISO 8601 (default: now, or the dump's time)")
    parser.add_argument("--main-health", type=Path, default=REPO_ROOT / MAIN_HEALTH, help=argparse.SUPPRESS)
    parser.add_argument(
        "--budget-until",
        metavar="SECONDS",
        help="read nothing: write --previous again with this time (seconds since 1970) as index.json's budget_until",
    )
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo):
        parser.error(f"--repo must be owner/name, not {args.repo!r}")
    if args.budget_until is not None:
        # It reads nothing and builds nothing, so a flag that says what to read or build would be ignored.
        ignored = [flag for flag, given in (("--from-raw", args.from_raw), ("--dump-raw", args.dump_raw),
                                            ("--milestone", args.milestone)) if given]
        if ignored:
            parser.error(f"--budget-until writes --previous again as it is: it can't be given with {', '.join(ignored)}")
        if args.previous is None:
            parser.error("--budget-until needs --previous, the data to publish again")
        try:
            judged_at = parse_time(args.now) if args.now else datetime.now(UTC)
        except ValueError:
            parser.error(f"--now must be ISO 8601, not {args.now!r}")
        assert judged_at is not None
        return republish_with_budget_note(args.previous, args.out, args.budget_until, judged_at)

    try:
        if args.from_raw:
            repo_raw = _load(args.from_raw / "repository.json")
            now = parse_time(args.now or repo_raw["collected_at"])
            gh = None
        else:
            gh = GitHub(args.repo, find_token())
            now = parse_time(args.now) if args.now else datetime.now(UTC)
            watched = watched_workflows(args.main_health.read_text(encoding="utf-8"))
            if not watched:
                raise GitHubError(f"no WATCHED workflows found in {args.main_health}")
            repo_raw = {
                "collected_at": iso(now),
                "watched": watched,
                "milestones": gh.rest_list("milestones", state="all"),
                "releases": gh.rest_list("releases"),
                "live": {},
            }
        assert now is not None
        releases = published_releases(repo_raw["releases"])
        pending = {
            version
            for version in (version_of(milestone["title"]) for milestone in repo_raw["milestones"])
            if version is not None and version not in releases
        }
        milestones = [
            milestone
            for milestone in repo_raw["milestones"]
            if version_of(milestone["title"]) is not None
            and (not args.milestone or milestone["title"] in args.milestone)
        ]
        watched = [tuple(entry) for entry in repo_raw["watched"]]

        milestones.sort(key=lambda milestone: version_of(milestone["title"]) or (0, 0, 0))
        windows = {milestone["title"]: cycle_window(milestone, releases, pending) for milestone in milestones}
        before = {milestone["title"]: earlier(args.previous, milestone["title"]) for milestone in milestones}
        kept = {title: found if reusable(found, windows[title], now) else None for title, found in before.items()}
        if gh is not None:
            # Only the red periods the releases being rebuilt can overlap: the list grows by dozens a cycle.
            starts = [windows[title]["start"] for title, found in kept.items() if found is None]
            oldest = min((start for start in starts if start is not None), default=now)
            repo_raw["red_issues"] = gh.rest_list(
                "issues", labels=RED_LABEL, state="all", since=iso(oldest - timedelta(days=1))
            )

        built: list[dict[str, Any]] = []
        for milestone in milestones:
            title = milestone["title"]
            if kept[title] is not None:
                print(f"{title}: {'final' if kept[title].get('final') else 'planned'}, copied from the last run")
                built.append(kept[title])
                continue
            version = version_of(title)
            assert version is not None
            branch = branch_of(version)
            window = windows[title]
            if gh is not None:
                raw = collect_milestone(gh, milestone)
                raw["ci_counts"], raw["pr_ci"], raw["ci_counted_at"] = [], None, iso(now)
                if window["start"] is not None:
                    counted = earlier_ci_counts(before[title], window, now)
                    if counted is not None:
                        raw["ci_counts"], raw["pr_ci"] = counted
                        raw["ci_counted_at"] = before[title]["ci_counted_at"]
                    else:
                        until = window["end"] or now
                        raw["ci_counts"] = collect_ci_counts(gh, watched, branch, window["start"], until)
                        raw["pr_ci"] = collect_pr_ci(gh, window["start"], until) if version[2] == 0 else {}
                    if not window["end"] and branch not in repo_raw["live"]:
                        repo_raw["live"][branch] = collect_live(gh, watched, branch, now)
                if args.dump_raw:
                    _save(args.dump_raw / f"{title}.json", raw, pretty=True)
            else:
                raw = _load(args.from_raw / f"{title}.json")
            release = build_release(
                raw,
                releases,
                repo_raw["red_issues"],
                raw["ci_counts"],
                repo_raw["live"].get(branch),
                args.repo,
                now,
                pending,
            )
            print(
                f"{title}: {release['stage']}, {release['counts']['open']} open of {release['counts']['issues']} issues"
            )
            built.append(release)
        if gh is not None and args.dump_raw:
            _save(args.dump_raw / "repository.json", repo_raw, pretty=True)
    except (GitHubError, OSError, json.JSONDecodeError) as error:
        # The API couldn't be read, or a --from-raw dump couldn't.
        print(f"::error title=Release dashboard::{error}", file=sys.stderr)
        return 1

    # Everything was read, so write: a failure above leaves the last run's files as they were.
    for release in built:
        _save(args.out / f"{release['milestone']['title']}.json", release)
    _save(args.out / "index.json", build_index(built, args.repo, now))
    if gh is not None:
        used, low = gh.budget()
        print(used)
        for warning in low:
            print(f"::warning title=Release dashboard::{warning}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
