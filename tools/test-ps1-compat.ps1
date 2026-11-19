# Tests that the Windows PowerShell 5.1 user gets a clear "needs PowerShell 7" message, not parse errors
# (#4319, UTF-8 rule R19). 5.1 reads a BOM-less script as the ANSI code page, so a non-ASCII byte in a string can
# break the parse before any message appears. Every tools/*.ps1 except newcomer-check.ps1 (which runs on a clean
# Windows's 5.1) declares `#Requires -Version 7`; the three scripts a newcomer meets first are ASCII-only and
# parse under 5.1. Where powershell.exe (5.1) exists, it parses every script and runs those three, which stop at
# the version check before doing anything.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$scripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' | Where-Object { $_.Name -notlike 'test-*' })
Assert-True ($scripts.Count -gt 10) "Expected the tools/*.ps1 scripts, found $($scripts.Count)"
$requiresPattern = '(?im)^#Requires\s+-Version\s+7(\.\d+)?\s*$'

foreach ($script in $scripts) {
    $text = [System.IO.File]::ReadAllText($script.FullName, [System.Text.UTF8Encoding]::new($false))
    if ($script.Name -eq 'newcomer-check.ps1') {
        Assert-True ($text -notmatch $requiresPattern) 'newcomer-check.ps1 runs on Windows PowerShell 5.1 and must not require 7'
        Assert-True ($text -notmatch '[^\x00-\x7F]') 'newcomer-check.ps1 must stay ASCII-only: 5.1 reads it as the ANSI code page'
    }
    else {
        Assert-True ($text -match $requiresPattern) "$($script.Name) needs a '#Requires -Version 7' line, so Windows PowerShell 5.1 says what to run it in"
    }
}

$newcomerScripts = @('setup-dev.ps1', 'check-prereqs.ps1', 'pgo.ps1')
foreach ($name in $newcomerScripts) {
    $text = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot $name), [System.Text.UTF8Encoding]::new($false))
    Assert-True ($text -notmatch '[^\x00-\x7F]') "$name must be ASCII-only (use [char]0x2713 and friends for marks)"
}

$windowsPowerShell = Get-Command powershell.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $windowsPowerShell) {
    Write-Host 'powershell.exe (5.1) not found; skipping the 5.1 parse and run checks'
    Write-Host 'ps1 compat tests passed'
    return
}

# The 5.1 parser, on each script, in one 5.1 process: every script must parse there.
$parseDriver = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-parse-51-$PID.ps1"
try {
    Set-Content -LiteralPath $parseDriver -Encoding utf8 -Value @'
foreach ($path in $args) {
    $errors = $null; $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    if ($errors.Count -gt 0) { "PARSE-ERROR $path $($errors[0].Message)" }
}
"PARSED $($args.Count)"
'@
    $output = & $windowsPowerShell.Source -NoProfile -ExecutionPolicy Bypass -File $parseDriver @($scripts.FullName) 2>&1 | Out-String
    Assert-True ($output -notmatch 'PARSE-ERROR') "Windows PowerShell 5.1 can't parse a tools script: $output"
    Assert-True ($output -match "PARSED $($scripts.Count)") "The 5.1 parse run didn't finish: $output"
}
finally {
    Remove-Item -LiteralPath $parseDriver -Force -ErrorAction SilentlyContinue
}

# Run under 5.1, each of the three stops at the version check, before any of its code.
foreach ($name in $newcomerScripts) {
    $output = & $windowsPowerShell.Source -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $name) 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -ne 0) "$name must fail under Windows PowerShell 5.1"
    Assert-True ($output -match '#requires' -and $output -match '7\.0') "$name under 5.1 must say it requires PowerShell 7, not fail to parse: $output"
}

Write-Host 'ps1 compat tests passed'
