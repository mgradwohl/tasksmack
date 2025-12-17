# GitHub Copilot Coding Agent - TaskSmack Tips

This guide helps you get the best results when using GitHub Copilot coding agent with the TaskSmack repository.

## Quick Start

GitHub Copilot coding agent can autonomously work on issues in this repository. It will:
1. Read the issue description and understand requirements
2. Explore the codebase and understand the architecture
3. Make minimal, surgical changes to address the issue
4. Create tests and validate changes
5. Submit a pull request for review

## What Copilot Excels At in TaskSmack

### Recommended Task Types

Copilot is particularly effective for these types of tasks in our C++23/ImGui project:

| Task Type | Examples | Notes |
|-----------|----------|-------|
| **Bug Fixes** | Memory leaks, null pointer checks, race conditions | Copilot can analyze stack traces and fix localized issues |
| **Test Coverage** | Add unit tests for Domain models, Platform probes | Follows existing Google Test patterns |
| **Code Formatting** | Apply clang-format, fix style issues | Automated with existing tools |
| **Documentation** | Update README, add code comments, fix typos | Copilot understands the architecture from docs |
| **New Probes** | Add new metrics (disk I/O, network stats) | Follows established Platform → Domain → UI pattern |
| **UI Enhancements** | New ImGui panels, table columns, charts | Copilot knows ImGui/ImPlot patterns |
| **Refactoring** | Extract methods, rename variables, simplify logic | Limited to single-file or small scope |
| **Dependencies** | Update CMakeLists.txt, add new libraries | Must use FetchContent with SYSTEM keyword |

### Tasks Requiring Human Review

These tasks benefit from Copilot but need careful human oversight:

- **Architecture changes** - Layer boundary modifications, new subsystems
- **Security fixes** - Privilege escalation, input validation, cryptography
- **Performance optimization** - Profiling-driven changes, algorithmic improvements
- **Cross-platform code** - Platform-specific Windows/Linux implementations
- **Concurrency** - Thread synchronization, lock-free data structures

### Tasks NOT Suitable for Copilot

Avoid delegating these to Copilot alone:

- **Design decisions** - Choosing between architectural approaches
- **Breaking changes** - Major API changes affecting multiple components
- **Business logic** - Domain-specific requirements needing human judgment
- **CI/CD workflows** - Requires human approval per security policy
- **Release management** - Versioning, changelog, packaging decisions

## Writing Effective Issues for Copilot

### Issue Template

```markdown
## Problem
[Clear description of what's broken or what feature is needed]

## Acceptance Criteria
- [ ] Specific requirement 1
- [ ] Specific requirement 2
- [ ] Tests pass
- [ ] Code formatted with clang-format

## Context
- Affected files: `src/Domain/ProcessModel.cpp`
- Related docs: [process.md](../process.md)
- Platform: Linux only / Windows only / Cross-platform

## Examples
[Code snippets, error messages, or expected behavior]
```

### Good Issue Characteristics

✅ **DO:**
- Be specific about which files/components need changes
- Include acceptance criteria as a checklist
- Reference existing patterns in the codebase
- Mention if tests are needed
- Link to relevant documentation
- Specify if it's platform-specific

