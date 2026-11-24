#Requires -Version 7
#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    The hand steps of a TaskSmack release in one place (#4854).
.DESCRIPTION
    release.yml does the build, signing, validation and publishing. This script does the steps between its
    jobs, each safe to run again, each printing what to do next (docs/dev/release-process.md, "Patch releases"
    and stage 7). It never pushes a tag: agents don't (AGENTS.md), and the maintainer does.

    Steps:
      Bump    A patch version bump on release/X.Y: a branch that changes only project(TaskSmack VERSION ...)
              in CMakeLists.txt, pushed, and a pull request against release/X.Y. Works in a temporary worktree,
              so your checkout is left alone.
      DryRun  Dispatches release.yml with dry_run=true on -Ref (a branch), waits, and prints the result and the
              run link. Refuses unless the branch's head is -Sha, when -Sha is given. Without it, it prints
              the head it tests.
      Tag     Prints the git tag and git push commands for the maintainer. Runs nothing.
      Verify  Downloads the published release and checks SHA256SUMS, the build-provenance attestation of every
              package, Authenticode on both installers and on the exes in both zips, cosign when installed, and
              that the notes are not the placeholder. Prints a Markdown summary for the tracker. Read-only.
      WinGet  Which WinGet path the release is on and what to do: A (TaskSmack.TaskSmack is in winget-pkgs; the
              version's pull request, its labels, Needs-CLA), B (the first submission is still open) or C (it was
              closed unmerged: the hand-submission commands). Same checks as publish-winget. With -Rerun, and only
              when the package exists, the version doesn't and no pull request for it is open, it re-runs the tag
              run's publish-winget job, waits, and prints the pull request it opened. Read-only without -Rerun.
      Status  The release run, the CHANGELOG pull requests, the winget-pkgs pull request, WinGet health issue
              and the milestone's open count. Read-only.

    Needs PowerShell 7 and an authenticated gh. Verify's Authenticode checks run on Windows only.
.PARAMETER Step
    Bump, DryRun, Tag, Verify, Status or WinGet.
.PARAMETER Version
    The release's version, X.Y.Z with no leading v (Bump needs a patch: Z of 1 or more).
.PARAMETER Ref
    DryRun: the branch to test (release/0.11 for a patch). A commit SHA can't be dispatched.
.PARAMETER Sha
    DryRun: the commit the branch's head must be. Tag: the commit to tag (40 hex digits).
.PARAMETER NoWait
    DryRun: dispatch and print the run link without waiting for the result.
.PARAMETER Rerun
    WinGet: re-run publish-winget of the vX.Y.Z tag run when that is safe (see Steps); otherwise say why not.
.EXAMPLE
    pwsh tools/release.ps1 -Step Bump -Version 0.11.1
.EXAMPLE
    pwsh tools/release.ps1 -Step DryRun -Ref release/0.11
.EXAMPLE
    pwsh tools/release.ps1 -Step Tag -Version 0.11.1 -Sha <sha>
.EXAMPLE
    pwsh tools/release.ps1 -Step Verify -Version 0.11.0
.EXAMPLE
    pwsh tools/release.ps1 -Step Status -Version 0.11.1
.EXAMPLE
    pwsh tools/release.ps1 -Step WinGet -Version 0.11.1 -Rerun
