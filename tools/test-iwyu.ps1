# Tests for tools/iwyu.ps1's wslpath call (#4321): WSL prints a path as UTF-8, so a checkout under a non-ASCII
# user name (C:\Users\Jos<e acute>\...) must come back intact, not decoded as the OEM code page. Get-WslPath is
# taken from the script with the PowerShell parser and run against a stub wsl.cmd that prints the UTF-8 bytes of
# such a path, with the console set to an OEM code page to prove it doesn't matter; WSL itself never runs.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'iwyu.ps1'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "iwyu.ps1 does not parse: $errors"
$definition = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-WslPath'
    }, $true)
Assert-True ($null -ne $definition) 'Get-WslPath not found in iwyu.ps1'
. ([scriptblock]::Create($definition.Extent.Text))

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-iwyu-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
$savedEncoding = $null
try {
    $savedEncoding = [Console]::OutputEncoding
    # A typical OEM code page; where there is no console to set it on, the helper must still read UTF-8.
    try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(437) } catch { Write-Verbose "Console encoding unchanged: $_" }

    $wsl = Join-Path $root 'wsl.cmd'
    [IO.File]::WriteAllText($wsl, "@type `"%~dp0out.txt`"`r`n@exit /b %WSL_STUB_EXIT%`r`n")

    # /mnt/c/Users/Jos<e acute>/<two CJK characters>/tasksmack, as the bytes WSL's Linux side prints.
    $linuxPath = '/mnt/c/Users/Jos' + [char]0xE9 + '/' + [char]0x738B + [char]0x4F1F + '/tasksmack'
    [IO.File]::WriteAllBytes((Join-Path $root 'out.txt'), [System.Text.UTF8Encoding]::new($false).GetBytes($linuxPath + "`n"))

    $env:WSL_STUB_EXIT = '0'
    $result = Get-WslPath -Wsl $wsl -Path ('C:\Users\Jos' + [char]0xE9 + '\tasksmack')
    Assert-True ($result.ExitCode -eq 0) "exit code was $($result.ExitCode)"
    Assert-True ($result.Path -ceq $linuxPath) "the path came back as [$($result.Path)], expected [$linuxPath]"

    $env:WSL_STUB_EXIT = '1'
    $failed = Get-WslPath -Wsl $wsl -Path 'C:\nowhere'
    Assert-True ($failed.ExitCode -eq 1) "a failing wslpath must report its exit code, got $($failed.ExitCode)"

    Write-Host 'iwyu.ps1 tests passed'
}
finally {
    Remove-Item Env:WSL_STUB_EXIT -ErrorAction SilentlyContinue
    if ($null -ne $savedEncoding) {
        try { [Console]::OutputEncoding = $savedEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
