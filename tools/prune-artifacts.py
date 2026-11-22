#!/usr/bin/env python3
"""Delete the repository's superseded Actions artifacts (#4617): every copy of a name but the newest on its branch.

Each CI run uploads artifacts under the same names (windows-debug-test-results, coverage-report, ...), so the
repository held about 6,600, nearly all of them older copies. ClusterFuzzLite's PR run lists every artifact for each
corpus lookup, about 42 REST calls per 100 artifacts, and one run emptied the workflow token's hourly budget for every
workflow (#4617). artifact-prune.yml runs this nightly so the list stays near one copy per name and branch.

Kept:
- the newest copy of each name on each branch (ClusterFuzzLite's cifuzz-* corpora and builds: of each name, since its
  action looks them up by name alone);
- anything newer than --min-age-hours (default 6), so a run still going keeps what it uploaded for a later job;
- benchmark* on every branch: the benchmark gate reads main's runs' results. (release-dashboard-data was kept too until
  #4720 stopped the dashboard uploading it on main; nothing read one after its own run, so its old copies go like any.)

The maintainer chose this rule on 2026-11-21 (#4617). The selection is a pure function (select_for_deletion), tested
by tests/tools/test_prune_artifacts.py; the listing and deleting go through the gh CLI, as the workflow's token.

Usage: prune-artifacts.py --repo OWNER/NAME [--dry-run] [--min-age-hours 6] [--max-deletions 3000] [--budget-floor 1500]
"""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
import time
from collections import defaultdict
from typing import Iterable

KEEP_PREFIXES = ("benchmark",)
BY_NAME_ONLY_PREFIX = "cifuzz-"


def select_for_deletion(artifacts: Iterable[dict], now: datetime.datetime, min_age_hours: float) -> list[dict]:
    """The artifacts to delete: each one that isn't the newest of its name on its branch (of its name alone for
    cifuzz-*), is at least min_age_hours old, isn't expired already, and isn't a kept family. Oldest first."""
    groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for artifact in artifacts:
        if artifact.get("expired"):
            continue
        name = artifact["name"]
        if name.startswith(KEEP_PREFIXES):
            continue
        branch = "*" if name.startswith(BY_NAME_ONLY_PREFIX) else (artifact.get("branch") or "")
        groups[(name, branch)].append(artifact)
    chosen = []
    for copies in groups.values():
        copies.sort(key=lambda a: a["created_at"], reverse=True)
        for artifact in copies[1:]:
            created = datetime.datetime.fromisoformat(artifact["created_at"].replace("Z", "+00:00"))
            if (now - created).total_seconds() >= min_age_hours * 3600:
                chosen.append(artifact)
    chosen.sort(key=lambda a: a["created_at"])
    return chosen


def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def list_artifacts(repo: str) -> list[dict]:
    """Every artifact, page by page (gh --paginate stopped short on a list this long, #4617)."""
    artifacts = []
    page = 1
    while True:
        out = gh("api", f"repos/{repo}/actions/artifacts?per_page=100&page={page}")
        batch = json.loads(out).get("artifacts", [])
        for item in batch:
            artifacts.append(
                {
                    "id": item["id"],
                    "name": item["name"],
                    "created_at": item["created_at"],
                    "expired": item.get("expired", False),
                    "branch": (item.get("workflow_run") or {}).get("head_branch") or "",
                }
            )
        if len(batch) < 100:
            return artifacts
        page += 1


def core_remaining() -> int:
    return int(gh("api", "rate_limit", "--jq", ".resources.core.remaining").strip())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--repo", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--min-age-hours", type=float, default=6.0)
    parser.add_argument("--max-deletions", type=int, default=3000)
    parser.add_argument("--budget-floor", type=int, default=1500, help="stop when the token has fewer REST calls left")
    parser.add_argument("--pause", type=float, default=0.3, help="seconds between deletions")
    args = parser.parse_args(argv)

    artifacts = list_artifacts(args.repo)
    chosen = select_for_deletion(artifacts, datetime.datetime.now(datetime.timezone.utc), args.min_age_hours)
    print(f"{len(artifacts)} artifacts; {len(chosen)} superseded copies to delete (at most {args.max_deletions} this run)")
    if args.dry_run:
        for artifact in chosen[:20]:
            print(f"  would delete {artifact['id']} {artifact['name']} ({artifact['branch']}, {artifact['created_at']})")
        return 0
    deleted = failed = 0
    for index, artifact in enumerate(chosen[: args.max_deletions]):
        # Every 100: stop while the token still has room for the other workflows (#4617 is about that budget).
        if index % 100 == 0 and core_remaining() < args.budget_floor:
            print(f"Stopping: fewer than {args.budget_floor} REST calls left; the rest waits for the next run")
            break
        try:
            gh("api", "-X", "DELETE", f"repos/{args.repo}/actions/artifacts/{artifact['id']}")
            deleted += 1
        except subprocess.CalledProcessError as error:
            failed += 1
            print(f"::warning::couldn't delete artifact {artifact['id']} ({artifact['name']}): {error.stderr.strip()}")
        time.sleep(args.pause)
    print(f"Deleted {deleted}, failed {failed}; {len(artifacts) - deleted} artifacts left")
    return 0


if __name__ == "__main__":
    sys.exit(main())