#>
[CmdletBinding()]
param(
    [ValidateSet('Bump', 'DryRun', 'Tag', 'Verify', 'Status', 'WinGet')]
    [string]$Step,
    [string]$Version,
    [string]$Ref,
    [string]$Sha,
    [switch]$NoWait,
    [switch]$Rerun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = 'mgradwohl/tasksmack'
$WingetRepo = 'microsoft/winget-pkgs'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CosignIdentityRegexp = '^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/.+$'
$CosignIssuer = 'https://token.actions.githubusercontent.com'

function Test-ReleaseVersion {
    param([string]$Value)
    return [bool]($Value -match '^\d+\.\d+\.\d+$')
}

function Assert-ReleaseVersion {
    param([string]$Value)
    if (-not (Test-ReleaseVersion $Value)) {
        throw "-Version must be X.Y.Z with no leading v (e.g. 0.11.1), not '$Value'."
    }
}

function Get-ReleaseBranch {
    param([string]$Value)
    $parts = $Value -split '\.'
    return "release/$($parts[0]).$($parts[1])"
}

function Get-ExpectedAssets {
    param([string]$Value)
    return @(
        "TaskSmack-$Value-win64.exe", "TaskSmack-$Value-windows-arm64.exe",
        "TaskSmack-$Value-win64.zip", "TaskSmack-$Value-windows-arm64.zip",
        "TaskSmack-$Value-Linux.deb", "TaskSmack-$Value-Linux.tar.gz",
        "TaskSmack-$Value-linux-aarch64.deb", "TaskSmack-$Value-linux-aarch64.tar.gz",
        'SHA256SUMS', 'SHA256SUMS.bundle')
}

function Get-TagCommands {
    param([string]$Value, [string]$CommitSha)
    return @(
        'git fetch origin',
        "git tag -a v$Value $CommitSha -m `"TaskSmack $Value`"",
        "git push origin v$Value")
}

function Get-ExpectedSigner {
    $workflow = Join-Path $RepoRoot '.github/workflows/release.yml'
    if (Test-Path -LiteralPath $workflow) {
        $match = Select-String -LiteralPath $workflow -Pattern "WINDOWS_EXPECTED_SIGNER:\s*'([^']+)'" | Select-Object -First 1
        if ($match) { return $match.Matches[0].Groups[1].Value }
    }
    return 'Matt Gradwohl'
}

function Get-BumpPullRequestBody {
    param([string]$Value, [string]$Branch)
    $line = "project(TaskSmack VERSION $Value LANGUAGES C CXX)"
    return @"
## Description

The version bump for the v$Value patch release on ``$Branch``. It changes only ``$line`` in ``CMakeLists.txt`` (docs/dev/release-process.md, "Patch releases"). Opened by ``tools/release.ps1 -Step Bump``.

Fixes #<!-- the patch's tracker issue number -->

## PR Size

Small: one line.

## Type of Change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Documentation update
- [x] Build/CI improvement

## Checklist

- [x] I have read the [CONTRIBUTING](../CONTRIBUTING.md) guidelines
- [x] My code follows the project's coding standards
- [ ] I have run ``clang-tidy`` and addressed any warnings (no C++ changed)
- [ ] I have run ``clang-format`` on my changes (no C++ changed)
- [x] I have run ``pre-commit run --all-files`` or installed pre-commit hooks
- [ ] I have added tests that prove my fix/feature works (a version number only)
- [ ] A change to a real OS path adds a ``CiRealSystem*`` test, or I say below why only a manual check can cover it (not a real OS path)
- [ ] All new and existing tests pass (CI runs them)
- [x] I have updated documentation as needed

## Testing

CI on this pull request. Next: ``pwsh tools/release.ps1 -Step DryRun -Ref $Branch`` on the merged head.

## Additional Notes

Merge it before the patch's candidate (docs/dev/release-process.md, "Versions").
"@
}

function Invoke-Gh {
    # Runs gh, returns its stdout lines, and throws when it fails.
    param([string[]]$Arguments)
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "gh $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
    return $output
}

function Invoke-GhJson {
    param([string[]]$Arguments)
    $text = (Invoke-Gh $Arguments) -join "`n"
    if ([string]::IsNullOrWhiteSpace($text)) { return $null }
    return $text | ConvertFrom-Json
}

function Invoke-Git {
    param([string[]]$Arguments, [string]$WorkDir = $RepoRoot)
    $output = & git -C $WorkDir @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
    return $output
}

function Invoke-BumpStep {
    param([string]$Value)
    Assert-ReleaseVersion $Value
    $parts = $Value -split '\.'
    if ([int]$parts[2] -lt 1) { throw "-Step Bump is for patch releases (X.Y.1 and up); X.Y.0 is cut from main, not bumped. Got $Value." }
    $branchBase = Get-ReleaseBranch $Value
    $bumpBranch = "chore/version-$Value"
    $pattern = 'project\(TaskSmack VERSION (\d+\.\d+\.\d+)( LANGUAGES C CXX\))'

    Invoke-Git @('fetch', 'origin', $branchBase, '--quiet') | Out-Null
    $existing = Invoke-GhJson @('pr', 'list', '--repo', $Repo, '--head', $bumpBranch, '--state', 'all', '--json', 'number,state,url')
    if ($existing -and @($existing).Count -gt 0) {
        $pr = @($existing)[0]
        Write-Host "Already open: #$($pr.number) ($($pr.state)) $($pr.url)"
        Write-Host "Next: merge it, then: pwsh tools/release.ps1 -Step DryRun -Ref $branchBase"
        return
    }

    $worktree = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-bump-$Value-$PID"
    Invoke-Git @('worktree', 'add', '-B', $bumpBranch, $worktree, "origin/$branchBase", '--quiet') | Out-Null
    try {
        $cmake = Join-Path $worktree 'CMakeLists.txt'
        $text = [System.IO.File]::ReadAllText($cmake)
        $found = [regex]::Match($text, $pattern)
        if (-not $found.Success) { throw "No project(TaskSmack VERSION ...) line in $branchBase's CMakeLists.txt." }
        $current = [Version]$found.Groups[1].Value
        if ("$($current.Major).$($current.Minor)" -ne "$($parts[0]).$($parts[1])") {
            throw "$branchBase reads $current, not a $($parts[0]).$($parts[1]).x version."
        }
        if ($current -ge [Version]$Value) { throw "$branchBase already reads $current, which is not below $Value." }
        $updated = [regex]::Replace($text, $pattern, "project(TaskSmack VERSION $Value`$2", 1)
        [System.IO.File]::WriteAllText($cmake, $updated)
        $changed = @(Invoke-Git @('diff', '--name-only') $worktree)
        if ($changed.Count -ne 1 -or $changed[0] -ne 'CMakeLists.txt') { throw "Expected only CMakeLists.txt to change, got: $($changed -join ', ')" }
        Invoke-Git @('commit', '-a', '-m', "chore(release): VERSION $Value on $branchBase", '--quiet') $worktree | Out-Null
        Invoke-Git @('push', '-u', 'origin', $bumpBranch, '--quiet') $worktree | Out-Null
        $bodyFile = Join-Path $worktree '.bump-pr-body.md'
        Set-Content -LiteralPath $bodyFile -Value (Get-BumpPullRequestBody -Value $Value -Branch $branchBase) -Encoding utf8
        $url = (Invoke-Gh @('pr', 'create', '--repo', $Repo, '--base', $branchBase, '--head', $bumpBranch,
                '--title', "chore(release): VERSION $Value on $branchBase", '--body-file', $bodyFile)) -join ''
        Write-Host "Opened: $url"
        Write-Host "Next: fill in the tracker issue number, merge it when CI is green, then: pwsh tools/release.ps1 -Step DryRun -Ref $branchBase"
    }
    finally {
        & git -C $RepoRoot worktree remove --force $worktree 2>$null
    }
}

function Invoke-DryRunStep {
    param([string]$Branch, [string]$ExpectedSha, [switch]$NoWait)
    if ([string]::IsNullOrWhiteSpace($Branch)) { throw '-Step DryRun needs -Ref <branch> (release/X.Y for a patch).' }
    if ($Branch -match '^[0-9a-f]{40}$') { throw "-Ref must be a branch name: release.yml can't be dispatched on a commit SHA. Give the branch whose head is $Branch." }
    if ($ExpectedSha -and $ExpectedSha -notmatch '^[0-9a-f]{40}$') { throw '-Sha must be the full 40-hex commit SHA.' }

    $head = (Invoke-Gh @('api', "repos/$Repo/commits/$Branch", '--jq', '.sha')) -join ''
    if ($ExpectedSha -and $head -ne $ExpectedSha) {
        throw "$Branch's head is $head, not $ExpectedSha. Refusing to dry-run a different commit."
    }
    Write-Host "Dry run of $Branch at $head"

    function Find-Run {
        $runs = Invoke-GhJson @('run', 'list', '--repo', $Repo, '--workflow', 'release.yml', '--event', 'workflow_dispatch',
            '--branch', $Branch, '--limit', '10', '--json', 'databaseId,headSha,status,conclusion,url,createdAt')
        return @($runs) | Where-Object { $_ -and $_.headSha -eq $head } | Select-Object -First 1
    }

    $run = Find-Run
    if ($run -and $run.status -ne 'completed') {
        Write-Host "A run for this commit is already going: $($run.url)"
    }
    else {
        $started = (Get-Date).ToUniversalTime().AddMinutes(-1)
        Invoke-Gh @('workflow', 'run', 'release.yml', '--repo', $Repo, '--ref', $Branch, '-f', 'dry_run=true') | Out-Null
        $run = $null
        for ($i = 0; $i -lt 30 -and -not $run; $i++) {
            Start-Sleep -Seconds 5
            $candidate = Find-Run
            if ($candidate -and ([datetime]$candidate.createdAt).ToUniversalTime() -ge $started) { $run = $candidate }
        }
        if (-not $run) { throw "Dispatched, but no run for $head appeared within 150 s. Check https://github.com/$Repo/actions/workflows/release.yml" }
        Write-Host "Dispatched: $($run.url)"
    }
    if ($NoWait) { Write-Host 'Not waiting (-NoWait).'; return }

    & gh run watch $run.databaseId --repo $Repo --interval 60 *> $null
    $final = Invoke-GhJson @('run', 'view', "$($run.databaseId)", '--repo', $Repo, '--json', 'conclusion,url')
    Write-Host "Result: $($final.conclusion) $($final.url)"
    if ($final.conclusion -ne 'success') { throw "The dry run did not succeed ($($final.conclusion))." }
    Write-Host "Next: validation (tools/sandbox-install-check.ps1), approval on the tracker, then: pwsh tools/release.ps1 -Step Tag -Version <X.Y.Z> -Sha $head"
}

function Invoke-TagStep {
    param([string]$Value, [string]$CommitSha)
    Assert-ReleaseVersion $Value
    if ($CommitSha -notmatch '^[0-9a-f]{40}$') { throw '-Step Tag needs -Sha <the approved commit>, the full 40-hex SHA.' }
    $known = Invoke-GhJson @('api', "repos/$Repo/commits/$CommitSha")
    if (-not $known) { throw "$CommitSha is not a commit in $Repo." }
    $tagExists = @(& gh api "repos/$Repo/git/ref/tags/v$Value" --jq .object.sha 2>$null)
    if ($LASTEXITCODE -eq 0 -and $tagExists) { Write-Host "Note: v$Value already exists on GitHub ($($tagExists -join '')). Check before pushing." }
    Write-Host "Maintainer: run these in a clone ($(Get-ReleaseBranch $Value) must contain $CommitSha). This script does not run them."
    Write-Host ''
    Get-TagCommands -Value $Value -CommitSha $CommitSha | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host "Next: watch release.yml, merge both CHANGELOG PRs, then: pwsh tools/release.ps1 -Step Verify -Version $Value"
}

function Invoke-VerifyStep {
    param([string]$Value)
    Assert-ReleaseVersion $Value
    $tag = "v$Value"
    $signer = Get-ExpectedSigner
    $work = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-verify-$Value-$PID"
    $rows = [System.Collections.Generic.List[object]]::new()
    function Add-Row {
        param([string]$Check, [string]$Status, [string]$Detail = '')
        $rows.Add([pscustomobject]@{ Check = $Check; Status = $Status; Detail = $Detail })
        Write-Host ("{0,-5} {1} {2}" -f $Status, $Check, $Detail)
    }
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        Invoke-Gh @('release', 'download', $tag, '--repo', $Repo, '--dir', $work, '--clobber') | Out-Null
        $present = @(Get-ChildItem -LiteralPath $work -File | ForEach-Object Name)
        $missing = @(Get-ExpectedAssets $Value | Where-Object { $_ -notin $present })
        Add-Row 'Assets present' $(if ($missing.Count -eq 0) { 'PASS' } else { 'FAIL' }) $(if ($missing.Count -eq 0) { "$($present.Count) files" } else { "missing: $($missing -join ', ')" })

        # SHA256SUMS: every listed file hashes to its line, and every package is listed.
        $sums = @{}
        foreach ($line in Get-Content -LiteralPath (Join-Path $work 'SHA256SUMS')) {
            if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') { $sums[$Matches[2]] = $Matches[1].ToLowerInvariant() }
        }
        $bad = @()
        foreach ($name in $sums.Keys) {
            $path = Join-Path $work $name
            if (-not (Test-Path -LiteralPath $path)) { $bad += "$name (not downloaded)"; continue }
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sums[$name]) { $bad += $name }
        }
        $packages = @(Get-ExpectedAssets $Value | Where-Object { $_ -match '\.(exe|zip|deb|tar\.gz)$' })
        $unlisted = @($packages | Where-Object { -not $sums.ContainsKey($_) })
        Add-Row 'SHA256SUMS' $(if ($bad.Count -eq 0 -and $unlisted.Count -eq 0 -and $sums.Count -gt 0) { 'PASS' } else { 'FAIL' }) `
            "$($sums.Count) entries$(if ($bad) { "; mismatch: $($bad -join ', ')" })$(if ($unlisted) { "; not listed: $($unlisted -join ', ')" })"

        # Build-provenance attestation of every package and of SHA256SUMS.
        foreach ($name in $packages + @('SHA256SUMS')) {
            $path = Join-Path $work $name
            if (-not (Test-Path -LiteralPath $path)) { Add-Row "Attestation $name" 'FAIL' 'not downloaded'; continue }
            & gh attestation verify $path --repo $Repo *> $null
            Add-Row "Attestation $name" $(if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' })
        }

        # cosign on SHA256SUMS, when installed.
        if (Get-Command cosign -ErrorAction SilentlyContinue) {
            & cosign verify-blob --bundle (Join-Path $work 'SHA256SUMS.bundle') --certificate-identity-regexp $CosignIdentityRegexp `
                --certificate-oidc-issuer $CosignIssuer (Join-Path $work 'SHA256SUMS') *> $null
            Add-Row 'cosign SHA256SUMS' $(if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' })
        }
        else {
            Add-Row 'cosign SHA256SUMS' 'SKIP' 'cosign is not installed'
        }

        # Authenticode on both installers and on the exes inside both zips (Windows only).
        if ($IsWindows) {
            $targets = [System.Collections.Generic.List[string]]::new()
            foreach ($name in "TaskSmack-$Value-win64.exe", "TaskSmack-$Value-windows-arm64.exe") { $targets.Add((Join-Path $work $name)) }
            foreach ($zip in "TaskSmack-$Value-win64.zip", "TaskSmack-$Value-windows-arm64.zip") {
                $zipPath = Join-Path $work $zip
                if (-not (Test-Path -LiteralPath $zipPath)) { continue }
                $dest = Join-Path $work "unzipped-$([System.IO.Path]::GetFileNameWithoutExtension($zip))"
                Expand-Archive -LiteralPath $zipPath -DestinationPath $dest -Force
                Get-ChildItem -LiteralPath $dest -Recurse -File -Filter '*.exe' | ForEach-Object { $targets.Add($_.FullName) }
            }
            foreach ($target in $targets) {
                $label = $target.Substring($work.Length + 1)
                if (-not (Test-Path -LiteralPath $target)) { Add-Row "Authenticode $label" 'FAIL' 'missing'; continue }
                $sig = Get-AuthenticodeSignature -LiteralPath $target
                $cn = if ($sig.SignerCertificate -and $sig.SignerCertificate.Subject -match 'CN=([^,]+)') { $Matches[1].Trim('"') } else { '' }
                $ok = $sig.Status -eq 'Valid' -and $cn -eq $signer -and $null -ne $sig.TimeStamperCertificate
                Add-Row "Authenticode $label" $(if ($ok) { 'PASS' } else { 'FAIL' }) "$($sig.Status), signer '$cn'$(if ($null -eq $sig.TimeStamperCertificate) { ', not timestamped' })"
            }
        }
        else {
            Add-Row 'Authenticode' 'SKIP' 'needs Windows'
        }

        # The notes are not the placeholder.
        $release = Invoke-GhJson @('release', 'view', $tag, '--repo', $Repo, '--json', 'body,isDraft,isPrerelease')
        $body = "$($release.body)".TrimStart()
        $notesOk = (-not $release.isDraft) -and $body.Length -gt 0 -and -not $body.StartsWith('Release notes follow.')
        Add-Row 'Release notes' $(if ($notesOk) { 'PASS' } else { 'FAIL' }) $(if ($release.isDraft) { 'still a draft' } elseif (-not $notesOk) { 'still the placeholder (changelog.yml has not filled them in)' } else { '' })
    }
    finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }

    $failed = @($rows | Where-Object Status -eq 'FAIL')
    Write-Host ''
    Write-Host "Markdown summary for the tracker:"
    Write-Host ''
    Write-Host "Post-publish verification of $tag (``tools/release.ps1 -Step Verify``): **$(if ($failed.Count -eq 0) { 'PASS' } else { "FAIL ($($failed.Count))" })**"
    Write-Host ''
    Write-Host '| Check | Result | Detail |'
    Write-Host '|---|---|---|'
    foreach ($row in $rows) { Write-Host "| $($row.Check) | $($row.Status) | $($row.Detail) |" }
    if ($failed.Count -gt 0) { throw "$($failed.Count) check(s) failed." }
}

