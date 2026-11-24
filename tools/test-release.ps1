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
    )) {
    $result = Invoke-Release $case.Args
    Assert-True ($result.ExitCode -ne 0) "release.ps1 $($case.Args -join ' ') should fail"
    Assert-True ($result.Output -match $case.Expect) "release.ps1 $($case.Args -join ' '): expected /$($case.Expect)/ in: $($result.Output)"
}

Write-Host 'release tests passed'
