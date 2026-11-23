# Tests for tools/newcomer-check.ps1's Smart App Control read and stall rule (#4781). The functions are taken
# from the script with the PowerShell parser (the script itself walks the whole newcomer path when run):
#   - Get-SmartAppControlState, against a scratch key under HKCU that the test deletes;
#   - Test-StepStalled, on synthetic samples: a step whose CPU doesn't move is stalled after the window, and
#     CPU, I/O, a new process or a growing log each count as progress;
#   - Get-ErrorDialog, on fake process lists (WerFault, a "Bad Image" window);
#   - Get-BusiestProcess and the heartbeat (#4824): a process whose CPU read throws is skipped, and nothing the
#     heartbeat does can fail the step;
#   - Invoke-Shell with -StallMinutes, on real short shells: a sleeping shell is killed and fails as
#     "stalled", a spinning one that finishes is not, and without -StallMinutes a sleeper just finishes.
# Nothing is installed. Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'newcomer-check.ps1'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "newcomer-check.ps1 does not parse: $errors"
foreach ($name in 'Get-SmartAppControlState', 'Get-ProcessTreeSample', 'Test-StepStalled', 'Get-ErrorDialog',
    'Write-OutFile', 'Write-CheckLog', 'Save-Diagnostic', 'Get-LogTail', 'Invoke-Shell', 'Get-BusiestProcess') {
    $definition = $ast.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }.GetNewClosure(), $true)
    Assert-True ($null -ne $definition) "$name not found in newcomer-check.ps1"
    . ([scriptblock]::Create($definition.Extent.Text))
}

# ── Smart App Control state: the same three states as check-prereqs.ps1, from a scratch key ───────
$sacKey = "HKCU:\Software\TaskSmackTests-newcomer-check-$([guid]::NewGuid().ToString('N'))"
try {
    foreach ($case in @(@{ Value = 1; State = 1 }, @{ Value = 2; State = 2 }, @{ Value = 0; State = 0 }, @{ Value = 9; State = 0 }, @{ Value = $null; State = 0 })) {
        $null = New-Item -Path $sacKey -Force
        if ($null -ne $case.Value) {
            $null = New-ItemProperty -LiteralPath $sacKey -Name VerifiedAndReputablePolicyState -Value $case.Value -PropertyType DWord
        }
        $state = Get-SmartAppControlState -KeyPath $sacKey
        Assert-True ($state -eq $case.State) "value [$($case.Value)] read as [$state], expected $($case.State)"
        Remove-Item -LiteralPath $sacKey -Recurse -Force
    }
    Assert-True ((Get-SmartAppControlState -KeyPath $sacKey) -eq 0) 'a missing key must read as off'
}
finally {
    Remove-Item -LiteralPath $sacKey -Recurse -Force -ErrorAction SilentlyContinue
}

# The script logs the state as a note and records a FINDING naming #4781 when it enforces.
$scriptText = Get-Content -LiteralPath $script -Raw
Assert-True ($scriptText -match "(?s)Add-Note \('Smart App Control: '") 'newcomer-check.ps1 must note the Smart App Control state in STEP 1'
Assert-True ($scriptText -match "(?s)if \(\`$smartAppControl -eq 1\) \{\s*Add-Finding 'Smart App Control enforces[^']*#4781") 'newcomer-check.ps1 must record a FINDING naming #4781 when Smart App Control enforces'
Assert-True ($scriptText -match '\[double\]\$StallMinutes = 15') 'the stall window parameter must default to 15 minutes'
Assert-True ($scriptText -match '(?s)\.PARAMETER StallMinutes') 'the stall window parameter must be documented in the help'

