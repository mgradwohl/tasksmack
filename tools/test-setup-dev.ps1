# Tests for tools/setup-dev.ps1's winget install helper (#2844): a pinned version winget's catalog no
# longer carries (0x8A150017, "No version found matching") installs the catalog's newest version
# with a warning, and prints what got installed, instead of stopping the script; every other
# failure still stops it, and the already-installed codes still pass. Install-WinGetPackage, the
# functions and the $WinGet* tables it reads are taken from the script with the PowerShell parser
# and run against a stub winget function that records its calls; one -DryRun run of the whole
# script, with a stub winget.cmd first on PATH, checks the commands it prints. Nothing is installed.
# The LLVM step (#3041) is tested the same way: Get-LlvmInstallPlan's decision for each kind of
# existing install, the uninstall commands, and Install-PinnedLlvm's run against stub winget,
# Start-Process and installed-LLVM readers, failures included. Nothing is uninstalled either; the
# registry reader runs against a scratch key under HKCU that the test deletes.
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
foreach ($name in 'Invoke-WinGet', 'Get-WinGetInstalledVersion', 'Install-WinGetPackage', 'Get-InstalledLlvm',
    'Get-LlvmInstallPlan', 'Get-LlvmUninstallCommand', 'Invoke-LlvmUninstall', 'Install-PinnedLlvm') {
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

# ── LLVM (#3041): one install, of the pinned major ─────────────────────────────────────────────
$LlvmDir = 'C:\Program Files\LLVM'
$LlvmVersion = '23.1.3'
$LlvmTargetMajor = 23
# A registration as Get-InstalledLlvm returns it. Its key doesn't exist, so an uninstall of it
# counts as finished once the uninstaller exits.
function New-Registration {
    param([string]$Version, [string]$ProductCode, [string]$UninstallString, [string]$QuietUninstallString)
    [pscustomobject]@{
        Version              = $Version
        Major                = if ($Version -match '^(\d+)') { [int]$Matches[1] } else { $null }
        ProductCode          = if ($ProductCode) { $ProductCode } else { $null }
        UninstallString      = if ($UninstallString) { $UninstallString } elseif ($ProductCode) { "MsiExec.exe /X$ProductCode" } else { '"C:\Program Files\LLVM\Uninstall.exe"' }
        QuietUninstallString = $QuietUninstallString
        Key                  = "HKCU:\Software\TaskSmackTests\absent-$([guid]::NewGuid().ToString('N'))"
    }
}
$msi22 = New-Registration -Version '22.1.5' -ProductCode '{22222222-2222-2222-2222-222222222222}'
$nsis22 = New-Registration -Version '22.1.8'
$msi23a = New-Registration -Version '23.1.2' -ProductCode '{F51D2584-CF0B-491B-8D94-5883E3B254B0}'
$msi23b = New-Registration -Version '23.1.3' -ProductCode '{2B2859B7-37CF-4E78-B617-72952CADA59B}'
$msi23c = New-Registration -Version '23.1.4' -ProductCode '{C261B515-7573-457D-AA83-FE1826A77ECE}'

# The decision, for each kind of existing install.
foreach ($case in @(
        @{ Name = 'none installed'; Installed = @(); Clang = $null; Pin = 23; Action = 'Install'; Remove = @(); Note = $null }
        @{ Name = 'unregistered LLVM 22 on disk'; Installed = @(); Clang = 22; Pin = 23; Action = 'Install'; Remove = @(); Note = '*LLVM 22 is in*without an installer registration; installing LLVM 23 over it*' }
        @{ Name = 'unregistered LLVM 23 on disk'; Installed = @(); Clang = 23; Pin = 23; Action = 'Keep'; Remove = @(); Note = 'LLVM 23 is in C:\Program Files\LLVM without an installer registration.' }
        @{ Name = 'same major'; Installed = @($msi23c); Clang = 23; Pin = 23; Action = 'Keep'; Remove = @(); Note = $null }
        @{ Name = 'same major, older minor than the pin'; Installed = @($msi23a); Clang = 23; Pin = 23; Action = 'Keep'; Remove = @(); Note = $null }
        @{ Name = 'same major x3'; Installed = @($msi23a, $msi23b, $msi23c); Clang = 23; Pin = 23; Action = 'Keep'; Remove = @(); Note = '3 installer registrations of LLVM 23 (23.1.2, 23.1.3, 23.1.4) share*leaving them*' }
        @{ Name = 'older NSIS 22'; Installed = @($nsis22); Clang = 22; Pin = 23; Action = 'Replace'; Remove = @($nsis22); Note = 'LLVM 22.1.8 is registered and C:\Program Files\LLVM\bin\clang.exe is LLVM 22, but the presets build with LLVM 23; uninstalling every LLVM registration (22.1.8), then installing LLVM 23.' }
        @{ Name = 'older MSI 22 + newer MSI 23'; Installed = @($msi22, $msi23c); Clang = 23; Pin = 23; Action = 'Replace'; Remove = @($msi22, $msi23c); Note = 'LLVM 22.1.5 is registered, but the presets build with LLVM 23; uninstalling every LLVM registration (22.1.5, 23.1.4), then*' }
        @{ Name = 'newer major x3 (pin 22)'; Installed = @($msi23a, $msi23b, $msi23c); Clang = 23; Pin = 22; Action = 'Replace'; Remove = @($msi23a, $msi23b, $msi23c); Note = 'LLVM 23.1.2, 23.1.3, 23.1.4 is registered and*clang.exe is LLVM 23, but the presets build with LLVM 22*' }
        @{ Name = 'pinned major registered, clang.exe missing'; Installed = @($msi23c); Clang = $null; Pin = 23; Action = 'Replace'; Remove = @($msi23c); Note = "there's no clang.exe in C:\Program Files\LLVM\bin, but*" }
        @{ Name = 'registration without a version'; Installed = @((New-Registration -Version '')); Clang = 23; Pin = 23; Action = 'Replace'; Remove = 1; Note = 'LLVM (no version) is registered, but*' }
    )) {
    $plan = Get-LlvmInstallPlan -Installed $case.Installed -PinnedMajor $case.Pin -ClangMajor $case.Clang
    Assert-True ($plan.Action -eq $case.Action) "$($case.Name): action $($plan.Action), expected $($case.Action)"
    $removed = @($plan.Remove)
    if ($case.Remove -is [int]) {
        Assert-True ($removed.Count -eq $case.Remove) "$($case.Name): removes $($removed.Count)"
    }
    else {
        Assert-True ($removed.Count -eq $case.Remove.Count) "$($case.Name): removes [$(($removed | ForEach-Object Version) -join ', ')]"
        for ($i = 0; $i -lt $removed.Count; $i++) {
            Assert-True ([object]::ReferenceEquals($removed[$i], $case.Remove[$i])) "$($case.Name): removes [$(($removed | ForEach-Object Version) -join ', ')]"
        }
    }
    if ($null -eq $case.Note) {
        Assert-True ($null -eq $plan.Note) "$($case.Name): note [$($plan.Note)]"
    }
    else {
        Assert-True ($plan.Note -like $case.Note) "$($case.Name): note [$($plan.Note)]"
    }
}

# The uninstall commands: msiexec for an MSI, the QuietUninstallString as written, else the NSIS
# uninstaller with /S, quoted or not; none is an error.
foreach ($case in @(
        @{ Registration = $msi22; FilePath = 'msiexec.exe'; Arguments = '/x {22222222-2222-2222-2222-222222222222} /passive /norestart' }
        @{ Registration = $nsis22; FilePath = 'C:\Program Files\LLVM\Uninstall.exe'; Arguments = '/S' }
        @{ Registration = (New-Registration -Version '22.1.8' -UninstallString 'C:\Program Files\LLVM\Uninstall.exe'); FilePath = 'C:\Program Files\LLVM\Uninstall.exe'; Arguments = '/S' }
        @{ Registration = (New-Registration -Version '22.1.8' -QuietUninstallString '"C:\Program Files\LLVM\Uninstall.exe" /S /quiet'); FilePath = 'C:\Program Files\LLVM\Uninstall.exe'; Arguments = '/S /quiet' }
    )) {
    $command = Get-LlvmUninstallCommand -Registration $case.Registration
    Assert-True ($command.FilePath -eq $case.FilePath -and $command.ArgumentList -eq $case.Arguments) "uninstall command for $($case.Registration.Version): [$($command.FilePath)] [$($command.ArgumentList)]"
}
$noCommand = New-Registration -Version '22.1.8'
$noCommand.UninstallString = ''
$thrown = $null
try { $null = Get-LlvmUninstallCommand -Registration $noCommand } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like '*no usable uninstall command*Settings > Apps*') "no uninstall command: [$thrown]"

