# CI Workflow Review Recommendations

This document previously contained a detailed, per-issue writeup of CI workflow improvement recommendations.

Those recommendations have been migrated to GitHub issues and are now tracked there as the source of truth:
- https://github.com/mgradwohl/tasksmack/issues/48
- https://github.com/mgradwohl/tasksmack/issues/49
- https://github.com/mgradwohl/tasksmack/issues/50
- https://github.com/mgradwohl/tasksmack/issues/51
- https://github.com/mgradwohl/tasksmack/issues/52
- https://github.com/mgradwohl/tasksmack/issues/53
- https://github.com/mgradwohl/tasksmack/issues/54
- https://github.com/mgradwohl/tasksmack/issues/55
- https://github.com/mgradwohl/tasksmack/issues/56
- https://github.com/mgradwohl/tasksmack/issues/57

Quick search: https://github.com/mgradwohl/tasksmack/issues?q=is%3Aissue+%5BCI%5D
# CI Workflow Review Recommendations

This document contains recommendations for improving the CI workflow in `.github/workflows/ci.yml` based on a comprehensive review of the project documentation and current implementation.

## Summary

After reviewing all markdown documentation files and the CI workflow, I've identified 10 key recommendations to improve the CI/CD pipeline. Each recommendation below should be created as a separate GitHub issue.

---

## Issue 1: Fix Python Version in Windows Build Job

**Priority:** HIGH  
**Type:** Bug  
**Labels:** bug, ci, windows

### Description

The Windows build job specifies Python version `3.14` which does not exist yet. Python 3.14 is not released (current stable versions are 3.12/3.13).

### Current Code (Line 66)

```yaml
- name: Setup Python
  uses: actions/setup-python@v5
  with:
    python-version: '3.14'
    cache: 'pip'
```

### Recommended Fix

```yaml
- name: Setup Python
  uses: actions/setup-python@v5
  with:
    python-version: '3.12'  # Use latest stable version
    cache: 'pip'
```

### Impact

- Build may fail or use incorrect Python version
- Inconsistent with documentation which mentions Python 3.12

### Files Affected

- `.github/workflows/ci.yml` (line 66)

---

## Issue 2: Add Missing Dependabot Configuration

**Priority:** MEDIUM  
**Type:** Feature  
**Labels:** enhancement, ci, security

### Description

The README.md mentions that "Dependabot is configured to automatically create PRs for GitHub Actions updates (weekly)" but the `.github/dependabot.yml` file does not exist in the repository.

### Recommended Implementation

Create `.github/dependabot.yml`:

```yaml
version: 2
updates:
  # GitHub Actions
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "weekly"
      day: "monday"
    open-pull-requests-limit: 10
    labels:
      - "dependencies"
      - "github-actions"
    commit-message:
      prefix: "ci"
      include: "scope"
    
  # Python dependencies (for GLAD/jinja2)
  - package-ecosystem: "pip"
    directory: "/"
    schedule:
      interval: "weekly"
      day: "monday"
    open-pull-requests-limit: 5
    labels:
      - "dependencies"
      - "python"
```

### Impact

- Automated security updates for GitHub Actions
- Reduced manual maintenance
- Improved security posture

### References

- README.md line 417: "**Dependabot** is configured to automatically create PRs for GitHub Actions updates (weekly)."

---

## Issue 3: Add Code Coverage Threshold Enforcement

**Priority:** MEDIUM  
**Type:** Enhancement  
**Labels:** enhancement, ci, testing

### Description

The CI workflow generates code coverage reports but doesn't enforce minimum coverage thresholds. This allows coverage to decrease over time without detection.

### Recommended Implementation

Add coverage threshold check to the `coverage` job:

```yaml
- name: Check coverage threshold
  run: |
    # Extract coverage percentage from llvm-cov output
    COVERAGE=$(grep "TOTAL" coverage-output.txt | awk '{print $NF}' | sed 's/%//')
    THRESHOLD=80
    
    echo "Current coverage: ${COVERAGE}%"
    echo "Minimum threshold: ${THRESHOLD}%"
    
    if (( $(echo "$COVERAGE < $THRESHOLD" | bc -l) )); then
      echo "❌ Coverage ${COVERAGE}% is below threshold ${THRESHOLD}%"
      exit 1
    fi
    
    echo "✅ Coverage ${COVERAGE}% meets threshold ${THRESHOLD}%"
```

