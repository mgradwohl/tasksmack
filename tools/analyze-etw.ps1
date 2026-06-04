<#
.SYNOPSIS
    Analyze a TaskSmack ETW CPU trace using xperf.
.DESCRIPTION
    Exports module-level and function-level sampled CPU reports, then prints the top
    TaskSmack-specific modules and functions for a chosen process name / PID.
.PARAMETER TracePath
    Path to the ETW trace (.etl).
.PARAMETER ProcessName
    Process name to filter in the exported xperf reports. Defaults to TaskSmack.exe.
.PARAMETER ProcessId
    Optional PID to disambiguate multiple processes with the same name.
.PARAMETER SymbolPath
    Optional symbol path used for function decoding. Defaults to build/win-profile/bin.
    When analyzing traces captured from win-optimized, function decoding may be limited.
.PARAMETER Top
    Number of rows to print in each summary.
.PARAMETER SkipFunctions
    Skip function-level symbol decoding and only export module-level hotspots.
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl -ProcessId 7412
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl -SkipFunctions
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TracePath,

    [string]$ProcessName = 'TaskSmack.exe',

    [int]$ProcessId,

    [string]$SymbolPath,

    [int]$Top = 20,

    [switch]$SkipFunctions
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Exe,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Exe $($Arguments -join ' ')"
    }
}

function Parse-XperfRows {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string]$TargetProcessName,

        [int]$TargetProcessId
    )

    $pattern = if ($TargetProcessId -ne 0) {
        '^\s*(?:.+?) \(' + [regex]::Escape([string]$TargetProcessId) + '\),\s*([0-9]+),\s*([0-9.]+),\s*(.+)$'
    }
    else {
        '^\s*' + [regex]::Escape($TargetProcessName) + ' \(([0-9]+)\),\s*([0-9]+),\s*([0-9.]+),\s*(.+)$'
    }

    foreach ($line in Get-Content $FilePath) {
        if ($line -match $pattern) {
            if ($TargetProcessId -ne 0) {
                [pscustomobject]@{
                    ProcessName = $TargetProcessName
                    ProcessId   = $TargetProcessId
                    Weight      = [int64]$Matches[1]
                    Usage       = [double]$Matches[2]
                    Symbol      = $Matches[3].Trim()
                }
            }
            else {
                [pscustomobject]@{
                    ProcessName = $TargetProcessName
                    ProcessId   = [int]$Matches[1]
                    Weight      = [int64]$Matches[2]
                    Usage       = [double]$Matches[3]
                    Symbol      = $Matches[4].Trim()
                }
            }
        }
    }
}

$xperf = Get-Command xperf -ErrorAction SilentlyContinue
if (-not $xperf) {
    throw 'xperf not found on PATH.'
}

$resolvedTrace = Resolve-Path $TracePath
$traceFile = $resolvedTrace.Path
$baseName = [IO.Path]::GetFileNameWithoutExtension($traceFile)
$perfDir = Split-Path -Parent $traceFile
$moduleReport = Join-Path $perfDir "$baseName-profile-modules.txt"
$functionReport = Join-Path $perfDir "$baseName-profile-functions.txt"
$summaryPath = Join-Path $perfDir "$baseName-summary.txt"

$symbolDir = if ($SymbolPath) { $SymbolPath } else { Join-Path $repoRoot 'build\win-profile\bin' }
$resolvedSymbolDir = Resolve-Path $symbolDir -ErrorAction Stop
$env:_NT_SYMBOL_PATH = $resolvedSymbolDir.Path
$env:_NT_SYMCACHE_PATH = Join-Path $perfDir 'SymCache'
New-Item -ItemType Directory -Path $env:_NT_SYMCACHE_PATH -Force | Out-Null

& xperf -i $traceFile -quiet -tle -a profile -detail 2>&1 | Set-Content -Path $moduleReport
if ($LASTEXITCODE -ne 0) {
    throw "xperf module export failed with exit code $LASTEXITCODE"
}

if (-not $SkipFunctions) {
    & xperf -i $traceFile -quiet -tle -symbols -a profile -detail 2>&1 | Set-Content -Path $functionReport
    if ($LASTEXITCODE -ne 0) {
        throw "xperf function export failed with exit code $LASTEXITCODE"
    }
}

$parseParams = @{ FilePath = $moduleReport; TargetProcessName = $ProcessName }
if ($PSBoundParameters.ContainsKey('ProcessId')) {
    $parseParams.TargetProcessId = $ProcessId
}

$moduleRows = @(Parse-XperfRows @parseParams)
if ($moduleRows.Count -eq 0) {
    throw "No rows found for $ProcessName in $moduleReport"
}

if (-not $PSBoundParameters.ContainsKey('ProcessId')) {
    $selected = $moduleRows | Group-Object ProcessId | ForEach-Object {
        [pscustomobject]@{
            ProcessId = [int]$_.Name
            Weight = ($_.Group | Measure-Object -Property Weight -Sum).Sum
        }
    } | Sort-Object Weight -Descending | Select-Object -First 1
    $ProcessId = $selected.ProcessId
    $moduleRows = $moduleRows | Where-Object { $_.ProcessId -eq $ProcessId }
}

$functionRows = @()
if ((-not $SkipFunctions) -and (Test-Path $functionReport)) {
    $functionRows = @(Parse-XperfRows -FilePath $functionReport -TargetProcessName $ProcessName -TargetProcessId $ProcessId)
}

$moduleTotal = ($moduleRows | Measure-Object -Property Weight -Sum).Sum
$topModules = $moduleRows | Sort-Object Weight -Descending | Select-Object -First $Top @{n='ProcessSharePct';e={[math]::Round(($_.Weight / $moduleTotal) * 100, 2)}}, Weight, Usage, @{n='Module';e={$_.Symbol}}

$summaryLines = @()
$summaryLines += "Trace: $traceFile"
$summaryLines += "Process: $ProcessName ($ProcessId)"
$summaryLines += "Module report: $moduleReport"
if (-not $SkipFunctions) {
    $summaryLines += "Function report: $functionReport"
}
$summaryLines += ''
$summaryLines += 'Top Modules'
$summaryLines += ($topModules | Format-Table -AutoSize | Out-String -Width 240).TrimEnd()

if ($functionRows.Count -gt 0) {
    $appFunctions = $functionRows | Where-Object { $_.Symbol -like "$ProcessName!*" }
    $functionTotal = ($appFunctions | Measure-Object -Property Weight -Sum).Sum
    $topFunctions = $appFunctions | Sort-Object Weight -Descending | Select-Object -First $Top @{n='CodeSharePct';e={[math]::Round(($_.Weight / $functionTotal) * 100, 2)}}, Weight, Usage, @{n='Function';e={$_.Symbol}}
    $summaryLines += ''
    $summaryLines += 'Top Functions'
    $summaryLines += ($topFunctions | Format-Table -AutoSize | Out-String -Width 240).TrimEnd()
}

$summaryText = $summaryLines -join [Environment]::NewLine
$summaryText | Set-Content -Path $summaryPath
Write-Host $summaryText
Write-Host ''
Write-Host "SUMMARY=$summaryPath"