# Install-PinnedLlvm against stubs: the registrations and the clang major it reads, Start-Process
# (the uninstallers) and winget, all driven by $LlvmState. A winget install of LLVM.LLVM that
# winget doesn't fail moves the stub clang to $LlvmState.ClangAfterInstall. The real registry
# reader is kept for its own test below.
$realGetInstalledLlvm = ${function:Get-InstalledLlvm}
$LlvmState = @{ Installed = @(); ClangMajor = $null; ClangAfterInstall = 23; StartExitCode = 0; InstallExitCode = 0 }
function Get-InstalledLlvm { $LlvmState.Installed }
function Get-LlvmClangMajor { $LlvmState.ClangMajor }
function Start-Process {
    param([string]$FilePath, [string]$ArgumentList, [switch]$Wait, [switch]$PassThru)
    $script:Calls.Add("start $FilePath $ArgumentList".Trim())
    if ($LlvmState.StartExitCode -is [string]) { throw $LlvmState.StartExitCode }
    [pscustomobject]@{ ExitCode = $LlvmState.StartExitCode }
}
function Set-LlvmStub {
    param([object[]]$Installed, [Nullable[int]]$Clang, [Nullable[int]]$ClangAfterInstall = 23, [object]$StartExitCode = 0, [int]$InstallExitCode = 0)
    $LlvmState.Installed = @($Installed)
    $LlvmState.ClangMajor = $Clang
    $LlvmState.ClangAfterInstall = $ClangAfterInstall
    $LlvmState.StartExitCode = $StartExitCode
    $LlvmState.InstallExitCode = $InstallExitCode
    $state = $LlvmState
    Set-Stub {
        param($Line)
        if ($Line -like 'install --id LLVM.LLVM *') {
            if ($state.InstallExitCode -in 0, 0x8A15010D) { $state.ClangMajor = $state.ClangAfterInstall }
            return @{ ExitCode = $state.InstallExitCode; Output = @() }
        }
        @{ ExitCode = 0; Output = @() }
    }.GetNewClosure()
}
$llvmInstall = "install --id LLVM.LLVM --version 23.1.3 $switches"
$msiexec = { param($Registration) "start msiexec.exe /x $($Registration.ProductCode) /passive /norestart" }