function Invoke-StatusStep {
    param([string]$Value)
    Assert-ReleaseVersion $Value
    $tag = "v$Value"
    function Show-Section {
        param([string]$Title, [scriptblock]$Body)
        Write-Host "== $Title"
        try { & $Body } catch { Write-Host "  unavailable: $($_.Exception.Message)" }
    }
    Show-Section "release.yml run for $tag" {
        $runs = @(Invoke-GhJson @('run', 'list', '--repo', $Repo, '--workflow', 'release.yml', '--branch', $tag, '--limit', '5', '--json', 'databaseId,status,conclusion,event,url'))
        if ($runs.Count -eq 0 -or -not $runs[0]) { Write-Host '  none yet' }
        else { $runs | ForEach-Object { Write-Host "  $($_.event): $($_.status) $($_.conclusion) $($_.url)" } }
    }
    Show-Section 'CHANGELOG pull requests' {
        $prs = @(Invoke-GhJson @('pr', 'list', '--repo', $Repo, '--state', 'all', '--search', "Update CHANGELOG.md for $tag in:title", '--json', 'number,title,state,baseRefName,url'))
        if ($prs.Count -eq 0 -or -not $prs[0]) { Write-Host '  none yet' }
        else { $prs | ForEach-Object { Write-Host "  #$($_.number) $($_.state) -> $($_.baseRefName) $($_.url)" } }
    }
    Show-Section "winget-pkgs pull request for TaskSmack.TaskSmack $Value" {
        $prs = @(Invoke-GhJson @('pr', 'list', '--repo', $WingetRepo, '--state', 'all', '--search', "TaskSmack.TaskSmack $Value in:title", '--json', 'number,title,state,url,labels'))
        $prs = @($prs | Where-Object { $_ -and $_.title -like "*$Value*" })
        if ($prs.Count -eq 0) { Write-Host '  none yet' }
        else { $prs | ForEach-Object { Write-Host "  #$($_.number) $($_.state) [$(($_.labels | ForEach-Object name) -join ', ')] $($_.url)" } }
    }
    Show-Section 'WinGet health issue' {
        $issues = @(Invoke-GhJson @('issue', 'list', '--repo', $Repo, '--label', 'release', '--state', 'open', '--search', 'WinGet: TaskSmack.TaskSmack in:title', '--json', 'number,title,url'))
        if ($issues.Count -eq 0 -or -not $issues[0]) { Write-Host '  none open' }
        else { $issues | ForEach-Object { Write-Host "  #$($_.number) $($_.title) $($_.url)" } }
    }
    Show-Section "milestone $tag" {
        $milestones = @(Invoke-GhJson @('api', "repos/$Repo/milestones?state=all&per_page=100"))
        $milestone = $milestones | Where-Object { $_ -and $_.title -eq $tag } | Select-Object -First 1
        if ($milestone) { Write-Host "  $($milestone.open_issues) open, $($milestone.closed_issues) closed ($($milestone.state))" }
        else { Write-Host '  no such milestone' }
    }
}

