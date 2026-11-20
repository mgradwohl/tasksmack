#!/usr/bin/env python3
"""
tools/release-dashboard.py -- Generate the Release Command Center's data from the GitHub API (#4524).

Usage:
    python -I -X utf8 tools/release-dashboard.py --out DIR [--previous DIR] [--milestone vX.Y.Z ...]
    python -I -X utf8 tools/release-dashboard.py --out DIR --dump-raw DIR     # also save what the API returned
    python -I -X utf8 tools/release-dashboard.py --out DIR --from-raw DIR     # rebuild from a dump, no network

docs/release/ on the docs site shows, for one release milestone, what still has to happen before it ships,
where each of those things stands and how fast it's moving. This writes the JSON that page reads: one
`<milestone>.json` for every `vX.Y.Z` milestone, and an `index.json` that lists them and holds the figures
the page compares between releases. .github/workflows/release-dashboard.yml runs it every 30 minutes and
publishes DIR to the data-only `dashboard-data` branch; nothing generated is committed to main.

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

A closed milestone whose release is published is written once, marked `final`, and copied from --previous
on later runs, so a run costs about 30 REST calls and 40 GraphQL points whatever the history's size.

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
SCHEMA = 1
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
MAIN_HEALTH = Path(".github/workflows/main-health.yml")
SOAK_HOURS = 24  # docs/dev/release-process.md, stage 6
# How main-health.yml judges a watched workflow: by these events' runs, and these conclusions pass.
JUDGED_EVENTS = ("push", "schedule")
PASSING = ("success", "neutral")
# Whose one-line tracker comments can declare the freeze or approve the candidate: anyone can comment on a
# public repository's issue, so an outsider's line is never read as either.
TRUSTED_AUTHORS = ("OWNER", "MEMBER", "COLLABORATOR")
# GitHub's list endpoints stop counting at a round number (2,500 seen on 2026-11-19); a count this high is
# split by date instead of trusted.
COUNT_CEILING = 1000

# docs/dev/labels.md's queue order, as one category per issue. The first label that matches decides.
CATEGORIES: tuple[tuple[str, Callable[[set[str]], bool]], ...] = (
    ("Security", lambda labels: "security" in labels),
    ("Bug", lambda labels: "bug" in labels),
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
        url = f"{self.api}/repos/{self.repo}/{path}" + (f"?{query}" if query else "")
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
        self.graphql_cost += (data.get("rateLimit") or {}).get("cost", 0)
        return data

    def graphql_nodes(self, query: str, path: Sequence[str], **variables: Any) -> Iterator[dict[str, Any]]:
        """Every node of the connection at @p path, a page at a time. The query takes $first and $cursor.
        A page that keeps failing is asked for again at half the size, since a large one can time out."""
        cursor = None
        first = 50
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
  rateLimit{cost}
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
  rateLimit{cost}
  repository(owner:$owner,name:$name){milestone(number:$number){pullRequests(first:$first,after:$cursor){
    pageInfo{hasNextPage endCursor}
    nodes{number state isDraft createdAt mergedAt closedAt additions deletions}}}}
}"""

OPEN_PULL_REQUESTS_QUERY = """
query($owner:String!,$name:String!,$first:Int!,$cursor:String){
  rateLimit{cost}
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
  rateLimit{cost}
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


def count_runs(gh: GitHub, workflow: str, branch: str, status: str, start: datetime, end: datetime) -> int:
    """How many of @p workflow's runs on @p branch, created in [start, end], ended with @p status."""
    created = f"{iso(start)}..{iso(end)}"
    total = gh.rest(f"actions/workflows/{workflow}/runs", branch=branch, status=status, created=created, per_page=1)[
        "total_count"
    ]
    if total < COUNT_CEILING or end - start <= timedelta(hours=1):
        return total
    middle = start + (end - start) / 2
    return count_runs(gh, workflow, branch, status, start, middle) + count_runs(
        gh, workflow, branch, status, middle + timedelta(seconds=1), end
    )


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