### Benefits

- Prevents coverage regression
- Enforces quality standards
- Makes coverage meaningful rather than informational

### Configuration

Start with 70-80% threshold and adjust based on current coverage levels.

---

## Issue 4: Add CodeQL Security Scanning

**Priority:** MEDIUM  
**Type:** Enhancement  
**Labels:** enhancement, ci, security

### Description

The project uses sanitizers (ASan, UBSan, TSan) and clang-tidy for bug detection, but lacks automated security vulnerability scanning. GitHub's CodeQL provides free security scanning for C++ projects.

### Recommended Implementation

Add new job to `.github/workflows/ci.yml`:

```yaml
  codeql-analysis:
    name: CodeQL Security Scan
    runs-on: ubuntu-24.04
    permissions:
      security-events: write
      actions: read
      contents: read
    
    steps:
      - uses: actions/checkout@v6
      
      - name: Initialize CodeQL
        uses: github/codeql-action/init@v3
        with:
          languages: cpp
          queries: security-and-quality
      
      - name: Setup LLVM
        uses: ./.github/actions/setup-llvm
      
      - name: Build for CodeQL
        run: |
          cmake --preset debug
          cmake --build --preset debug
      
      - name: Perform CodeQL Analysis
        uses: github/codeql-action/analyze@v3
        with:
          category: "/language:cpp"
```

### Benefits

- Detects common security vulnerabilities (buffer overflows, SQL injection, XSS, etc.)
- Free for public repositories
- Integrates with GitHub Security tab
- Complements existing sanitizer and static analysis tools

### References

- SECURITY.md mentions security best practices but no automated scanning
- README.md line 410: "Runs static analysis (clang-tidy)" - CodeQL adds another layer

---

## Issue 5: Optimize Job Dependencies and Parallelization

**Priority:** LOW  
**Type:** Enhancement  
**Labels:** enhancement, ci, performance

### Description

Currently, all jobs depend on `validate-environment`, which creates a sequential bottleneck. Most jobs don't actually need prerequisite validation to run.

### Current Structure

```
validate-environment (30-60s)
    ↓
build-linux, build-windows, format-check, static-analysis, coverage, sanitizers (parallel)
```

### Recommended Structure

```
Independent jobs (all parallel):
- build-linux
- build-windows  
- format-check
- static-analysis
- coverage
- sanitizers
```

Keep `validate-environment` as an optional diagnostic job that doesn't block others.

### Rationale

- Reduces total workflow time by 30-60 seconds
- Jobs validate their own prerequisites through setup steps
- Failures are detected quickly in actual jobs
- `validate-environment` can still run for diagnostic purposes

### Implementation

Remove `needs: validate-environment` from all jobs and make it run independently:

```yaml
jobs:
  validate-environment:
    runs-on: ubuntu-24.04
    # Remove from critical path - runs in parallel for diagnostics
    
  build-linux:
    # Remove: needs: validate-environment
    runs-on: ubuntu-24.04
    # ... rest of job
```

---

## Issue 6: Add Artifact Retention Policy

**Priority:** LOW  
**Type:** Enhancement  
**Labels:** enhancement, ci

### Description

Artifacts (test results, coverage reports, sanitizer logs) have no explicit retention period set. GitHub's default is 90 days, which may consume unnecessary storage.

### Recommended Implementation

Add retention policy to all artifact uploads:

```yaml
- name: Upload test results
  if: always()
  uses: actions/upload-artifact@v6
  with:
    name: linux-test-results
    path: build/debug/test-results.xml
    retention-days: 30  # Keep for 30 days
```

### Retention Guidelines

- **Test results:** 30 days (for recent failure investigation)
- **Coverage reports:** 30 days (trend analysis done via external tools)
- **Sanitizer reports:** 14 days (issues should be fixed immediately)
- **clang-tidy results:** 14 days (issues should be addressed quickly)