foreach ($case in @(
        @{ Name = 'none installed'; Installed = @(); Clang = $null; Calls = @($llvmInstall) }
        @{ Name = 'same major'; Installed = @($msi23c); Clang = 23; Calls = @() }
        @{ Name = 'same major x3'; Installed = @($msi23a, $msi23b, $msi23c); Clang = 23; Calls = @() }
        @{ Name = 'older NSIS 22'; Installed = @($nsis22); Clang = 22; Calls = @('start C:\Program Files\LLVM\Uninstall.exe /S', $llvmInstall) }
        @{ Name = 'older MSI 22 + newer MSI 23'; Installed = @($msi22, $msi23c); Clang = 23; Calls = @((& $msiexec $msi22), (& $msiexec $msi23c), $llvmInstall) }
    )) {
    Set-LlvmStub -Installed $case.Installed -Clang $case.Clang
    $null = Install-PinnedLlvm -WarningVariable warnings -InformationVariable info
    Assert-True (($calls -join '; ') -eq ($case.Calls -join '; ')) "$($case.Name): calls [$($calls -join '; ')], expected [$($case.Calls -join '; ')]"
    Assert-True (@($warnings).Count -eq 0) "$($case.Name): warned [$warnings]"
    $text = ($info | ForEach-Object { "$_" }) -join "`n"
    if ($case.Calls.Count -eq 0) {
        Assert-True ($text -like '*LLVM 23 is already installed; leaving it.*') "$($case.Name): lines [$text]"
    }
    else {
        Assert-True ($text -like '*C:\Program Files\LLVM\bin\clang.exe is LLVM 23.*') "$($case.Name): lines [$text]"
    }
    if ($case.Name -eq 'same major x3') {
        Assert-True ($text -like '*3 installer registrations of LLVM 23 (23.1.2, 23.1.3, 23.1.4)*') "$($case.Name): no note in [$text]"
    }
}