def collect_live(gh: GitHub, watched: Sequence[tuple[str, str]], branch: str) -> dict[str, Any]:
    """What only an open release needs: the open pull requests, each watched workflow's latest finished run
    on @p branch, and release.yml's manual runs there (a dry run: a real release runs on a tag)."""
    latest = []
    for file, name in watched:
        # As main-health.yml's judge_branch does (#2131): the latest finished push run and the latest finished
        # schedule run, apart, since some jobs only run nightly and a later push run would hide a red one. A
        # cancelled or skipped run says nothing, and a manual dispatch isn't judged. (Its per-job rule for
        # CI's Sunday unity build, WATCHED_JOBS, isn't repeated here: the red-main issue carries that.)
        runs = []
        for event in JUDGED_EVENTS:
            found = gh.rest(
                f"actions/workflows/{file}/runs", branch=branch, event=event, status="completed", per_page=10
            )
            run = next(
                (run for run in found["workflow_runs"] if run["conclusion"] not in ("cancelled", "skipped")), None
            )
            if run is not None:
                runs.append(_run_summary(run))
        latest.append({"file": file, "name": name, "runs": runs})
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
        "open_pull_requests": list(gh.graphql_nodes(OPEN_PULL_REQUESTS_QUERY, ("repository", "pullRequests"))),
        "latest_runs": latest,
        "dry_runs": dry_runs,
        "dry_run_behind": behind,
    }


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
    pull_requests = list(
        gh.graphql_nodes(PULL_REQUESTS_QUERY, ("repository", "milestone", "pullRequests"), number=number)
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
    return {
        "milestone": milestone,
        "issues": issues,
        "pull_requests": pull_requests,
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
        # Lines added and removed: the project asks for small pull requests (CONTRIBUTING.md).
        "lines": None if node.get("additions") is None else node["additions"] + (node.get("deletions") or 0),
    }


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
            summary = f"{len(scoped)} open: {len(scoped) - drafts} ready, {drafts} draft"
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
                    note="The candidate needs every push check and nightly job green for a day.",
                    link=health.get("issue_url"),
                )
            )
        else:
            soaked = health["hours"] is None or health["hours"] >= SOAK_HOURS
            rows.append(
                _row(
                    "main",
                    f"{live['branch']} green for {SOAK_HOURS} hours",
                    "done" if soaked else "active",
                    "green" if health["hours"] is None else f"green for {_duration(health['hours'])}",
                    note="The candidate needs every push check and nightly job green for a day.",
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
        last = max((period["end"] for period in periods if period["end"] is not None), default=None)
        main = {
            "state": "green",
            "since": last,
            "hours": None if last is None else round(hours_between(last, now), 1),
            "issue": None,
        }
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
            "events": {
                "joined": sorted(epoch(issue["joined"]) for issue in issues),
                "done": sorted(epoch(issue["done"]) for issue in issues if issue["done"] is not None),
                "started": sorted(epoch(issue["started"]) for issue in issues if issue["started"] is not None),
                "merged": sorted(epoch(pr["merged"]) for pr in pull_requests if pr["merged"] is not None),
            },
            "red_periods": [
                {"n": period["n"], "start": period["start"], "end": period["end"]}
                for period in periods
                if not planned and (period["end"] or now) > window["start"] and period["start"] < end
            ],
            "live": live,
            "figures": release_figures(
                issues, pull_requests, window, periods, ci_counts, now, parse_time(milestone["created_at"])
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


def reusable(previous: Path | None, title: str) -> dict[str, Any] | None:
    """The earlier run's file for @p title, when it's a finished release in this schema."""
    if previous is None:
        return None
    path = previous / f"{title}.json"
    try:
        found = _load(path)
    except (OSError, ValueError):
        return None
    if isinstance(found, dict) and found.get("schema") == SCHEMA and found.get("final") is True:
        return found
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
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo):
        parser.error(f"--repo must be owner/name, not {args.repo!r}")

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
        kept = {milestone["title"]: reusable(args.previous, milestone["title"]) for milestone in milestones}
        windows = {milestone["title"]: cycle_window(milestone, releases, pending) for milestone in milestones}
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
                print(f"{title}: final, copied from the last run")
                built.append(kept[title])
                continue
            version = version_of(title)
            assert version is not None
            branch = branch_of(version)
            window = windows[title]
            if gh is not None:
                raw = collect_milestone(gh, milestone)
                raw["ci_counts"] = []
                if window["start"] is not None:
                    raw["ci_counts"] = collect_ci_counts(gh, watched, branch, window["start"], window["end"] or now)
                    if not window["end"] and branch not in repo_raw["live"]:
                        repo_raw["live"][branch] = collect_live(gh, watched, branch)
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
        print(f"{gh.rest_calls} REST calls, {gh.graphql_calls} GraphQL calls ({gh.graphql_cost} points)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
