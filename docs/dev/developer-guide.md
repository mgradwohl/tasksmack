# Developer Guide

The repository keeps contributor instructions in one canonical file:

**[Read `CONTRIBUTING.md` on GitHub](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md)**

It covers prerequisites, automated setup, CMake presets, tests, formatting, static analysis, benchmarks, profiling, packaging, CI, and the pull-request process.

## Fast Path

### Linux

```bash
./tools/setup-dev.sh
source .venv/bin/activate
cmake --workflow --preset dev
```

### Windows

```powershell
pwsh tools/setup-dev.ps1
cmake --workflow --preset win-dev
```

Run the prerequisite checker if an existing environment fails to configure:

```bash
./tools/check-prereqs.sh
pwsh tools/check-prereqs.ps1
```

The docs site intentionally does not duplicate the detailed command reference. This prevents setup and CI instructions from drifting away from the repository's current tooling.
