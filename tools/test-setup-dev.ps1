# Tests for tools/setup-dev.ps1's winget install helper (#2844): a pinned version winget's catalog no
# longer carries (0x8A150017, "No version found matching") installs the catalog's newest version
# with a warning, and prints what got installed, instead of stopping the script; every other
# failure still stops it, and the already-installed codes still pass. Install-WinGetPackage, the
# functions and the $WinGet* tables it reads are taken from the script with the PowerShell parser
# and run against a stub winget function that records its calls; one -DryRun run of the whole
# script, with a stub winget.cmd first on PATH, checks the commands it prints. Nothing is installed.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$setup = Join-Path $PSScriptRoot 'setup-dev.ps1'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($setup, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "setup-dev.ps1 does not parse: $errors"

# The exit-code tables and the common install switches, from the script's own top-level assignments.
$tables = @($ast.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -like '$WinGet*'
        }, $false))
Assert-True ($tables.Count -ge 4) "Expected the `$WinGet* tables at the top of setup-dev.ps1, found $($tables.Count)"
foreach ($table in $tables) {
    . ([scriptblock]::Create($table.Extent.Text))
}
foreach ($name in 'Invoke-WinGet', 'Get-WinGetInstalledVersion', 'Install-WinGetPackage') {
    $definition = $ast.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }.GetNewClosure(), $true)
    Assert-True ($null -ne $definition) "$name not found in setup-dev.ps1"
    . ([scriptblock]::Create($definition.Extent.Text))
}
$DryRun = $false

# The stub winget records each call's arguments, prints what $script:Stub answers for it, and exits
# with the code it answers, as winget.exe would. $script:Stub takes the joined arguments and returns
# @{ ExitCode; Output }.
$script:Calls = [System.Collections.Generic.List[string]]::new()
$script:Stub = { param($Line) @{ ExitCode = 0; Output = @() } }
function winget {
    param([Parameter(ValueFromRemainingArguments)][string[]]$CommandLine)
    $line = $CommandLine -join ' '
    $script:Calls.Add($line)
    $answer = & $script:Stub $line
    $answer.Output
    $global:LASTEXITCODE = $answer.ExitCode
}

function Set-Stub {
    param([scriptblock]$Stub)
    $script:Calls.Clear()
    $script:Stub = $Stub
}

$switches = '--source winget --silent --accept-package-agreements --accept-source-agreements'
$pinned = "install --id Kitware.CMake --version 3.31.12 $switches"
$newest = "install --id Kitware.CMake $switches"
$list = 'list --id Kitware.CMake --source winget --accept-source-agreements --disable-interactivity'
$noManifest = 0x8A150017
$listRows = @('Name  Id            Version', '---------------------------', 'CMake Kitware.CMake 4.4.4')
$calls = $script:Calls

# ── The pin is in the catalog: one install, no warning ─────────────────────────────────────────
Set-Stub { param($Line) @{ ExitCode = 0; Output = @() } }
$null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 -WarningVariable warnings
Assert-True ($calls.Count -eq 1 -and $calls[0] -eq $pinned) "pin present: calls [$($calls -join '; ')]"
Assert-True (@($warnings).Count -eq 0) "pin present: warned [$warnings]"

# ── #2844: the pin is gone (0x8A150017): warn, install the newest, print what got installed ────
Set-Stub {
    param($Line)
    if ($Line -like '*--version 3.31.12*') { return @{ ExitCode = $noManifest; Output = @('No version found matching: 3.31.12') } }
    if ($Line -like 'list *') { return @{ ExitCode = 0; Output = $listRows } }
    @{ ExitCode = 0; Output = @('Successfully installed') }
}
$null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 -WarningVariable warnings -InformationVariable info
Assert-True ($calls.Count -eq 3 -and $calls[0] -eq $pinned -and $calls[1] -eq $newest -and $calls[2] -eq $list) "pin gone: calls [$($calls -join '; ')]"
Assert-True (@($warnings).Count -eq 1 -and $warnings[0] -like '*0x8A150017*Kitware.CMake 3.31.12*no longer in the winget catalog*') "pin gone: warnings [$warnings]"
$installedLine = @($info | Where-Object { "$_" -like '*is installed*' })
Assert-True ($installedLine.Count -eq 1 -and "$($installedLine[0])" -like '*Kitware.CMake 4.4.4 is installed (the pin was 3.31.12)*') "pin gone: installed line [$installedLine] in [$info]"