# ── Test-StepStalled, on synthetic samples ────────────────────────────────────────────────────────
$t0 = [datetime]'2026-11-22 10:00:00'
function New-Sample {
    param([double]$Minute, [double]$Cpu = 13, [double]$Io = 5e6, [string]$Pids = '10,11', [double]$Log = 100)
    [pscustomobject]@{ Time = $t0.AddMinutes($Minute); Cpu = $Cpu; IoBytes = $Io; Pids = $Pids; LogBytes = $Log }
}
# Five-minute heartbeats, as the script takes them.
$frozen = @(0, 5, 10, 15 | ForEach-Object { New-Sample -Minute $_ })
Assert-True ($null -ne (Test-StepStalled -Samples $frozen -WindowMinutes 15)) 'a frozen tree for the whole window must be stalled'
Assert-True ($null -eq (Test-StepStalled -Samples $frozen[0..2] -WindowMinutes 15)) 'a frozen tree for 10 of 15 minutes is not stalled yet'
Assert-True ($null -eq (Test-StepStalled -Samples $frozen -WindowMinutes 0)) 'a window of 0 turns the rule off'
Assert-True ($null -eq (Test-StepStalled -Samples @() -WindowMinutes 15)) 'no samples is not stalled'
Assert-True ($null -ne (Test-StepStalled -Samples $frozen -WindowMinutes 10)) 'the window is configurable: 10 minutes is enough at 15'
# The issue's own numbers: 13 -> 15 CPU seconds in 15 minutes is under 0.2 CPU s per minute.
$barely = @(New-Sample -Minute 0 -Cpu 13), (New-Sample -Minute 5 -Cpu 13.5), (New-Sample -Minute 10 -Cpu 14.2), (New-Sample -Minute 15 -Cpu 15)
$reason = Test-StepStalled -Samples $barely -WindowMinutes 15
Assert-True ($reason -like 'no progress in 15.0 min: 2.0 CPU s*') "installer at 13 -> 15 CPU s: [$reason]"
# Anything that moves is progress.
$busy = @(New-Sample -Minute 0 -Cpu 13), (New-Sample -Minute 5 -Cpu 20), (New-Sample -Minute 10 -Cpu 25), (New-Sample -Minute 15 -Cpu 31)
Assert-True ($null -eq (Test-StepStalled -Samples $busy -WindowMinutes 15)) 'a tree using CPU is not stalled'
$copying = @(New-Sample -Minute 0), (New-Sample -Minute 5 -Io 9e6), (New-Sample -Minute 10 -Io 20e6), (New-Sample -Minute 15 -Io 40e6)
Assert-True ($null -eq (Test-StepStalled -Samples $copying -WindowMinutes 15)) 'a tree moving I/O (an install copying files) is not stalled'
$spawning = @(New-Sample -Minute 0), (New-Sample -Minute 5), (New-Sample -Minute 10 -Pids '10,11,12'), (New-Sample -Minute 15 -Pids '10,11,12')
Assert-True ($null -eq (Test-StepStalled -Samples $spawning -WindowMinutes 15)) 'a new process in the tree is progress'
$logging = @(New-Sample -Minute 0), (New-Sample -Minute 5), (New-Sample -Minute 10), (New-Sample -Minute 15 -Log 4096)
Assert-True ($null -eq (Test-StepStalled -Samples $logging -WindowMinutes 15)) 'a growing installer log is progress'
# Progress that ended before the window began doesn't count: frozen since minute 5, window 15 at minute 25.
$wasBusy = @(New-Sample -Minute 0 -Cpu 1), (New-Sample -Minute 5 -Cpu 30), (New-Sample -Minute 10 -Cpu 30), (New-Sample -Minute 15 -Cpu 30), (New-Sample -Minute 20 -Cpu 30), (New-Sample -Minute 25 -Cpu 30)
Assert-True ($null -ne (Test-StepStalled -Samples $wasBusy -WindowMinutes 15)) 'a tree frozen since minute 5 is stalled at minute 25'

# ── Get-ErrorDialog, on fake process lists ────────────────────────────────────────────────────────
function New-FakeProcess {
    param([string]$Name, [string]$Title = '', [int]$Id = 1, [string]$CommandLine = '')
    [pscustomobject]@{ ProcessName = $Name; MainWindowTitle = $Title; Id = $Id; CommandLine = $CommandLine }
}
# The step's tree is pids 10 and 42 in these cases.
$tree = @(10, 42)
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'explorer' 'Program Manager'), (New-FakeProcess 'git' '' 42)))) 'ordinary windows are not error dialogs'
$dialog = Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'git' 'git.exe - Bad Image' 42))
Assert-True ($dialog -like "git (pid 42) 'git.exe - Bad Image'*") "Bad Image box in the tree: [$dialog]"
Assert-True ($null -ne (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'WerFault' '' 10)))) 'a WerFault in the tree is an error dialog'
Assert-True ($null -ne (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'WerFault' '' 900 'WerFault.exe -u -p 42 -s 1234')))) 'a WerFault whose -p names a tree process is an error dialog'
Assert-True ($null -ne (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'app' 'app.exe - Application Error' 10)))) 'an Application Error window in the tree is an error dialog'
Assert-True ($null -ne (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'app' 'app.exe stopped working' 42)))) 'a stopped working window in the tree is an error dialog'
# Outside the tree: somebody else's crash is not this step's stall (#4803).
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'WerFault' '' 900 'WerFault.exe -u -p 777 -s 1234')))) 'a WerFault for a process outside the tree is ignored'
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'WerFault' '' 901 'WerFault.exe -u -p 7770 -s 1')))) 'a -p prefix of a longer outside pid is ignored'
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'WerFault' '' 902 '')))) 'a WerFault with no command line outside the tree is ignored'
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'app' 'app.exe stopped working' 777)))) 'a stopped working window outside the tree is ignored'
Assert-True ($null -eq (Get-ErrorDialog -TreePid $tree -Process @((New-FakeProcess 'git' 'git.exe - Bad Image' 777)))) 'a Bad Image window outside the tree is ignored'
Assert-True ($null -eq (Get-ErrorDialog -TreePid @() -Process @((New-FakeProcess 'WerFault' '' 7)))) 'an empty tree matches nothing'