# Every way the step can fail stops the script with a message that says what failed; nothing after
# a failed uninstall runs.
foreach ($case in @(
        @{ Name = 'uninstall fails'; Installed = @($nsis22); Clang = 22; StartExitCode = 2; Message = 'Uninstalling LLVM 22.1.8 failed with exit code 2*Settings > Apps*'; Calls = 1 }
        @{ Name = 'msiexec fails'; Installed = @($msi22, $msi23c); Clang = 23; StartExitCode = 1603; Message = 'Uninstalling LLVM 22.1.5 failed with exit code 1603*'; Calls = 1 }
        @{ Name = 'administrator approval declined'; Installed = @($msi22); Clang = 22; StartExitCode = 'The operation was canceled by the user.'; Message = "Uninstalling LLVM 22.1.5 didn't start (The operation was canceled by the user.)*"; Calls = 1 }
        @{ Name = 'install fails'; Installed = @($nsis22); Clang = 22; InstallExitCode = 0x8A150011; Message = 'Installing LLVM 23.1.3 failed: winget failed with exit code 0x8A150011*'; Calls = 2 }
        @{ Name = 'clang still the old major'; Installed = @($nsis22); Clang = 22; ClangAfterInstall = 22; Message = 'After installing LLVM 23.1.3, C:\Program Files\LLVM\bin\clang.exe --version reports LLVM 22, not LLVM 23*'; Calls = 2 }
        @{ Name = 'clang missing after the install'; Installed = @(); Clang = $null; ClangAfterInstall = $null; Message = '*reports no LLVM version (missing, or it does not run), not LLVM 23*'; Calls = 1 }
        @{ Name = 'winget: another version installed'; Installed = @(); Clang = 22; ClangAfterInstall = 22; InstallExitCode = 0x8A15010D; Message = 'After installing LLVM 23.1.3*reports LLVM 22, not LLVM 23*'; Calls = 1 }
    )) {
    $stub = @{ Installed = $case.Installed; Clang = $case.Clang }
    foreach ($key in 'ClangAfterInstall', 'StartExitCode', 'InstallExitCode') {
        if ($case.ContainsKey($key)) { $stub[$key] = $case[$key] }
    }
    Set-LlvmStub @stub
    $thrown = $null
    try { $null = Install-PinnedLlvm -WarningAction SilentlyContinue -InformationAction SilentlyContinue } catch { $thrown = $_.Exception.Message }
    Assert-True ($thrown -like $case.Message) "$($case.Name): [$thrown]"
    Assert-True ($calls.Count -eq $case.Calls) "$($case.Name): calls [$($calls -join '; ')]"
}

# A registration still there after its uninstaller exits stops the script once the wait runs out.
Set-LlvmStub -Installed @() -Clang $null
$stillThere = New-Registration -Version '22.1.8'
$stillThere.Key = 'HKCU:\Software'
$thrown = $null
try { Invoke-LlvmUninstall -Registration $stillThere -TimeoutSeconds 0 -InformationAction SilentlyContinue } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like 'LLVM 22.1.8 is still registered 0 s after its uninstaller exited*') "still registered: [$thrown]"

# msiexec's "restart to finish" codes count as done, with a warning.
Set-LlvmStub -Installed @($msi22) -Clang 22 -StartExitCode 3010
$null = Install-PinnedLlvm -WarningVariable warnings -InformationAction SilentlyContinue
Assert-True (@($warnings).Count -eq 1 -and $warnings[0] -like '*restart to finish uninstalling LLVM 22.1.5*') "msiexec 3010: warnings [$warnings]"
Assert-True ($calls.Count -eq 2 -and $calls[1] -eq $llvmInstall) "msiexec 3010: calls [$($calls -join '; ')]"