# The WinGet step (#4866). It makes the same checks publish-winget makes (release.yml, "Check TaskSmack.TaskSmack is in
# winget-pkgs and has no pull request for this version"): the same contents paths and the same open-PR search. Only a
# confirmed 404 means "not there"; a rate limit, a 5xx or no response is retried with backoff, then it throws.
$WingetPackageId = 'TaskSmack.TaskSmack'
$WingetFolder = "repos/$WingetRepo/contents/manifests/t/TaskSmack/TaskSmack"
$WingetJobName = 'Submit the WinGet Update'
$script:WingetRetryDelay = 5
$script:WingetPollSeconds = 20
$script:WingetWaitMinutes = 40

function Invoke-GhRaw {
    # Runs gh and returns its output lines and exit code without throwing (stderr merged in).
    param([string[]]$Arguments)
    $lines = @(& gh @Arguments 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Lines = $lines }
}

function Get-WingetHttpStatus {
    # 200 or 404 for a contents path; anything else is retried with backoff, then throws.
    param([string]$ApiPath)
    $delay = $script:WingetRetryDelay
    for ($try = 1; $try -le 4; $try++) {
        $out = Invoke-GhRaw @('api', '-i', '--silent', $ApiPath)
        $lines = @($out.Lines)
        $status = if ($lines.Count -gt 0 -and $lines[0] -match '^HTTP/\S+ (\d{3})') { [int]$Matches[1] } else { 0 }
        if ($status -eq 200 -or $status -eq 404) { return $status }
        $detail = if ($status) { ($lines | Where-Object { $_ -match '^(HTTP/|Retry-After|X-Ratelimit-Remaining|X-Ratelimit-Reset)' }) -join '; ' } else { "no response: $($lines -join ' ')" }
        Write-Host "GET $ApiPath, try $try of 4: $detail"
        if ($try -lt 4 -and ($status -eq 0 -or $status -eq 403 -or $status -eq 429 -or $status -ge 500)) {
            Start-Sleep -Seconds $delay
            $delay *= 2
            continue
        }
        throw "GET $ApiPath failed ($detail), so the WinGet state is unknown; run it again."
    }
}