### Benefits

- Reduces storage costs
- Keeps artifact list manageable
- Encourages timely issue resolution

---

## Issue 7: Add GitHub Release Automation Workflow

**Priority:** HIGH  
**Type:** Feature  
**Labels:** enhancement, ci, release

### Description

TODO.md lists "GitHub Release workflow" as a priority item (#4 in "Recommended Next Steps"). Currently, releases must be created manually.

### Recommended Implementation

Create `.github/workflows/release.yml`:

```yaml
name: Release

on:
  push:
    tags:
      - 'v*.*.*'

jobs:
  create-release:
    runs-on: ubuntu-24.04
    permissions:
      contents: write
    steps:
      - uses: actions/checkout@v6
      
      - name: Create Release
        uses: softprops/action-gh-release@v2
        with:
          draft: false
          prerelease: false
          generate_release_notes: true
          
  build-artifacts:
    needs: create-release
    strategy:
      matrix:
        include:
          - os: ubuntu-24.04
            preset: release
            artifact: tasksmack-linux-x64
          - os: windows-latest
            preset: win-release
            artifact: tasksmack-windows-x64
    
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v6
      
      # ... build steps ...
      
      - name: Package with CPack
        run: |
          cpack --config build/${{ matrix.preset }}/CPackConfig.cmake -G ZIP
      
      - name: Upload Release Assets
        uses: softprops/action-gh-release@v2
        with:
          files: dist/*.zip
```

### Benefits

- Automated binary releases on tag push
- Consistent release process
- Auto-generated release notes
- Distributable packages via CPack

### References

- TODO.md line 10: "**GitHub Release workflow** (1 hr) - Auto-create releases with binaries on tag push"
- README.md lines 327-351: CPack packaging documentation

---

## Issue 8: Add Build Matrix for Multiple Configurations

**Priority:** LOW  
**Type:** Enhancement  
**Labels:** enhancement, ci, testing

### Description

Currently, CI only tests the `debug` configuration on Linux and `win-debug` on Windows. The project supports multiple build types (debug, release, relwithdebinfo, optimized) but these are not tested in CI.

### Recommended Implementation

Expand `build-linux` and `build-windows` jobs to use matrix strategy:

```yaml
  build-linux:
    needs: validate-environment
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        preset: [debug, release, relwithdebinfo]
    steps:
      # ... setup steps ...
      
      - name: Configure
        run: cmake --preset ${{ matrix.preset }}
      
      - name: Build
        run: cmake --build --preset ${{ matrix.preset }}
      
      - name: Test
        run: ctest --preset ${{ matrix.preset }} -j $(nproc) --output-on-failure
```

### Considerations

- Increases CI time (3x builds vs 1x)
- Catches build-type-specific issues
- May want to make optional/scheduled rather than on every PR

### Alternative

Run full matrix on `main` branch only, run debug-only on PRs:

```yaml
strategy:
  fail-fast: false
  matrix:
    preset: ${{ github.ref == 'refs/heads/main' && fromJSON('["debug", "release", "relwithdebinfo"]') || fromJSON('["debug"]') }}
```

---

## Issue 9: Improve Parallelism with Environment Variables

**Priority:** LOW  
**Type:** Enhancement  
**Labels:** enhancement, ci

### Description

Test parallelism is hardcoded differently for Linux and Windows. Should use GitHub Actions environment variables for consistency.

### Current Code

Line 47 (Linux):
```yaml
run: ctest --preset debug -j $(nproc) --output-on-failure --timeout 60
```

Line 102 (Windows):
```yaml
run: ctest --preset win-debug -j $env:NUMBER_OF_PROCESSORS --output-on-failure --timeout 60
```

### Recommended Change

Use GitHub Actions environment variables for consistency:

```yaml
# Linux
run: ctest --preset debug -j ${{ env.GITHUB_ACTIONS_RUNNER_CPU_COUNT || '$(nproc)' }} --output-on-failure --timeout 60

# Windows  
run: ctest --preset win-debug -j ${{ env.GITHUB_ACTIONS_RUNNER_CPU_COUNT || env.NUMBER_OF_PROCESSORS }} --output-on-failure --timeout 60
```

Or simply use GitHub's runner context:

```yaml
# Works on both platforms
run: ctest --preset debug -j $(( $(nproc 2>/dev/null || echo 2) )) --output-on-failure
```

### Benefits

- Consistent parallelism across platforms
- Easier to maintain
- Can be overridden if needed

---

## Issue 10: Add Pre-commit Hook Workflow Validation

**Priority:** LOW  
**Type:** Enhancement  
**Labels:** enhancement, ci

### Description

TODO.md mentions pre-commit hooks as a priority improvement (#3). The CI workflow should validate that committed code passes pre-commit checks.

### Recommended Implementation

Add job to validate pre-commit compliance:

```yaml
  pre-commit:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v6
      
      - name: Setup Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.12'
      
      - name: Install pre-commit
        run: pip install pre-commit
      
      - name: Run pre-commit on all files
        run: pre-commit run --all-files
```

### Prerequisites

Create `.pre-commit-config.yaml` as mentioned in TODO.md:

```yaml
repos:
  - repo: https://github.com/pre-commit/pre-commit-hooks
    rev: v4.5.0
    hooks:
      - id: trailing-whitespace
      - id: end-of-file-fixer
      - id: check-yaml
      - id: check-added-large-files
  
  - repo: https://github.com/pocc/pre-commit-hooks
    rev: v1.3.5
    hooks:
      - id: clang-format
        args: [--style=file]
```

### References

- TODO.md line 9: "**Pre-commit hooks** (30 min) - Catch formatting issues before CI"

---

## Additional Observations

### Documentation Consistency

1. **README.md** mentions Dependabot but file doesn't exist (covered in Issue #2)
2. **Python version** inconsistency - README shows 3.12, CI uses 3.14 (covered in Issue #1)
3. **CMake version** - README says "3.28+ (4.2.1+ recommended)" - the 4.2.1 seems like a typo (CMake is at 3.x)

### Workflow Strengths

The CI workflow has several excellent features:

- ✅ Comprehensive testing (Linux + Windows)
- ✅ Multiple sanitizers (ASan, UBSan, TSan)
- ✅ Code coverage with HTML reports
- ✅ Static analysis with clang-tidy
- ✅ Format checking
- ✅ Good use of composite actions (setup-llvm)
- ✅ Helpful workflow summaries for coverage and sanitizers
- ✅ ccache for faster builds
- ✅ Proper artifact uploads

### Recommended Priority Order

1. **Issue 1** (HIGH): Fix Python version - immediate bug fix
2. **Issue 7** (HIGH): Add release automation - high-value feature per TODO.md
3. **Issue 2** (MEDIUM): Add Dependabot - security and maintenance
4. **Issue 3** (MEDIUM): Coverage thresholds - quality enforcement
5. **Issue 4** (MEDIUM): CodeQL scanning - security enhancement
6. **Issue 5** (LOW): Optimize job dependencies - performance
7. **Issue 6** (LOW): Artifact retention - cost optimization
8. **Issue 8** (LOW): Build matrix - comprehensive testing
9. **Issue 9** (LOW): Parallelism variables - code quality
10. **Issue 10** (LOW): Pre-commit validation - workflow enhancement

---

## Implementation Notes

- Each issue should be created separately with the appropriate labels
- Link related issues where dependencies exist
- Reference this document in each issue for full context
- Consider creating a project board to track these improvements
- Some issues (like #7, #10) require creating new files beyond just modifying ci.yml

## Cross-References

- **README.md**: Lines 407-417 (CI/CD section)
- **CONTRIBUTING.md**: Lines 142-169 (CI workflow reports)
- **TODO.md**: Lines 6-12 (Recommended next steps)
- **SECURITY.md**: Lines 29-38 (Security best practices)
- **.github/workflows/ci.yml**: Full file (358 lines)
- **.github/actions/setup-llvm/action.yml**: Composite action (111 lines)
