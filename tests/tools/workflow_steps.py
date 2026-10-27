"""Reads one step of a GitHub Actions workflow so a test can run its `run:` script (#2478).

Only the shapes this repository's workflows use are understood: a step starts with `- name: <name>`,
its `env:` values are plain scalars or `|`/`|-` block scalars, and its `run:` is a `|` block. Standard
library only, like the tools/check-*.py checkers, so it needs no PyYAML.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

_EXPRESSION = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")


@dataclass
class Step:
    name: str
    env: dict[str, str] = field(default_factory=dict)
    run: str = ""
    condition: str = ""  # its `if:`, as written; empty when it has none


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _block(lines: list[str], start: int, parent_indent: int) -> tuple[str, int]:
    """The block scalar whose lines start at `start`, de-indented, and the index after it."""
    body: list[str] = []
    index = start
    while index < len(lines) and (not lines[index].strip() or _indent(lines[index]) > parent_indent):
        body.append(lines[index])
        index += 1
    while body and not body[-1].strip():
        body.pop()
    width = min((_indent(line) for line in body if line.strip()), default=0)
    return "\n".join(line[width:] for line in body), index


def _job_lines(lines: list[str], job: str) -> tuple[int, int]:
    """The [first, end) line range of the job whose key is `job`; raises LookupError when there is none."""
    key = re.compile(r"^(\s*)" + re.escape(job) + r":\s*$")
    for start, line in enumerate(lines):
        match = key.match(line)
        if match is None:
            continue
        indent = len(match.group(1))
        end = start + 1
        while end < len(lines) and (
            not lines[end].strip() or lines[end].lstrip().startswith("#") or _indent(lines[end]) > indent
        ):
            end += 1
        return start + 1, end
    raise LookupError(f"no job named {job!r}")


def find_step(workflow: Path, name: str, job: str | None = None) -> Step:
    """The step named `name` in `workflow`, only within `job` when given (two jobs can each have a
    step of the same name); raises LookupError when there is none."""
    lines = workflow.read_text(encoding="utf-8").splitlines()
    if job is not None:
        first, last = _job_lines(lines, job)
        lines = lines[first:last]
    header = re.compile(r"^(\s*)- name:\s*" + re.escape(name) + r"\s*$")
    for start, line in enumerate(lines):
        match = header.match(line)
        if match is None:
            continue
        dash = len(match.group(1))
        keys = dash + 2
        end = start + 1
        while end < len(lines) and (not lines[end].strip() or _indent(lines[end]) > dash):
            end += 1
        step = Step(name=name)
        index = start + 1
        while index < end:
            text = lines[index]
            if not text.strip() or _indent(text) != keys or text.lstrip().startswith("#"):
                index += 1
                continue
            key, _, value = text.strip().partition(":")
            value = value.strip()
            if key == "run":
                step.run, index = _block(lines, index + 1, keys)
                if value not in ("|", "|-"):
                    step.run = value
                continue
            if key == "if":
                step.condition = value
                index += 1
                continue
            if key == "env":
                index += 1
                while index < end and (not lines[index].strip() or _indent(lines[index]) > keys):
                    entry = lines[index]
                    if not entry.strip() or entry.lstrip().startswith("#") or _indent(entry) != keys + 2:
                        index += 1
                        continue
                    env_key, _, env_value = entry.strip().partition(":")
                    env_value = env_value.strip()
                    if env_value in ("|", "|-"):
                        step.env[env_key], index = _block(lines, index + 1, keys + 2)
                    else:
                        step.env[env_key] = env_value
                        index += 1
                continue
            index += 1
        return step
    where = f"{workflow.name}'s {job}" if job is not None else workflow.name
    raise LookupError(f"{where} has no step named {name!r}")


def substitute(text: str, values: dict[str, str]) -> str:
    """Replaces each `${{ expression }}` in `text` by `values[expression]`; an unlisted one is an error."""

    def replace(match: re.Match[str]) -> str:
        expression = match.group(1)
        if expression not in values:
            raise KeyError(f"no value given for ${{{{ {expression} }}}}")
        return values[expression]

    return _EXPRESSION.sub(replace, text)