function Search-WingetPullRequests {
    # winget-pkgs pull requests whose title names the package, through the search API (retried like publish-winget's).
    param([string]$Qualifiers)
    $query = "repo:$WingetRepo is:pr $Qualifiers $WingetPackageId in:title" -replace '\s+', ' '
    $found = $null
    for ($try = 1; $try -le 4 -and -not $found; $try++) {
        $out = Invoke-GhRaw @('api', '-X', 'GET', 'search/issues', '-f', "q=$query", '-f', 'sort=created', '-f', 'order=desc')
        if ($out.ExitCode -eq 0) { $found = ($out.Lines -join "`n") | ConvertFrom-Json }
        elseif ($try -lt 4) { Write-Host "Search, try $try of 4, failed: $($out.Lines -join ' ')"; Start-Sleep -Seconds ($script:WingetRetryDelay * [math]::Pow(2, $try - 1)) }
        else { throw "searching $WingetRepo's pull requests failed: $($out.Lines -join ' ')" }
    }
    return @($found.items | Where-Object { $_ -and $_.title.Contains($WingetPackageId) })
}

function Test-WingetTitleHasVersion {
    param([string]$Title, [string]$Value)
    return [bool]($Title -match "(^|[^0-9.])$([regex]::Escape($Value))($|[^0-9.])")
}

