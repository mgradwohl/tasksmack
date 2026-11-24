# Tests for tools/sandbox-install-check.ps1 (#4854): its argument checks, the .wsb it writes and the check script
# that runs inside the Sandbox. No Sandbox starts and nothing is downloaded (-WriteOnly). Registered in CTest on
# Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'sandbox-install-check.ps1'
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "sandbox-install-check.ps1 does not parse: $errors"

. $script

# The .wsb: valid XML, the mapped folder read-write at C:\ws, the LogonCommand, and networking by mode.
$wsb = New-SandboxConfig -HostFolder 'C:\Temp\check one' -ScriptName 'inside.ps1'
$xml = [xml]$wsb
Assert-True ($xml.Configuration.Networking -eq 'Disable') 'networking is off without winget'
Assert-True ($xml.Configuration.MappedFolders.MappedFolder.HostFolder -eq 'C:\Temp\check one') 'host folder'
Assert-True ($xml.Configuration.MappedFolders.MappedFolder.SandboxFolder -eq 'C:\ws') 'sandbox folder'
Assert-True ($xml.Configuration.MappedFolders.MappedFolder.ReadOnly -eq 'false') 'mapped folder is read-write'
Assert-True ($xml.Configuration.LogonCommand.Command -eq 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ws\inside.ps1') 'logon command'
Assert-True (([xml](New-SandboxConfig -HostFolder 'C:\x' -ScriptName 'a.ps1' -Network)).Configuration.Networking -eq 'Enable') 'networking on with -Network'

# The in-Sandbox script: parses, is ASCII (Windows PowerShell 5.1 reads it as the ANSI code page), fills every
# token, and carries the checks.
$direct = New-CheckScript -InstallerName 'TaskSmack-0.11.0-win64.exe' -ExpectedVersion '0.11.0' -OutDirExpression '(Split-Path -Parent $MyInvocation.MyCommand.Path)'
$viaWinget = New-CheckScript -InstallerName 'winget manifest 0.11.0' -ExpectedVersion '0.11.0' -ViaWinGet -OutDirExpression "(Join-Path `$env:USERPROFILE 'Desktop\x')"
foreach ($text in $direct, $viaWinget) {
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$null, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) "the generated check script does not parse: $parseErrors"
    Assert-True ($text -notmatch '[^\x00-\x7F]') 'the generated check script must be ASCII-only'
    Assert-True ($text -notmatch '__[A-Z]+__') 'a template token was left unfilled'
    foreach ($needle in 'WOW6432Node', 'TaskSmack Contributors', "expectedVersion = '0.11.0'", 'RESULT: ', "'DONE'", 'results.md', 'Seconds 15', 'AddSeconds(60)') {
        Assert-True ($text.Contains($needle)) "the generated check script lacks '$needle'"
    }
}
Assert-True ($direct.Contains("-ArgumentList '/S' -Wait -PassThru")) 'direct mode installs silently'
Assert-True ($direct.Contains('Uninstall.exe') -and $direct.Contains('87')) 'direct mode runs the uninstaller and names exit 87'
Assert-True (-not $direct.Contains('winget uninstall')) 'direct mode does not use winget'
Assert-True ($viaWinget.Contains('uninstall --name TaskSmack --exact --silent') -and -not $viaWinget.Contains("-ArgumentList '/S'")) 'winget mode uninstalls through winget'

# Argument checks, run as a real process: each is refused before anything is touched.
function Invoke-Check {
    param([string[]]$Arguments)
    $output = & pwsh -NoProfile -File $script @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}
$fixtures = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-sandbox-test-$PID"
New-Item -ItemType Directory -Force $fixtures | Out-Null
try {
    $installer = Join-Path $fixtures 'TaskSmack-0.11.0-win64.exe'
    Set-Content -LiteralPath $installer -Value 'not an installer'
    $oddName = Join-Path $fixtures 'setup.exe'
    Set-Content -LiteralPath $oddName -Value 'x'
    foreach ($case in @(
            @{ Args = @(); Expect = 'Give -Version' }
            @{ Args = @('-Version', '0.11.0', '-Installer', $installer); Expect = 'not both' }
            @{ Args = @('-Version', 'v0.11.0'); Expect = 'X\.Y\.Z' }
            @{ Args = @('-Installer', $installer, '-WinGet'); Expect = '-WinGet needs -Version' }
            @{ Args = @('-Installer', (Join-Path $fixtures 'missing.exe')); Expect = 'does not exist' }
            @{ Args = @('-Installer', $oddName); Expect = 'TaskSmack-X\.Y\.Z-win64\.exe' }
            @{ Args = @('-Installer', $installer, '-WorkDir', 'C:\a&b'); Expect = 'WorkDir' }
        )) {
        $result = Invoke-Check $case.Args
        Assert-True ($result.ExitCode -ne 0) "sandbox-install-check.ps1 $($case.Args -join ' ') should fail"
        Assert-True ($result.Output -match $case.Expect) "sandbox-install-check.ps1 $($case.Args -join ' '): expected /$($case.Expect)/ in: $($result.Output)"
    }

    # -WriteOnly with a local installer writes the folder and starts nothing.
    $work = Join-Path $fixtures 'work'
    $result = Invoke-Check @('-Installer', $installer, '-WorkDir', $work, '-WriteOnly')
    Assert-True ($result.ExitCode -eq 0) "-WriteOnly failed: $($result.Output)"
    foreach ($name in 'TaskSmack-0.11.0-win64.exe', 'sandbox-install-check-inside.ps1', 'check.wsb') {
        Assert-True (Test-Path -LiteralPath (Join-Path $work $name)) "-WriteOnly did not write $name"
    }
    $written = Get-Content -LiteralPath (Join-Path $work 'sandbox-install-check-inside.ps1') -Raw
    Assert-True ($written.Contains("installerName = 'TaskSmack-0.11.0-win64.exe'") -and $written.Contains("expectedVersion = '0.11.0'")) 'the written script names the installer and version'
    Assert-True (([xml](Get-Content -LiteralPath (Join-Path $work 'check.wsb') -Raw)).Configuration.MappedFolders.MappedFolder.HostFolder -eq (Resolve-Path $work).Path) 'the written .wsb maps the work folder'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $work 'DONE'))) '-WriteOnly must not produce a result'
}
finally {
    Remove-Item -LiteralPath $fixtures -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'sandbox-install-check tests passed'
