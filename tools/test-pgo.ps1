# Tests for tools/pgo.ps1's command line: an explicit help request prints the usage and exits 0, an
# unknown command prints it and exits 1 (#4239). Neither runs a build, a benchmark or LLVM detection.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$hostPwsh = (Get-Process -Id $PID).Path
$pgoScript = Join-Path $PSScriptRoot 'pgo.ps1'

foreach ($flag in @('--help', '-h', '-Help', 'help', '-?')) {
    $out = & $hostPwsh -NoProfile -File $pgoScript $flag 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "pgo.ps1 $flag must exit 0, exited $LASTEXITCODE`: $out"
    Assert-True ($out -like '*Usage: pwsh tools/pgo.ps1*') "pgo.ps1 $flag must print the usage: $out"
}

$unknown = & $hostPwsh -NoProfile -File $pgoScript 'nonsense' 2>&1 | Out-String
Assert-True ($LASTEXITCODE -eq 1) "An unknown command must exit 1, exited $LASTEXITCODE`: $unknown"
Assert-True ($unknown -like '*Usage: pwsh tools/pgo.ps1*') "An unknown command must print the usage: $unknown"

Write-Host 'pgo tests passed'