function Test-WingetMerged {
    param($Item)
    return [bool]($Item.pull_request -and $Item.pull_request.PSObject.Properties['merged_at'] -and $Item.pull_request.merged_at)
}

function Get-WingetState {
    # Everything the step decides from. Reads only.
    param([string]$Value)
    $packageExists = (Get-WingetHttpStatus $WingetFolder) -eq 200
    $versionExists = $packageExists -and ((Get-WingetHttpStatus "$WingetFolder/$Value") -eq 200)
    $openForVersion = @(Search-WingetPullRequests 'is:open' | Where-Object { Test-WingetTitleHasVersion $_.title $Value })
    $all = @(Search-WingetPullRequests '')
    return [pscustomobject]@{
        PackageExists   = $packageExists
        VersionExists   = $versionExists
        OpenForVersion  = $openForVersion
        AllPullRequests = $all
    }
}

function Get-WingetDecision {
    # Which path a release is on, and whether publish-winget would submit it now (and so a re-run is allowed).
    #   A: the package is in winget-pkgs.   B: it isn't, and its first submission is open (or merged, not yet visible).
    #   C: it isn't, and no first submission is open: closed unmerged, or never made.
    param([string]$Value, $State)
    $versionPrs = @($State.AllPullRequests | Where-Object { Test-WingetTitleHasVersion $_.title $Value })
    if ($State.PackageExists) {
        $eligible = (-not $State.VersionExists) -and @($State.OpenForVersion).Count -eq 0
        return [pscustomobject]@{ Path = 'A'; CanRerun = $eligible; VersionPrs = $versionPrs; FirstSubmissions = @() }
    }
    $first = @($State.AllPullRequests | Where-Object { $_.state -eq 'open' -or (Test-WingetMerged $_) })
    $path = if ($first.Count -gt 0) { 'B' } else { 'C' }
    return [pscustomobject]@{ Path = $path; CanRerun = $false; VersionPrs = $versionPrs; FirstSubmissions = @($State.AllPullRequests) }
}

