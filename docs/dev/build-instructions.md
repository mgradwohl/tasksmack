# Build Instructions

Build, test, sanitizer, benchmark, profiling, PGO, cleanup, and packaging instructions are maintained in:

**[The canonical contributor guide](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md#build)**

The normal development cycle is:

```bash
cmake --workflow --preset dev       # Linux
cmake --workflow --preset win-dev   # Windows
```

Useful discovery commands:

```bash
cmake --list-presets
cmake --list-presets=workflow
```

Keeping the detailed preset tables in `CONTRIBUTING.md` ensures they are updated alongside `CMakePresets.json`.