# -DryRun prints each uninstall, the install and the check, and runs none of them.
$DryRun = $true
Set-LlvmStub -Installed @($msi22, $msi23c) -Clang 23
Set-Stub { param($Line) throw "winget ran under -DryRun: $Line" }
$null = Install-PinnedLlvm -InformationVariable info
$DryRun = $false
Assert-True ($calls.Count -eq 0) "LLVM dry-run: calls [$($calls -join '; ')]"
$lines = @($info | ForEach-Object { "$_" } | Where-Object { $_ -like '`[dry-run`]*' })
$expected = @(
    "[dry-run] msiexec.exe /x $($msi22.ProductCode) /passive /norestart"
    "[dry-run] msiexec.exe /x $($msi23c.ProductCode) /passive /norestart"
    "[dry-run] winget $llvmInstall"
    '[dry-run]   (if 23.1.3 is no longer in the winget catalog: a warning, then the same install without --version)'
    '[dry-run] C:\Program Files\LLVM\bin\clang.exe --version (must report LLVM 23)'
)
Assert-True (($lines -join "`n") -eq ($expected -join "`n")) "LLVM dry-run: lines [$($lines -join "`n")]"

# The registry reader, against a scratch uninstall root under HKCU: an NSIS LLVM, an MSI LLVM, one
# installed elsewhere and an unrelated LLVM-named package; only the first two are the LLVM in $LlvmDir.
$scratch = "HKCU:\Software\TaskSmackTests-setup-dev-$([guid]::NewGuid().ToString('N'))"
try {
    $entries = @(
        @{ Key = 'LLVM'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '22.1.8'; UninstallString = '"C:\Program Files\LLVM\Uninstall.exe"' } }
        @{ Key = '{C261B515-7573-457D-AA83-FE1826A77ECE}'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '23.1.4'; WindowsInstaller = 1; InstallLocation = 'C:\Program Files\LLVM\'; UninstallString = 'MsiExec.exe /X{C261B515-7573-457D-AA83-FE1826A77ECE}' } }
        @{ Key = 'LLVM32'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '21.1.0'; InstallLocation = 'C:\Program Files (x86)\LLVM'; UninstallString = '"C:\Program Files (x86)\LLVM\Uninstall.exe"' } }
        @{ Key = 'OpenMP'; Values = @{ DisplayName = 'LLVM OpenMP Runtime'; DisplayVersion = '19.0.0' } }
    )
    foreach ($entry in $entries) {
        $path = Join-Path $scratch $entry.Key
        $null = New-Item -Path $path -Force
        foreach ($name in $entry.Values.Keys) {
            $type = if ($entry.Values[$name] -is [int]) { 'DWord' } else { 'String' }
            $null = New-ItemProperty -LiteralPath $path -Name $name -Value $entry.Values[$name] -PropertyType $type
        }
    }
    $LlvmUninstallKeys = @($scratch)
    $found = @(& $realGetInstalledLlvm | Sort-Object Version)
    Assert-True ($found.Count -eq 2) "registry: found [$(($found | ForEach-Object Version) -join ', ')]"
    Assert-True ($found[0].Version -eq '22.1.8' -and $found[0].Major -eq 22 -and $null -eq $found[0].ProductCode -and $found[0].UninstallString -eq '"C:\Program Files\LLVM\Uninstall.exe"') "registry: NSIS entry [$($found[0] | Out-String)]"
    Assert-True ($found[1].Version -eq '23.1.4' -and $found[1].Major -eq 23 -and $found[1].ProductCode -eq '{C261B515-7573-457D-AA83-FE1826A77ECE}') "registry: MSI entry [$($found[1] | Out-String)]"
    Assert-True (Test-Path -LiteralPath $found[1].Key) "registry: key [$($found[1].Key)]"
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

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
            "[dry-run] winget install --id Python.Python.3.14 $switches"
        )) {
        Assert-True ($output.Contains($expected)) "-DryRun output lacks [$expected]:`n$output"
    }
    # The LLVM step depends on this machine's LLVM: it installs the pin, or leaves the pinned major.
    Assert-True ($output.Contains('[dry-run] winget install --id LLVM.LLVM --version ') -or $output -match 'LLVM \d+ is already installed; leaving it\.') "-DryRun output has no LLVM install or keep line:`n$output"

    Write-Host 'setup-dev.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