# The pin is gone and winget list can't say what got installed: the fallback still installs, and the
# line points at check-prereqs.ps1 instead of a version.
foreach ($case in @(
        @{ Name = 'list fails'; ExitCode = 0x8A150014; Output = @('No installed package found matching input criteria.') }
        @{ Name = 'list row lacks the id'; ExitCode = 0; Output = @('Name  Id  Version', 'CMake Kitware.CMa… 4.4.4') }
    )) {
    Set-Stub {
        param($Line)
        if ($Line -like '*--version 3.31.12*') { return @{ ExitCode = $noManifest; Output = @() } }
        if ($Line -like 'list *') { return @{ ExitCode = $case.ExitCode; Output = $case.Output } }
        @{ ExitCode = 0; Output = @() }
    }.GetNewClosure()
    $null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 -WarningVariable warnings -InformationVariable info
    Assert-True ($calls.Count -eq 3 -and $calls[1] -eq $newest) "$($case.Name): calls [$($calls -join '; ')]"
    $installedLine = @($info | Where-Object { "$_" -like '*Kitware.CMake installed (the pin was 3.31.12); run pwsh tools/check-prereqs.ps1*' })
    Assert-True ($installedLine.Count -eq 1) "$($case.Name): lines [$info]"
}

# The pin is gone and the fallback install fails too: that failure stops the script.
Set-Stub {
    param($Line)
    if ($Line -like '*--version 3.31.12*') { return @{ ExitCode = $noManifest; Output = @() } }
    @{ ExitCode = 0x8A150011; Output = @() }
}
$thrown = $null
try { $null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like 'winget failed with exit code 0x8A150011*') "fallback fails: [$thrown]"
Assert-True ($calls.Count -eq 2) "fallback fails: calls [$($calls -join '; ')]"

# ── Any other failure of the pinned install still stops the script, with no fallback ───────────
Set-Stub { param($Line) @{ ExitCode = 0x8A150011; Output = @('Internal error') } }
$thrown = $null
try { $null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like "winget failed with exit code 0x8A150011 (*): winget $pinned") "other failure: [$thrown]"
Assert-True ($calls.Count -eq 1) "other failure: calls [$($calls -join '; ')]"

# ── Already installed: as before, no throw and no fallback ─────────────────────────────────────
foreach ($case in @(
        @{ Code = 0x8A15002B; Warnings = 0 }
        @{ Code = 0x8A150061; Warnings = 0 }
        @{ Code = 0x8A15010D; Warnings = 1 }
    )) {
    Set-Stub { param($Line) @{ ExitCode = $case.Code; Output = @() } }.GetNewClosure()
    $null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 -WarningVariable warnings
    $label = '0x{0:X8}' -f $case.Code
    Assert-True ($calls.Count -eq 1 -and $calls[0] -eq $pinned) "$label`: calls [$($calls -join '; ')]"
    Assert-True (@($warnings).Count -eq $case.Warnings) "$label`: warnings [$warnings]"
}

# ── No pin: the newest version, and -Arguments after the common switches ───────────────────────
Set-Stub { param($Line) @{ ExitCode = 0; Output = @() } }
$null = Install-WinGetPackage -Id Microsoft.VisualStudio.2022.BuildTools -Arguments @('--override', '--wait --passive') -WarningVariable warnings
Assert-True ($calls.Count -eq 1 -and $calls[0] -eq "install --id Microsoft.VisualStudio.2022.BuildTools $switches --override --wait --passive") "no pin: calls [$($calls -join '; ')]"
Assert-True (@($warnings).Count -eq 0) "no pin: warned [$warnings]"

# ── -DryRun: the command is printed, with the fallback it would take, and winget never runs ─────
$DryRun = $true
Set-Stub { param($Line) throw "winget ran under -DryRun: $Line" }
$null = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 -InformationVariable info
$DryRun = $false
Assert-True ($calls.Count -eq 0) "dry-run: calls [$($calls -join '; ')]"
$lines = @($info | ForEach-Object { "$_" })
Assert-True ($lines.Count -eq 2 -and $lines[0] -eq "[dry-run] winget $pinned" -and $lines[1] -like '*if 3.31.12 is no longer in the winget catalog*') "dry-run: lines [$lines]"

# ── The whole script under -DryRun, with a stub winget.cmd first on PATH ───────────────────────
# Its install lines carry the -*Version parameters; the stub makes any real winget call visible.
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-setup-dev-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $root 'winget.cmd'), "@echo STUB WINGET RAN %*`r`n@exit /b 1`r`n")
    $env:PATH = "$root$([IO.Path]::PathSeparator)$env:PATH"
    $pwsh = (Get-Process -Id $PID).Path
    $output = & $pwsh -NoProfile -File $setup -DryRun -CMakeVersion 9.9.9 -NinjaVersion 8.8.8 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "setup-dev.ps1 -DryRun exited $LASTEXITCODE`:`n$output"
    Assert-True (-not $output.Contains('STUB WINGET RAN')) "-DryRun ran winget:`n$output"
    foreach ($expected in @(
            "[dry-run] winget install --id Kitware.CMake --version 9.9.9 $switches"
            '[dry-run]   (if 9.9.9 is no longer in the winget catalog'
            "[dry-run] winget install --id Ninja-build.Ninja --version 8.8.8 $switches"
            '[dry-run] winget install --id LLVM.LLVM --version '
            "[dry-run] winget install --id Python.Python.3.14 $switches"
        )) {
        Assert-True ($output.Contains($expected)) "-DryRun output lacks [$expected]:`n$output"
    }

    Write-Host 'setup-dev.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
