<#
.SYNOPSIS
    Dismiss open CodeQL alerts for dependency/generated paths.
.DESCRIPTION
    Queries GitHub code-scanning alerts and dismisses open alerts whose file paths
    match configured wildcard patterns (for example .cache/fetchcontent/*).
    Requires GitHub CLI (gh) with repo security permissions.
.EXAMPLE
    pwsh tools/dismiss-codeql-dependency-alerts.ps1 -Owner mgradwohl -Repo tasksmack -DryRun
.EXAMPLE
    pwsh tools/dismiss-codeql-dependency-alerts.ps1 -Owner mgradwohl -Repo tasksmack
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Owner,

    [Parameter(Mandatory = $true)]
    [string]$Repo,

    [Parameter()]
    [string[]]$PathPatterns = @(
        '.cache/fetchcontent/*',
        'build/*/gladsources/*',
        'build/*/CMakeFiles/CMakeScratch/*',
        'build/*/CMakeFiles/*/src.c'
    ),

    [Parameter()]
    [ValidateSet("false positive", "won't fix", "used in tests")]
    [string]$DismissedReason = "won't fix",

    [Parameter()]
    [string]$DismissedComment = 'Third-party dependency or generated build artifact.',

    [Parameter()]
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Get-CodeScanningAlerts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Owner,
        [Parameter(Mandatory = $true)]
        [string]$Repo
    )

    # Emit one JSON object per line so multi-page results remain parseable.
    $rawLines = gh api -X GET "repos/$Owner/$Repo/code-scanning/alerts" --paginate --jq '.[] | @json' 2>&1
    if ($LASTEXITCODE -ne 0) {
        $message = ($rawLines | Out-String).Trim()
        if ([string]::IsNullOrWhiteSpace($message)) {
            $message = "gh exited with code $LASTEXITCODE."
        }
        throw "Failed to fetch code-scanning alerts: $message"
    }

    $alerts = @()
    foreach ($line in ($rawLines -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $alerts += ($line | ConvertFrom-Json)
        }
    }

    return $alerts
}

function Test-PathPatternMatch {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string[]]$Patterns
    )

    foreach ($pattern in $Patterns) {
        if ($Path -like $pattern) {
            return $true
        }
    }

    return $false
}

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
    throw 'GitHub CLI (gh) not found in PATH.'
}

Write-Host "Fetching CodeQL alerts for $Owner/$Repo ..."
$alerts = Get-CodeScanningAlerts -Owner $Owner -Repo $Repo

$openAlerts = @($alerts | Where-Object { $_.state -eq 'open' -and $_.tool.name -eq 'CodeQL' })
$targets = @($openAlerts | Where-Object {
    $path = $_.most_recent_instance.location.path
    Test-PathPatternMatch -Path $path -Patterns $PathPatterns
})

Write-Host "Open alerts total: $($openAlerts.Count)"
Write-Host "Open alerts matching patterns: $($targets.Count)"

if ($targets.Count -eq 0) {
    Write-Host 'Nothing to dismiss.'
    exit 0
}

$targets |
    Select-Object number,
                  @{Name = 'rule'; Expression = { $_.rule.id }},
                  @{Name = 'path'; Expression = { $_.most_recent_instance.location.path }} |
    Sort-Object number |
    Format-Table -AutoSize

if ($DryRun) {
    Write-Host 'Dry run enabled; no alerts were dismissed.'
    exit 0
}

$dismissed = 0
foreach ($alert in $targets) {
    $number = $alert.number
    $patchOutput = gh api -X PATCH "repos/$Owner/$Repo/code-scanning/alerts/$number" `
        -f "state=dismissed" `
        -f "dismissed_reason=$DismissedReason" `
        -f "dismissed_comment=$DismissedComment" 2>&1

    if ($LASTEXITCODE -ne 0) {
        $patchMessage = ($patchOutput | Out-String).Trim()
        if ([string]::IsNullOrWhiteSpace($patchMessage)) {
            $patchMessage = "gh exited with code $LASTEXITCODE."
        }
        throw "Failed to dismiss alert #${number}: $patchMessage"
    }

    $dismissed++
    Write-Host "Dismissed alert #$number"
}

$alertsAfter = Get-CodeScanningAlerts -Owner $Owner -Repo $Repo
$remaining = (@($alertsAfter | Where-Object {
            $_.state -eq 'open' -and
            $_.tool.name -eq 'CodeQL' -and
            (Test-PathPatternMatch -Path $_.most_recent_instance.location.path -Patterns $PathPatterns)
        })).Count

Write-Host "Dismissed: $dismissed"
Write-Host "Remaining matching open alerts: $remaining"
