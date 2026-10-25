# CLAUDE.md

The shared rules for every agent live in [AGENTS.md](AGENTS.md), imported here. Change shared rules there; this
file holds only what applies to Claude Code alone. `tools/check-guidance-sync.py` (a pre-commit hook) fails if
this file stops importing AGENTS.md.

@AGENTS.md

## Claude Code only

- Review: run `/code-review` before opening a PR, and `/code-review ultra <PR#>` for a deeper multi-agent pass on
  an existing PR. Run the `tasksmack-review` skill (`.claude/skills/tasksmack-review/SKILL.md`, the Claude Code
  copy of `.github/skills/code-review/SKILL.md`) alongside it.
- A `PostToolUse` hook in `.claude/settings.json` runs `clang-format -i` on any `.cpp`/`.h`/`.hpp`/`.cc`/`.cxx`
  file you edit or write, so hand-running the formatter is a backstop, not a requirement, for files you touched.
