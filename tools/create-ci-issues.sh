#!/bin/bash
# Script to create GitHub issues for CI workflow recommendations
# Run this script after reviewing CI_WORKFLOW_RECOMMENDATIONS.md
# Requires: gh CLI installed and authenticated (gh auth login)

set -e

REPO="mgradwohl/tasksmack"

echo "Creating CI workflow improvement issues for ${REPO}..."
echo ""
echo "Prerequisites:"
echo "  1. GitHub CLI must be installed (https://cli.github.com/)"
echo "  2. You must be authenticated: gh auth login"
echo ""
read -p "Press Enter to continue or Ctrl+C to cancel..."

# Issue 1: Fix Python Version
echo "Creating Issue 1: Fix Python Version in Windows Build Job..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Fix Python version 3.14 to 3.12 in Windows build" \
  --label "bug,ci,windows" \
  --body "## Description

The Windows build job specifies Python version \`3.14\` which does not exist yet. Python 3.14 is not released (current stable versions are 3.12/3.13).

## Current Code (Line 66)

\`\`\`yaml
- name: Setup Python
  uses: actions/setup-python@v5
  with:
    python-version: '3.14'
    cache: 'pip'
\`\`\`

## Recommended Fix

\`\`\`yaml
- name: Setup Python
  uses: actions/setup-python@v5
  with:
    python-version: '3.12'  # Use latest stable version
    cache: 'pip'
\`\`\`

## Impact

- Build may fail or use incorrect Python version
- Inconsistent with documentation which mentions Python 3.12

## Files Affected

- \`.github/workflows/ci.yml\` (line 66)

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 2: Add Dependabot
echo "Creating Issue 2: Add Missing Dependabot Configuration..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add missing Dependabot configuration" \
  --label "enhancement,ci,security" \
  --body "## Description

The README.md mentions that \"Dependabot is configured to automatically create PRs for GitHub Actions updates (weekly)\" but the \`.github/dependabot.yml\` file does not exist in the repository.

## Recommended Implementation

Create \`.github/dependabot.yml\`:

\`\`\`yaml
version: 2
updates:
  # GitHub Actions
  - package-ecosystem: \"github-actions\"
    directory: \"/\"
    schedule:
      interval: \"weekly\"
      day: \"monday\"
    open-pull-requests-limit: 10
    labels:
      - \"dependencies\"
      - \"github-actions\"
    commit-message:
      prefix: \"ci\"
      include: \"scope\"

  # Python dependencies (for GLAD/jinja2)
  - package-ecosystem: \"pip\"
    directory: \"/\"
    schedule:
      interval: \"weekly\"
      day: \"monday\"
    open-pull-requests-limit: 5
    labels:
      - \"dependencies\"
      - \"python\"
\`\`\`

## Benefits

- Automated security updates for GitHub Actions
- Reduced manual maintenance
- Improved security posture

## References

- README.md line 417: \"**Dependabot** is configured to automatically create PRs for GitHub Actions updates (weekly).\"
- See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 3: Coverage Thresholds
echo "Creating Issue 3: Add Code Coverage Threshold Enforcement..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add code coverage threshold enforcement" \
  --label "enhancement,ci,testing" \
  --body "## Description

The CI workflow generates code coverage reports but doesn't enforce minimum coverage thresholds. This allows coverage to decrease over time without detection.

## Recommended Implementation

Add coverage threshold check to the \`coverage\` job:

\`\`\`yaml
- name: Check coverage threshold
  run: |
    # Extract coverage percentage from llvm-cov output
    COVERAGE=\$(grep \"TOTAL\" coverage-output.txt | awk '{print \$NF}' | sed 's/%//')
    THRESHOLD=80

    echo \"Current coverage: \${COVERAGE}%\"
    echo \"Minimum threshold: \${THRESHOLD}%\"

    if (( \$(echo \"\$COVERAGE < \$THRESHOLD\" | bc -l) )); then
      echo \"❌ Coverage \${COVERAGE}% is below threshold \${THRESHOLD}%\"
      exit 1
    fi

    echo \"✅ Coverage \${COVERAGE}% meets threshold \${THRESHOLD}%\"
\`\`\`

## Benefits

- Prevents coverage regression
- Enforces quality standards
- Makes coverage meaningful rather than informational

## Configuration

Start with 70-80% threshold and adjust based on current coverage levels.

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 4: CodeQL
echo "Creating Issue 4: Add CodeQL Security Scanning..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add CodeQL security scanning" \
  --label "enhancement,ci,security" \
  --body "## Description

The project uses sanitizers (ASan, UBSan, TSan) and clang-tidy for bug detection, but lacks automated security vulnerability scanning. GitHub's CodeQL provides free security scanning for C++ projects.

## Recommended Implementation

Add new job to \`.github/workflows/ci.yml\`:

\`\`\`yaml
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
          category: \"/language:cpp\"
\`\`\`

## Benefits

- Detects common security vulnerabilities (buffer overflows, SQL injection, XSS, etc.)
- Free for public repositories
- Integrates with GitHub Security tab
- Complements existing sanitizer and static analysis tools

## References

- SECURITY.md mentions security best practices but no automated scanning
- README.md line 410: \"Runs static analysis (clang-tidy)\" - CodeQL adds another layer
- See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 5: Job Dependencies
echo "Creating Issue 5: Optimize Job Dependencies..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Optimize job dependencies and parallelization" \
  --label "enhancement,ci,performance" \
  --body "## Description

Currently, all jobs depend on \`validate-environment\`, which creates a sequential bottleneck. Most jobs don't actually need prerequisite validation to run.

## Current Structure

\`\`\`
validate-environment (30-60s)
    ↓
build-linux, build-windows, format-check, static-analysis, coverage, sanitizers (parallel)
\`\`\`

## Recommended Structure

\`\`\`
Independent jobs (all parallel):
- build-linux
- build-windows
- format-check
- static-analysis
- coverage
- sanitizers
\`\`\`

Keep \`validate-environment\` as an optional diagnostic job that doesn't block others.

## Rationale

- Reduces total workflow time by 30-60 seconds
- Jobs validate their own prerequisites through setup steps
- Failures are detected quickly in actual jobs
- \`validate-environment\` can still run for diagnostic purposes

## Implementation

Remove \`needs: validate-environment\` from all jobs and make it run independently.

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 6: Artifact Retention
echo "Creating Issue 6: Add Artifact Retention Policy..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add artifact retention policy" \
  --label "enhancement,ci" \
  --body "## Description

Artifacts (test results, coverage reports, sanitizer logs) have no explicit retention period set. GitHub's default is 90 days, which may consume unnecessary storage.

## Recommended Implementation

Add retention policy to all artifact uploads:

\`\`\`yaml
- name: Upload test results
  if: always()
  uses: actions/upload-artifact@v6
  with:
    name: linux-test-results
    path: build/debug/test-results.xml
    retention-days: 30  # Keep for 30 days
\`\`\`

## Retention Guidelines

- **Test results:** 30 days (for recent failure investigation)
- **Coverage reports:** 30 days (trend analysis done via external tools)
- **Sanitizer reports:** 14 days (issues should be fixed immediately)
- **clang-tidy results:** 14 days (issues should be addressed quickly)

## Benefits

- Reduces storage costs
- Keeps artifact list manageable
- Encourages timely issue resolution

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full review details."

# Issue 7: Release Automation
echo "Creating Issue 7: Add GitHub Release Automation..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add GitHub release automation workflow" \
  --label "enhancement,ci,release" \
  --body "## Description

TODO.md lists \"GitHub Release workflow\" as a priority item (#4 in \"Recommended Next Steps\"). Currently, releases must be created manually.

## Recommended Implementation

Create \`.github/workflows/release.yml\` that:

1. Triggers on version tags (v*.*.*)
2. Builds release binaries for Linux and Windows
3. Packages with CPack
4. Creates GitHub release with artifacts
5. Auto-generates release notes

## Benefits

- Automated binary releases on tag push
- Consistent release process
- Auto-generated release notes
- Distributable packages via CPack

## References

- TODO.md line 10: \"**GitHub Release workflow** (1 hr) - Auto-create releases with binaries on tag push\"
- README.md lines 327-351: CPack packaging documentation
- See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full implementation details."

# Issue 8: Build Matrix
echo "Creating Issue 8: Add Build Matrix for Multiple Configurations..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add build matrix for multiple configurations" \
  --label "enhancement,ci,testing" \
  --body "## Description

Currently, CI only tests the \`debug\` configuration on Linux and \`win-debug\` on Windows. The project supports multiple build types (debug, release, relwithdebinfo, optimized) but these are not tested in CI.

## Recommended Implementation

Expand \`build-linux\` and \`build-windows\` jobs to use matrix strategy testing debug, release, and relwithdebinfo configurations.

## Considerations

- Increases CI time (3x builds vs 1x)
- Catches build-type-specific issues
- May want to make optional/scheduled rather than on every PR

## Alternative

Run full matrix on \`main\` branch only, run debug-only on PRs to balance coverage vs speed.

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full implementation details."

# Issue 9: Parallelism
echo "Creating Issue 9: Improve Parallelism Configuration..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Improve test parallelism with environment variables" \
  --label "enhancement,ci" \
  --body "## Description

Test parallelism is hardcoded differently for Linux (\`\$(nproc)\`) and Windows (\`\$env:NUMBER_OF_PROCESSORS\`). Should use GitHub Actions environment variables for consistency.

## Current Code

Line 47 (Linux):
\`\`\`yaml
run: ctest --preset debug -j \$(nproc) --output-on-failure --timeout 60
\`\`\`

Line 102 (Windows):
\`\`\`yaml
run: ctest --preset win-debug -j \$env:NUMBER_OF_PROCESSORS --output-on-failure --timeout 60
\`\`\`

## Benefits

- Consistent parallelism across platforms
- Easier to maintain
- Can be overridden if needed

## References

See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for recommended changes."

# Issue 10: Pre-commit
echo "Creating Issue 10: Add Pre-commit Hook Validation..."
gh issue create \
  --repo "${REPO}" \
  --title "[CI] Add pre-commit hook workflow validation" \
  --label "enhancement,ci" \
  --body "## Description

TODO.md mentions pre-commit hooks as a priority improvement (#3). The CI workflow should validate that committed code passes pre-commit checks.

## Recommended Implementation

1. Create \`.pre-commit-config.yaml\` with clang-format and other hooks
2. Add CI job to validate pre-commit compliance on all files

## Benefits

- Catches formatting issues before CI
- Enforces consistent code quality
- Educates contributors about project standards

## Prerequisites

This depends on implementing pre-commit hooks in the repository (see TODO.md).

## References

- TODO.md line 9: \"**Pre-commit hooks** (30 min) - Catch formatting issues before CI\"
- See [CI_WORKFLOW_RECOMMENDATIONS.md](../CI_WORKFLOW_RECOMMENDATIONS.md) for full implementation details."

echo ""
echo "✅ All 10 issues created successfully!"
echo ""
echo "View issues at: https://github.com/${REPO}/issues"
echo ""
echo "Next steps:"
echo "  1. Review and prioritize the issues"
echo "  2. Consider creating a project board to track progress"
echo "  3. Start with high-priority issues (#1, #7, #2, #3, #4)"
