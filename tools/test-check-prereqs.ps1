# Tests for tools/check-prereqs.ps1's ccache version parse (#2350): a two-component version
# (ccache 4.14, which setup-dev.ps1 pins) is recognized like a three-component one. Get-CcacheVersion
# is taken from the script with the PowerShell parser and run against a stub ccache function; one
# full run of the checker with a stub ccache.cmd first on PATH checks the row it prints. Registered
# in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$checker = Join-Path $PSScriptRoot 'check-prereqs.ps1'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($checker, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "check-prereqs.ps1 does not parse: $errors"
$definition = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-CcacheVersion'
    }, $true)
Assert-True ($null -ne $definition) 'Get-CcacheVersion not found in check-prereqs.ps1'
. ([scriptblock]::Create($definition.Extent.Text))

$minimum = [Version]'4.9.1'
$script:StubOutput = @()
function ccache { $script:StubOutput }

foreach ($case in @(
        @{ Output = @('ccache version 4.14'); Expected = '4.14'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.14.1', 'Features: file-storage http-storage'); Expected = '4.14.1'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.9.1'); Expected = '4.9.1'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.2'); Expected = '4.2'; MeetsMinimum = $false }
        @{ Output = @('ccache, but not a version line'); Expected = $null; MeetsMinimum = $false }
        @{ Output = @(); Expected = $null; MeetsMinimum = $false }
    )) {
    $script:StubOutput = $case.Output
    $version = Get-CcacheVersion
    $label = "[$($case.Output -join ' | ')]"
    if ($null -eq $case.Expected) {
        Assert-True ($null -eq $version) "$label parsed as [$version], expected none"
    }
    else {
        Assert-True ($version -is [Version] -and $version -eq [Version]$case.Expected) "$label parsed as [$version], expected $($case.Expected)"
    }
    Assert-True ([bool]($version -and $version -ge $minimum) -eq $case.MeetsMinimum) "$label against $minimum"
}

# The checker itself, with a stub ccache.cmd ahead of any real ccache: its row shows the version.
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-check-prereqs-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $root 'ccache.cmd'), "@echo ccache version 4.14`r`n")
    $env:PATH = "$root$([IO.Path]::PathSeparator)$env:PATH"
    $pwsh = (Get-Process -Id $PID).Path
    $output = & $pwsh -NoProfile -File $checker 2>&1 | Out-String
    $row = @($output -split '\r?\n' | Where-Object { $_ -match '^\S+ ccache\s' })
    Assert-True ($row.Count -eq 1) "Expected one ccache row:`n$output"
    Assert-True ($row[0] -match '^\S+ ccache\s+4\.14\s') "ccache row: [$($row[0])]"
    Assert-True ($row[0] -like '*ccache.cmd*') "ccache row does not show the stub's path: [$($row[0])]"

    Write-Host 'check-prereqs.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