❌ **DON'T:**
- Use vague terms like "improve performance" or "make it better"
- Ask for multiple unrelated changes in one issue
- Assume Copilot knows business context it hasn't seen
- Request changes to CI/CD workflows (Copilot can't modify `.github/workflows/*`)
- Ask for architectural redesigns without constraints

### Example: Good Issue

```markdown
Title: Add SHR (shared memory) column to process table

## Problem
The process table currently shows RES (resident memory) but not SHR (shared memory).
Users need to see how much memory is shared between processes.

## Acceptance Criteria
- [ ] Add `sharedMemoryBytes` field to `ProcessCounters` (Platform layer)
- [ ] Update Linux probe to read shared memory from `/proc/[pid]/statm`
- [ ] Add `sharedMemory` to `ProcessSnapshot` (Domain layer)
- [ ] Add "SHR" column to process table in UI
- [ ] Format shared memory in human-readable units (KB/MB/GB)
- [ ] Update tests for ProcessModel
- [ ] Run clang-format and clang-tidy

## Context
- Files: `src/Platform/ProcessTypes.h`, `src/Platform/Linux/LinuxProcessProbe.cpp`,
  `src/Domain/ProcessSnapshot.h`, `src/App/Panels/ProcessPanel.cpp`
- Platform: Linux (use `/proc/[pid]/statm` field 2)
- Windows: Use `WorkingSetSize - PrivateUsage` approximation
- Pattern: Follow existing RES column implementation

## Reference
See how htop displays SHR column: https://htop.dev/
```

## Repository-Specific Guidance

### Architecture Rules (Critical)

Copilot **must** respect these layer boundaries:

```
App (Panels) → UI → Core → Domain → Platform → OS APIs
```

**Rules:**
1. Platform returns **raw counters**, Domain computes **deltas/rates**
2. UI never calls Platform probes directly (use Domain models)
3. All OpenGL calls confined to `Core/` and `UI/`
4. Domain and Platform never depend on UI, Core, or graphics libraries

**Copilot will automatically check** these rules before submitting PRs.

### Code Style Enforcement

Copilot is configured to:
- ✅ Apply `.clang-format` rules (LLVM base, Allman braces)
- ✅ Fix `.clang-tidy` warnings
- ✅ Use modern C++23 features (`std::ranges`, `std::format`, `std::jthread`)
- ✅ Follow naming conventions (PascalCase classes, camelCase functions, m_camelCase members)
- ✅ Maintain include order (matching header → project → third-party → stdlib)

**No manual formatting needed** - Copilot handles this automatically.

### Testing Requirements

Copilot will:
1. Add tests when creating new features (follows existing Google Test patterns)
2. Run tests before submitting PR (`ctest --preset debug`)
3. Use mocks from `tests/Mocks/MockProbes.h` for Domain tests
4. Use `EXPECT_DOUBLE_EQ` for floating-point comparisons

### Build Validation

Copilot runs these checks before submitting:
```bash
# Format check
./tools/clang-format.sh

# Build
cmake --preset debug
cmake --build --preset debug

# Test
ctest --preset debug

# Static analysis
./tools/clang-tidy.sh debug
```

## Pull Request Review Process

### What Copilot Does

1. **Creates a branch** - `copilot/<issue-name>`
2. **Makes minimal changes** - Only touches files necessary to fix the issue
3. **Runs validation** - Format, build, test, static analysis
4. **Submits PR** - With detailed description and checklist

### What Reviewers Should Check

✅ **Code Quality:**
- Changes are minimal and surgical
- No unrelated modifications
- Follows architecture patterns
- Uses modern C++23 idioms

✅ **Correctness:**
- Tests pass (check CI results)
- No new clang-tidy warnings
- Memory safety (no leaks, use RAII)
- Thread safety (proper mutex usage)

✅ **Architecture:**
- Layer boundaries respected
- Platform returns raw counters, Domain computes rates
- No OpenGL calls outside Core/UI
- No UI dependencies in Domain/Platform

✅ **Documentation:**
- Comments explain *why*, not *what*
- README/docs updated if needed
- PR description explains changes

### Iterating on Copilot PRs

You can request changes by commenting on the PR:

```markdown
@copilot The CPU% calculation looks incorrect. It should divide by the number
of CPU cores to get a percentage per core, not total. See `ProcessModel::computeSnapshot()`
in existing code.
```

Copilot will read your feedback and update the PR.

## Advanced Tips

### Providing Context

Copilot reads these files automatically:
- `.github/copilot-instructions.md` - Architecture and coding standards
- `README.md` - Build and tool instructions
- `tasksmack.md` - Architecture overview
- `process.md` - Process enumeration details
- `CONTRIBUTING.md` - Development workflow
- `TODO.md` - Known issues and roadmap

**You don't need to repeat this information in issues.**

### Handling Platform-Specific Code

For cross-platform features, be explicit:

```markdown
## Platform Notes
- Linux: Read from `/proc/[pid]/io` (requires elevated privileges)
- Windows: Use `GetProcessIoCounters()` (no special privileges needed)
- If probe fails, return zeros (graceful degradation)
```

### Security Considerations

Copilot operates with:
- ✅ Read-only access to repository
- ✅ Cannot modify `.github/workflows/*` (CI/CD protected)
- ✅ Cannot access secrets or credentials
- ✅ All changes require human review before merge

**Still review carefully** - Copilot can make mistakes, especially with:
- Buffer overflows
- Integer overflows
- Race conditions
- Null pointer dereferences

### Performance Guidance

If asking Copilot to optimize performance:

```markdown
## Performance Requirements
- Target: Enumerate 5000 processes in <50ms
- Profile first: Run `perf record` and attach flamegraph
- Optimize hot paths only (avoid premature optimization)
- Benchmark before/after with Google Benchmark
```

## Common Copilot Mistakes (and How to Avoid Them)

### Mistake 1: Computing Rates in Platform Layer

❌ **Wrong:**
```cpp
// Platform/Linux/LinuxProcessProbe.cpp
double LinuxProcessProbe::getCpuPercent(int pid) {
    // Platform computes final value - BAD!
}
```

✅ **Right:**
```cpp
// Platform/Linux/LinuxProcessProbe.cpp
ProcessCounters LinuxProcessProbe::readCounters(int pid) {
    // Returns raw CPU ticks - GOOD!
}

// Domain/ProcessModel.cpp
double ProcessModel::computeCpuPercent(const ProcessCounters& current, const ProcessCounters& previous) {
    // Domain computes from deltas - GOOD!
}
```

**Issue template to prevent this:**
```markdown
⚠️ Platform layer returns raw counters only. Domain layer computes rates/percentages.
```

### Mistake 2: Adding OpenGL Calls Outside Core/UI

❌ **Wrong:**
```cpp
// Domain/ProcessModel.cpp
void ProcessModel::render() {
    glClear(GL_COLOR_BUFFER_BIT); // OpenGL in Domain - BAD!
}
```

✅ **Right:**
```cpp
// UI/ProcessPanel.cpp (inherits from Layer)
void ProcessPanel::render(bool* open) {
    // ImGui rendering only - OpenGL handled by Core - GOOD!
    ImGui::Begin("Processes", open);
    for (const auto& proc : m_Model->snapshots()) {
        // Render using ImGui API
    }
    ImGui::End();
}
```

### Mistake 3: Ignoring PID Reuse

❌ **Wrong:**
```cpp
// Domain/ProcessModel.cpp
uint64_t key = static_cast<uint64_t>(pid); // PID alone - BAD!
```

✅ **Right:**
```cpp
// Domain/ProcessModel.cpp
uint64_t key = makeUniqueKey(pid, startTime); // PID + start time - GOOD!
```

**Issue template to prevent this:**
```markdown
⚠️ Use `makeUniqueKey(pid, startTime)` to handle PID reuse correctly.
```

## Troubleshooting

### Copilot Can't Find Test Infrastructure

If Copilot says "no test framework found":
- Point it to `tests/` directory structure
- Reference `tests/Domain/test_ProcessModel.cpp` as an example

### Copilot Ignores Architecture Rules

If Copilot violates layer boundaries:
1. Reference `.github/copilot-instructions.md` in the issue
2. Explicitly state the rule in acceptance criteria
3. Request changes in PR review

### Build Failures in Copilot PR

Common causes:
- Missing CMakeLists.txt updates (new source files)
- Missing includes (forgot to add header)
- Platform-specific code not guarded with `#ifdef`

**Copilot will fix these** if you comment with the build error.

### Clang-Tidy Warnings

If Copilot's PR has clang-tidy warnings:
1. Check if warning is disabled in `.clang-tidy`
2. If legitimate issue, comment: `@copilot Fix clang-tidy warning about X`
3. Copilot will add appropriate fix or `// NOLINT` with justification

## Quick Reference

### Files Copilot Reads

| File | Purpose |
|------|---------|
| `.github/copilot-instructions.md` | Architecture, coding standards, patterns |
| `README.md` | Build instructions, project structure |
| `tasksmack.md` | High-level architecture vision |
| `process.md` | Process enumeration implementation details |
| `CONTRIBUTING.md` | Development workflow, PR process |
| `TODO.md` | Known issues, improvement ideas |
| `.clang-format` | Code formatting rules |
| `.clang-tidy` | Static analysis configuration |

### Tools Copilot Uses

| Tool | Purpose | When |
|------|---------|------|
| `clang-format` | Code formatting | Before every commit |
| `clang-tidy` | Static analysis | Before submitting PR |
| `cmake --preset debug` | Configure build | Before building |
| `cmake --build --preset debug` | Build project | Before testing |
| `ctest --preset debug` | Run tests | Before submitting PR |

### Issue Labels for Copilot

Use these labels to help Copilot prioritize:

- `good first issue` - Simple, well-scoped tasks ideal for Copilot
- `bug` - Bug fixes (Copilot excels at these)
- `documentation` - Docs updates (Copilot handles well)
- `enhancement` - New features (good if well-specified)
- `architecture` - Requires human design first
- `security` - Requires extra human review

## Resources

- [GitHub Copilot Best Practices](https://docs.github.com/en/copilot/tutorials/coding-agent/get-the-best-results)
- [TaskSmack Architecture](../tasksmack.md)
- [TaskSmack Contributing Guide](../CONTRIBUTING.md)
- [TaskSmack Process Implementation](../process.md)

---

**Remember:** Copilot is a powerful assistant, but you're the expert on your project. Review all PRs carefully, provide feedback, and iterate until the code meets your standards. Copilot learns from your feedback and improves over time.