function Get-WingetPrSummary {
    param($Item)
    $labels = @($Item.labels | ForEach-Object { $_.name })
    $state = if (Test-WingetMerged $Item) { 'merged' } else { $Item.state }
    return "#$($Item.number) $state [$($labels -join ', ')] $($Item.html_url)  $($Item.title)"
}

function Get-WingetHandSubmitCommands {
    param([string]$Value)
    return @(
        "gh release download v$Value --repo $Repo --pattern SHA256SUMS --pattern SHA256SUMS.bundle --dir winget-$Value",
        "pwsh tools/winget-manifest.ps1 -Version $Value -ReleaseDir winget-$Value -ReleaseDate <yyyy-MM-dd>",
        "winget validate --manifest winget-$Value\winget\manifests\t\TaskSmack\TaskSmack\$Value",
        "wingetcreate submit --prtitle `"New package: $WingetPackageId version $Value`" winget-$Value\winget\manifests\t\TaskSmack\TaskSmack\$Value")
}

function Write-WingetReport {
    param([string]$Value, $State, $Decision)
    switch ($Decision.Path) {
        'A' {
            Write-Host "Path A: $WingetPackageId is in $WingetRepo."
            if ($State.VersionExists) { Write-Host "  $Value is already published there." }
            else { Write-Host "  $Value is not published there yet." }
            foreach ($pr in @($Decision.VersionPrs)) {
                Write-Host "  PR: $(Get-WingetPrSummary $pr)"
                if (@($pr.labels | ForEach-Object { $_.name }) -contains 'Needs-CLA') {
                    Write-Host '  Needs-CLA: sign the CLA as tasksmack-boop (comment on the pull request when the bot asks).'
                }
            }
            if (@($Decision.VersionPrs).Count -eq 0 -and -not $State.VersionExists) { Write-Host "  No pull request for $Value (open or merged)." }
        }
        'B' {
            Write-Host "Path B: $WingetPackageId is not in $WingetRepo yet; its first submission is waiting."
            foreach ($pr in @($Decision.FirstSubmissions)) { Write-Host "  First submission: $(Get-WingetPrSummary $pr)" }
            Write-Host "  publish-winget skips $Value until that merges; then: pwsh tools/release.ps1 -Step WinGet -Version $Value -Rerun"
        }
        default {
            Write-Host "Path C: $WingetPackageId is not in $WingetRepo, and no first submission is open."
            foreach ($pr in @($Decision.FirstSubmissions)) { Write-Host "  Earlier: $(Get-WingetPrSummary $pr)" }
            Write-Host '  Submit it by hand (docs/dev/release-process.md, "Publishing to WinGet"):'
            Get-WingetHandSubmitCommands $Value | ForEach-Object { Write-Host "    $_" }
        }
    }
}

function Wait-WingetRerun {
    # Polls the run until the re-run's attempt (one past -AttemptBefore) completes; returns the WinGet job's
    # final state. Right after `gh run rerun` the run can still read as the earlier attempt, completed, so a
    # completed status counts only once the attempt number has moved on.
    param([string]$RunId, [int]$AttemptBefore)
    $deadline = (Get-Date).AddMinutes($script:WingetWaitMinutes)
    Start-Sleep -Seconds $script:WingetPollSeconds
    $run = $null
    $job = $null
    while ((Get-Date) -lt $deadline) {
        $run = Invoke-GhJson @('run', 'view', $RunId, '--repo', $Repo, '--json', 'status,attempt,jobs')
        $job = @($run.jobs) | Where-Object { $_.name -eq $WingetJobName } | Select-Object -First 1
        if ([int]$run.attempt -gt $AttemptBefore -and $run.status -eq 'completed') { break }
        Start-Sleep -Seconds $script:WingetPollSeconds
    }
    if (-not $job -or -not $run -or [int]$run.attempt -le $AttemptBefore -or $run.status -ne 'completed') { throw "The re-run did not finish within $($script:WingetWaitMinutes) minutes: https://github.com/$Repo/actions/runs/$RunId" }
    return $job
}

function Invoke-WingetStep {
    param([string]$Value, [switch]$Rerun)
    Assert-ReleaseVersion $Value
    $tag = "v$Value"
    $state = Get-WingetState $Value
    $decision = Get-WingetDecision -Value $Value -State $state
    Write-WingetReport -Value $Value -State $state -Decision $decision
    if (-not $Rerun) { return }

    if ($decision.Path -ne 'A') { Write-Host "Not re-running: path $($decision.Path); the package must be in $WingetRepo first."; return }
    if ($state.VersionExists) { Write-Host "Not re-running: $Value is already in $WingetRepo."; return }
    if (@($state.OpenForVersion).Count -gt 0) { Write-Host "Not re-running: a pull request for $Value is already open: $($state.OpenForVersion[0].html_url)"; return }

    $runs = @(Invoke-GhJson @('run', 'list', '--repo', $Repo, '--workflow', 'release.yml', '--branch', $tag, '--event', 'push', '--limit', '10', '--json', 'databaseId,status,conclusion,url,headBranch'))
    $run = $runs | Where-Object { $_ -and $_.headBranch -eq $tag } | Select-Object -First 1
    if (-not $run) { Write-Host "Not re-running: no release.yml push run for $tag."; return }
    if ($run.status -ne 'completed') { Write-Host "Not re-running: the $tag run is still $($run.status): $($run.url)"; return }
    $view = Invoke-GhJson @('run', 'view', "$($run.databaseId)", '--repo', $Repo, '--json', 'attempt,jobs')
    $job = @($view.jobs) | Where-Object { $_.name -eq $WingetJobName } | Select-Object -First 1
    if (-not $job) { Write-Host "Not re-running: the $tag run has no job named '$WingetJobName' (publish-winget is skipped for a release that was not Latest): $($run.url)"; return }

    Write-Host "Re-running '$WingetJobName' of $($run.url) (job $($job.databaseId))."
    Invoke-Gh @('run', 'rerun', "$($run.databaseId)", '--repo', $Repo, '--job', "$($job.databaseId)") | Out-Null
    $done = Wait-WingetRerun -RunId "$($run.databaseId)" -AttemptBefore ([int]$view.attempt)
    Write-Host "Result: $($done.conclusion) https://github.com/$Repo/actions/runs/$($run.databaseId)"
    $prs = @(Search-WingetPullRequests '' | Where-Object { Test-WingetTitleHasVersion $_.title $Value })
    if ($prs.Count -eq 0) { Write-Host "No winget-pkgs pull request for $Value turned up (the search can lag a minute; run -Step WinGet -Version $Value again)." }
    foreach ($pr in $prs) {
        Write-Host "PR: $(Get-WingetPrSummary $pr)"
        if (@($pr.labels | ForEach-Object { $_.name }) -contains 'Needs-CLA') { Write-Host 'Needs-CLA: sign the CLA as tasksmack-boop.' }
    }
    if ($done.conclusion -ne 'success') { throw "The re-run of $WingetJobName did not succeed ($($done.conclusion))." }
}

# Dot-sourcing (the tests) loads the functions without running a step.
if ($MyInvocation.InvocationName -eq '.') { return }

if (-not $Step) { throw 'Give -Step Bump, DryRun, Tag, Verify, Status or WinGet (Get-Help tools/release.ps1).' }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'gh (GitHub CLI) is required.' }
switch ($Step) {
    'Bump' { Invoke-BumpStep -Value $Version }
    'DryRun' { Invoke-DryRunStep -Branch $Ref -ExpectedSha $Sha -NoWait:$NoWait }
    'Tag' { Invoke-TagStep -Value $Version -CommitSha $Sha }
    'Verify' { Invoke-VerifyStep -Value $Version }
    'Status' { Invoke-StatusStep -Value $Version }
    'WinGet' { Invoke-WingetStep -Value $Version -Rerun:$Rerun }
}
