# Tests for tools/release.ps1 (#4854): its argument checks, and the commands and text it builds. gh is never
# called for a step that is refused up front, and the Tag step only prints. Registered in CTest on Windows with
# PowerShell 7; can also run directly with pwsh -File. Nothing is downloaded, pushed or dispatched.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'release.ps1'
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "release.ps1 does not parse: $errors"

. $script

# Version and branch helpers.
foreach ($good in '0.11.1', '1.0.0', '10.20.30') { Assert-True (Test-ReleaseVersion $good) "$good should be accepted" }
foreach ($bad in 'v0.11.1', '0.11', '0.11.1.2', '0.11.x', '', ' 0.11.1') { Assert-True (-not (Test-ReleaseVersion $bad)) "'$bad' should be refused" }
Assert-True ((Get-ReleaseBranch '0.11.1') -eq 'release/0.11') 'release branch of 0.11.1'
Assert-True ((Get-ReleaseBranch '1.20.3') -eq 'release/1.20') 'release branch of 1.20.3'

# The assets a release has, and the tag commands.
$assets = Get-ExpectedAssets '0.11.1'
foreach ($name in 'TaskSmack-0.11.1-win64.exe', 'TaskSmack-0.11.1-windows-arm64.exe', 'TaskSmack-0.11.1-win64.zip',
    'TaskSmack-0.11.1-windows-arm64.zip', 'TaskSmack-0.11.1-Linux.deb', 'SHA256SUMS', 'SHA256SUMS.bundle') {
    Assert-True ($name -in $assets) "$name missing from the expected assets"
}
$sha = 'a' * 40
$commands = Get-TagCommands -Value '0.11.1' -CommitSha $sha
Assert-True ($commands -contains "git tag -a v0.11.1 $sha -m `"TaskSmack 0.11.1`"") "tag command: $commands"
Assert-True ($commands -contains 'git push origin v0.11.1') "push command: $commands"

# The expected signer is the workflow's.
Assert-True ((Get-ExpectedSigner) -eq 'Matt Gradwohl') 'expected signer'

# The bump pull request follows the template's headings and names the one changed line.
$body = Get-BumpPullRequestBody -Value '0.11.1' -Branch 'release/0.11'
foreach ($heading in '## Description', '## PR Size', '## Type of Change', '## Checklist', '## Testing', '## Additional Notes') {
    Assert-True ($body.Contains($heading)) "bump body lacks '$heading'"
}
Assert-True ($body.Contains('project(TaskSmack VERSION 0.11.1 LANGUAGES C CXX)')) 'bump body names the line'

# Argument checks, run as a real process so a throw is an exit code. Each is refused before gh is called.
function Invoke-Release {
    param([string[]]$Arguments)
    $output = & pwsh -NoProfile -File $script @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}
foreach ($case in @(
        @{ Args = @(); Expect = 'Give -Step' }
        @{ Args = @('-Step', 'Bogus'); Expect = 'ValidateSet|Bogus' }
        @{ Args = @('-Step', 'Bump', '-Version', 'v0.11.1'); Expect = 'X\.Y\.Z' }
        @{ Args = @('-Step', 'Bump'); Expect = 'X\.Y\.Z' }
        @{ Args = @('-Step', 'Bump', '-Version', '0.11.0'); Expect = 'patch releases' }
        @{ Args = @('-Step', 'Tag', '-Version', '0.11.1'); Expect = '-Sha' }
        @{ Args = @('-Step', 'Tag', '-Version', '0.11.1', '-Sha', 'abc123'); Expect = '40-hex' }
        @{ Args = @('-Step', 'Tag', '-Version', '0.11', '-Sha', ('a' * 40)); Expect = 'X\.Y\.Z' }
        @{ Args = @('-Step', 'DryRun'); Expect = '-Ref' }
        @{ Args = @('-Step', 'DryRun', '-Ref', ('b' * 40)); Expect = 'branch name' }
        @{ Args = @('-Step', 'DryRun', '-Ref', 'release/0.11', '-Sha', 'short'); Expect = '40-hex' }
        @{ Args = @('-Step', 'Verify', '-Version', '0.11'); Expect = 'X\.Y\.Z' }
        @{ Args = @('-Step', 'Status', '-Version', 'latest'); Expect = 'X\.Y\.Z' }
        @{ Args = @('-Step', 'WinGet', '-Version', 'latest'); Expect = 'X\.Y\.Z' }
    )) {
    $result = Invoke-Release $case.Args
    Assert-True ($result.ExitCode -ne 0) "release.ps1 $($case.Args -join ' ') should fail"
    Assert-True ($result.Output -match $case.Expect) "release.ps1 $($case.Args -join ' '): expected /$($case.Expect)/ in: $($result.Output)"
}

# The WinGet step (#4866): the path decision and the re-run guard, with gh mocked. Functions redefined here win over
# release.ps1's, so no gh call, sleep or network is made.
function Start-Sleep { param([int]$Seconds) }
function New-FakePr {
    param([int]$Number, [string]$Title, [string]$State = 'open', [string[]]$Labels = @(), [bool]$Merged = $false)
    return [pscustomobject]@{
        number = $Number; title = $Title; state = $State; html_url = "https://example.test/pull/$Number"
        labels = @($Labels | ForEach-Object { [pscustomobject]@{ name = $_ } })
        pull_request = [pscustomobject]@{ merged_at = $(if ($Merged) { '2026-11-24T00:00:00Z' } else { $null }) }
    }
}
function New-FakeState {
    param([bool]$Package, [bool]$Version = $false, $Open = @(), $All = @())
    return [pscustomobject]@{ PackageExists = $Package; VersionExists = $Version; OpenForVersion = @($Open); AllPullRequests = @($All) }
}
$first = New-FakePr 1 'New package: TaskSmack.TaskSmack version 0.11.0'
$update = New-FakePr 2 'Update: TaskSmack.TaskSmack to 0.11.1' -Labels 'Needs-CLA'

$d = Get-WingetDecision -Value '0.11.1' -State (New-FakeState $true -All @($first))
Assert-True ($d.Path -eq 'A' -and $d.CanRerun) 'package present, no version, no open PR: path A, re-run allowed'
$d = Get-WingetDecision -Value '0.11.1' -State (New-FakeState $true -Open @($update) -All @($update, $first))
Assert-True ($d.Path -eq 'A' -and -not $d.CanRerun -and @($d.VersionPrs).Count -eq 1) 'open PR for the version: path A, no re-run'
$d = Get-WingetDecision -Value '0.11.1' -State (New-FakeState $true -Version $true -All @($first))
Assert-True ($d.Path -eq 'A' -and -not $d.CanRerun) 'version already there: no re-run'
$d = Get-WingetDecision -Value '0.11.0' -State (New-FakeState $false -All @($first))
Assert-True ($d.Path -eq 'B' -and -not $d.CanRerun) 'package absent, first submission open: path B'
$d = Get-WingetDecision -Value '0.11.0' -State (New-FakeState $false -All @((New-FakePr 1 $first.title -State 'closed')))
Assert-True ($d.Path -eq 'C' -and -not $d.CanRerun) 'package absent, first submission closed unmerged: path C'
$d = Get-WingetDecision -Value '0.11.0' -State (New-FakeState $false -All @((New-FakePr 1 $first.title -State 'closed' -Merged $true)))
Assert-True ($d.Path -eq 'B') 'package absent, first submission merged but not visible yet: path B'
$d = Get-WingetDecision -Value '0.11.0' -State (New-FakeState $false)
Assert-True ($d.Path -eq 'C') 'package absent, no submission at all: path C'
Assert-True (Test-WingetTitleHasVersion 'Update: TaskSmack.TaskSmack to 0.11.1' '0.11.1') 'version in title'
Assert-True (-not (Test-WingetTitleHasVersion 'Update: TaskSmack.TaskSmack to 0.11.10' '0.11.1')) '0.11.10 is not 0.11.1'
Assert-True (-not (Test-WingetTitleHasVersion 'Update: TaskSmack.TaskSmack to 10.11.1' '0.11.1')) '10.11.1 is not 0.11.1'
Assert-True (@(@(Get-WingetHandSubmitCommands '0.11.0') -match 'wingetcreate submit').Count -eq 1) 'hand-submit commands'

# Status codes: a 404 is absent, a 200 present, a 5xx or rate limit is retried and then throws.
$script:ghCalls = @()
$script:ghReplies = @()
function Invoke-GhRaw {
    param([string[]]$Arguments)
    $script:ghCalls += , $Arguments
    $reply = $script:ghReplies[[math]::Min($script:ghCalls.Count, $script:ghReplies.Count) - 1]
    return [pscustomobject]@{ ExitCode = 0; Lines = @($reply) }
}
$script:ghReplies = @('HTTP/2.0 404 Not Found'); $script:ghCalls = @()
Assert-True ((Get-WingetHttpStatus 'repos/x/y') -eq 404) '404 is absent'
$script:ghReplies = @('HTTP/2.0 200 OK'); $script:ghCalls = @()
Assert-True ((Get-WingetHttpStatus 'repos/x/y') -eq 200) '200 is present'
$script:ghReplies = @('HTTP/2.0 502 Bad Gateway', 'HTTP/2.0 403 Forbidden', 'HTTP/2.0 200 OK'); $script:ghCalls = @()
Assert-True ((Get-WingetHttpStatus 'repos/x/y') -eq 200) 'a 5xx and a rate limit are retried'
Assert-True ($script:ghCalls.Count -eq 3) "three tries, got $($script:ghCalls.Count)"
$script:ghReplies = @('HTTP/2.0 500 Server Error'); $script:ghCalls = @()
$threw = $false
try { Get-WingetHttpStatus 'repos/x/y' | Out-Null } catch { $threw = $true }
Assert-True ($threw -and $script:ghCalls.Count -eq 4) 'a persistent 5xx throws after four tries'
$script:ghReplies = @('HTTP/2.0 401 Unauthorized'); $script:ghCalls = @()
$threw = $false
try { Get-WingetHttpStatus 'repos/x/y' | Out-Null } catch { $threw = $true }
Assert-True ($threw -and $script:ghCalls.Count -eq 1) 'a 401 throws at once'

# The re-run guard: gh run rerun is called only for path A with the version absent and no open PR.
$script:reruns = @()
$script:fake = $null
function Get-WingetState { param([string]$Value) return $script:fake }
function Invoke-GhJson {
    param([string[]]$Arguments)
    if ($Arguments[0] -eq 'run' -and $Arguments[1] -eq 'list') {
        return @([pscustomobject]@{ databaseId = 99; status = 'completed'; conclusion = 'success'; url = 'https://example.test/run/99'; headBranch = 'v0.11.1' })
    }
    if ($Arguments[0] -eq 'run' -and $Arguments[1] -eq 'view') {
        # The run's attempt moves on with each re-run, as GitHub's does; the wait counts only the new attempt.
        return [pscustomobject]@{ status = 'completed'; attempt = 1 + @($script:reruns).Count; jobs = @([pscustomobject]@{ name = 'Submit the WinGet Update'; databaseId = 555; conclusion = 'success' }) }
    }
    throw "unexpected gh $($Arguments -join ' ')"
}
function Invoke-Gh {
    param([string[]]$Arguments)
    if ($Arguments[0] -eq 'run' -and $Arguments[1] -eq 'rerun') { $script:reruns += , $Arguments; return @() }
    throw "unexpected gh $($Arguments -join ' ')"
}
function Search-WingetPullRequests { param([string]$Qualifiers) return @($update) }

foreach ($case in @(
        @{ Name = 'path B'; State = (New-FakeState $false -All @($first)) }
        @{ Name = 'path C'; State = (New-FakeState $false) }
        @{ Name = 'version present'; State = (New-FakeState $true -Version $true -All @($first)) }
        @{ Name = 'open PR'; State = (New-FakeState $true -Open @($update) -All @($update, $first)) }
    )) {
    $script:fake = $case.State; $script:reruns = @()
    Invoke-WingetStep -Value '0.11.1' -Rerun *> $null
    Assert-True ($script:reruns.Count -eq 0) "-Rerun must not re-run on $($case.Name)"
}
$script:fake = New-FakeState $true -All @($first); $script:reruns = @()
Invoke-WingetStep -Value '0.11.1' *> $null
Assert-True ($script:reruns.Count -eq 0) 'without -Rerun nothing is re-run'
$script:fake = New-FakeState $true -All @($first)
$output = Invoke-WingetStep -Value '0.11.1' -Rerun 6>&1 | Out-String
Assert-True ($script:reruns.Count -eq 1) 'path A, version absent, no open PR: one re-run'
Assert-True (($script:reruns[0] -join ' ') -eq 'run rerun 99 --repo mgradwohl/tasksmack --job 555') "rerun arguments: $($script:reruns[0] -join ' ')"
Assert-True ($output -match 'Result: success' -and $output -match 'Needs-CLA: sign the CLA as tasksmack-boop') "re-run output: $output"

Write-Host 'release tests passed'