# ── The heartbeat's busiest-process list (#4824) ──────────────────────────────────────────────────
function New-CpuProcess {
    param([string]$Name, [double]$Seconds, [switch]$Throws)
    $proc = [pscustomobject]@{ Name = $Name }
    if ($Throws) { Add-Member -InputObject $proc -MemberType ScriptProperty -Name TotalProcessorTime -Value { throw 'The process has exited.' } }
    else { Add-Member -InputObject $proc -MemberType NoteProperty -Name TotalProcessorTime -Value ([TimeSpan]::FromSeconds($Seconds)) }
    $proc
}
$fakes = @((New-CpuProcess 'idle' 0), (New-CpuProcess 'clang++' 0 -Throws), (New-CpuProcess 'cl' 50), (New-CpuProcess 'ninja' 200), (New-CpuProcess 'git' 7))
$busy = @(Get-BusiestProcess -Process $fakes)
Assert-True ($busy.Count -eq 3) "a process whose CPU read throws, and an idle one, are skipped: [$($busy -join ', ')]"
Assert-True ($busy[0] -eq 'ninja 200s' -and $busy[1] -eq 'cl 50s' -and $busy[2] -eq 'git 7s') "busiest first: [$($busy -join ', ')]"
Assert-True (@(Get-BusiestProcess -Process @((New-CpuProcess 'a' 1 -Throws))).Count -eq 0) 'only throwing processes gives an empty list'

# ── Invoke-Shell with -StallMinutes, on real short shells ─────────────────────────────────────────
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-newcomer-check-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $OutDir = Join-Path $root 'out'
    $WorkDir = Join-Path $root 'work'
    $RepoDir = Join-Path $WorkDir 'tasksmack'
    New-Item -ItemType Directory -Path $OutDir, $WorkDir | Out-Null
    $script:HeartbeatMilliseconds = 1000

    # A shell that sleeps: no CPU, no new processes, no log. A 3 s window fails it, well before its 5 min timeout.
    $started = Get-Date
    $thrown = $null
    try { $null = Invoke-Shell -Shell pwsh -LogName 'sleeper' -TimeoutMinutes 5 -StallMinutes 0.05 -Command 'Start-Sleep -Seconds 120' 6>$null }
    catch { $thrown = $_.Exception.Message }
    $seconds = ((Get-Date) - $started).TotalSeconds
    Assert-True ($null -ne $thrown -and $thrown.StartsWith('stalled')) "a sleeping step must fail as stalled: [$thrown]"
    Assert-True ($seconds -lt 60) "the stall took $seconds s, it should be seconds, not the timeout"
    Assert-True ((Get-Content -LiteralPath (Join-Path $OutDir 'progress.log') -Raw) -match 'stalled: no progress') 'the stall must be logged'
    Assert-True (Test-Path -LiteralPath (Join-Path $OutDir 'sleeper-diagnostics')) 'a stalled step must save its diagnostics folder'

    # A shell that spins its CPU for 8 s with the same 3 s window: progress throughout, so it finishes.
    $code = Invoke-Shell -Shell pwsh -LogName 'spinner' -TimeoutMinutes 5 -StallMinutes 0.05 -Command '$end = (Get-Date).AddSeconds(8); while ((Get-Date) -lt $end) { }' 6>$null
    Assert-True ($code -eq 0) "a busy step must not be failed as stalled (exit $code)"

    # Without -StallMinutes a quiet step is left alone (the 3 s sleep outlasts the 1 s heartbeat).
    $code = Invoke-Shell -Shell pwsh -LogName 'quiet' -TimeoutMinutes 5 -Command 'Start-Sleep -Seconds 4' 6>$null
    Assert-True ($code -eq 0) "without -StallMinutes a quiet step finishes (exit $code)"

    # The heartbeat survives a process whose CPU read throws (#4824), and a Save-Diagnostic that throws too: both
    # become a warning in progress.log, and the step still finishes with its own exit code.
    function Get-Process { param([switch]$ErrorAction) $script:fakes }
    $code = Invoke-Shell -Shell pwsh -LogName 'racy' -TimeoutMinutes 5 -Command 'Start-Sleep -Seconds 3' 6>$null
    Assert-True ($code -eq 0) "a heartbeat over a throwing process must not fail the step (exit $code)"
    Assert-True ((Get-Content -LiteralPath (Join-Path $OutDir 'progress.log') -Raw) -match 'busiest: ninja 200s, cl 50s, git 7s') 'the heartbeat must list the busiest processes'
    function Save-Diagnostic { param([string]$Prefix) throw 'diagnostics failed' }
    $code = Invoke-Shell -Shell pwsh -LogName 'diagfail' -TimeoutMinutes 5 -Command 'Start-Sleep -Seconds 3' 6>$null
    Assert-True ($code -eq 0) "a failing heartbeat must not fail the step (exit $code)"
    Assert-True ((Get-Content -LiteralPath (Join-Path $OutDir 'progress.log') -Raw) -match 'heartbeat skipped: diagnostics failed') 'a failed heartbeat must log a warning'
    Remove-Item function:Get-Process, function:Save-Diagnostic

    Write-Host 'newcomer-check.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
