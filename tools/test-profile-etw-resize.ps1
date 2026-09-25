# No ETW sessions, UAC prompts, or application windows: native/launcher calls are mocked below.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'profile-etw-resize.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Assert-Throws {
    param([scriptblock]$Action, [string]$Pattern)
    $caught = $false
    try { & $Action }
    catch {
        $caught = $true
        Assert-True ($_.ToString() -like "*$Pattern*") "Unexpected exception: $_"
    }
    Assert-True $caught "Expected failure: $Pattern"
}

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-resize-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # Real command wrapper: nonzero native exits retain stdout/stderr and exit metadata.
    $hostExe = (Get-Process -Id $PID).Path
    $nativeLog = Join-Path $root 'native.log'
    $PSNativeCommandUseErrorActionPreference = $true
    Assert-Throws { Invoke-ResizeCaptureCommand $hostExe @('-NoProfile', '-Command', '[Console]::Error.WriteLine("decoder warning"); exit 7') $nativeLog } 'exited 7'
    Assert-True ((Get-Content -LiteralPath $nativeLog -Raw).Contains('decoder warning')) 'stderr was lost'
    Assert-True ((Get-Content -LiteralPath "$nativeLog.json" -Raw | ConvertFrom-Json).ExitCode -eq 7) 'exit code was lost'

    $collector = @{ State = 'saved'; StartUtc = '2026-09-09T03:00:00Z'; StopRequestedUtc = '2026-09-09T03:03:00Z' }
    $app = @{ State = 'exited'; ExitCode = 0; StartUtc = '2026-09-09T03:00:01Z'; EndUtc = '2026-09-09T03:02:00Z' }
    Assert-True (Test-ResizeCaptureOverlap $collector $app) 'Matching run rejected'
    $app.EndUtc = '2026-09-09T03:04:00Z'
    Assert-True (-not (Test-ResizeCaptureOverlap $collector $app)) 'Non-overlap accepted'
    $app.EndUtc = '2026-09-09T03:02:00Z'
    $app.ExitCode = 7
    Assert-True (-not (Test-ResizeCaptureOverlap $collector $app)) 'Failed app accepted'

    $script:elevated = $false
    function Get-ResizeCaptureIdentity { @{ Elevated = $script:elevated; LauncherPid = $PID } }
    $script:calls = [Collections.Generic.List[object]]::new()
    $script:failOn = ''
    function Invoke-ResizeCaptureCommand {
        param([string]$Exe, [string[]]$Arguments, [string]$Log)
        $script:calls.Add(@{ Exe = $Exe; Arguments = $Arguments })
        if ($script:failOn -and $Arguments -contains $script:failOn) { throw "simulated $script:failOn failure" }
        'mock diagnostic output' | Set-Content -LiteralPath $Log
    }
    function Get-Command { param($Name, $ErrorAction) @{ Source = "mock-$Name.exe" } }
    function Start-Sleep { param($Seconds) }
    function New-TestRun {
        $run = Join-Path $root ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $run | Out-Null
        Write-ResizeCaptureJson (Join-Path $run 'run.json') @{ Files = @(); SymbolPath = 'binary' }
        return $run
    }

    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run } 'requires a separately'
    $script:elevated = $true
    Assert-Throws { Invoke-ResizeCapture -Phase App -RunDirectory $run } 'normal-user'
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 601 } '15-600'

    Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15
    $saved = Get-Content -LiteralPath (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json
    Assert-True ($saved.State -eq 'saved') 'Collector did not save'
    $start = @($script:calls | Where-Object { $_.Arguments -contains '-start' })
    $stop = @($script:calls | Where-Object { $_.Arguments -contains '-stop' })
    Assert-True ($start.Count -eq 1 -and $stop.Count -eq 1) 'Wrong start/stop count'
    Assert-True ($start[0].Arguments[-1] -eq $saved.Instance -and $stop[0].Arguments[-1] -eq $saved.Instance) 'Instance ownership lost'
    Assert-True ($start[0].Arguments[-2] -eq '-instancename') 'Instance option must be last'
    foreach ($profile in @('GeneralProfile.Verbose', 'GPU.Verbose', 'DesktopComposition.Verbose')) {
        Assert-True ($start[0].Arguments -contains $profile) "Missing $profile"
    }
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 } 'already exists'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-cancel' }).Count -eq 0) 'Unexpected cancel'

    # A failed start must not stop any recording; a failed status must still stop ours.
    $script:calls.Clear()
    $script:failOn = '-start'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 } 'simulated'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-stop' }).Count -eq 0) 'Stopped after failed start'
    Assert-True ((Get-Content (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json).State -eq 'failed') 'Start failure not preserved'
    $script:calls.Clear()
    $script:failOn = '-status'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 } 'simulated'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-stop' }).Count -eq 1) 'Status failure leaked recording'
    $script:failOn = '-stop'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 } 'simulated'
    Assert-True ((Get-Content (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json).State -eq 'stop-failed') 'Stop failure not preserved'

    # Hash mismatch must stop launch before creating a process.
    $script:elevated = $false
    $script:failOn = ''
    $file = Join-Path $run 'sample.exe'
    'original' | Set-Content -LiteralPath $file
    $manifest = @{ Files = @(@{ RelativePath = 'sample.exe'; Sha256 = (Get-FileHash $file).Hash }) }
    Assert-ResizeCaptureFiles $run $manifest
    'changed' | Set-Content -LiteralPath $file
    Assert-Throws { Assert-ResizeCaptureFiles $run $manifest } 'artifact changed'

    # App launcher error must restore environment, preserve error and never claim completion.
    $run = New-TestRun
    Write-ResizeCaptureJson (Join-Path $run 'collector.json') @{
        State = 'recording'; DeadlineUtc = [DateTimeOffset]::UtcNow.AddSeconds(90).ToString('o')
    }
    function Start-Process { throw 'simulated launch failure' }
    $env:TASKSMACK_TRACE_RESIZE_PERF = 'prior-trace'
    $env:TASKSMACK_LOG_LEVEL = 'prior-level'
    Assert-Throws { Invoke-ResizeCapture -Phase App -RunDirectory $run } 'simulated launch failure'
    Assert-True ($env:TASKSMACK_TRACE_RESIZE_PERF -eq 'prior-trace' -and $env:TASKSMACK_LOG_LEVEL -eq 'prior-level') 'Environment not restored'
    Assert-True ((Get-Content (Join-Path $run 'app.json') -Raw | ConvertFrom-Json).State -eq 'failed') 'App failure not preserved'

    # Successful app launch and Check: verify separate streams, inherited trace flag,
    # no elevation verb, conservative overlap, anchors, and retained decoder invocation.
    function Start-Process {
        param($FilePath, $WorkingDirectory, $RedirectStandardOutput, $RedirectStandardError, [switch]$PassThru)
        Assert-True ($env:TASKSMACK_TRACE_RESIZE_PERF -eq '1') 'Trace not enabled'
        Assert-True ($env:TASKSMACK_LOG_LEVEL -eq 'info') 'Trace logs suppressed'
        @(
            'ResizePerfAnchor: pid=42 uiTid=7 clock=QPC frequency=10000000'
            'ResizePerfWallSummary: loops=10'
            'ResizePerfAnchor: pid=42 uiTid=7 clock=QPC frequency=10000000'
        ) | Set-Content -LiteralPath $RedirectStandardOutput
        'retained stderr' | Set-Content -LiteralPath $RedirectStandardError
        $process = [pscustomobject]@{ Id = 42; ExitCode = 0 }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($Timeout) return $true }
        return $process
    }
    $run = New-TestRun
    $startUtc = [DateTimeOffset]::UtcNow.AddSeconds(-1).ToString('o')
    Write-ResizeCaptureJson (Join-Path $run 'collector.json') @{
        State = 'recording'; StartUtc = $startUtc; DeadlineUtc = [DateTimeOffset]::UtcNow.AddSeconds(90).ToString('o')
    }
    Invoke-ResizeCapture -Phase App -RunDirectory $run
    Assert-True ((Get-Content (Join-Path $run 'app.json') -Raw | ConvertFrom-Json).Pid -eq 42) 'App PID lost'
    Assert-True ((Get-Content (Join-Path $run 'stderr.log') -Raw).Contains('retained stderr')) 'Separate stderr lost'
    Write-ResizeCaptureJson (Join-Path $run 'collector.json') @{
        State = 'saved'; StartUtc = $startUtc; StopRequestedUtc = [DateTimeOffset]::UtcNow.AddSeconds(1).ToString('o')
    }
    'mock ETL' | Set-Content -LiteralPath (Join-Path $run 'trace.etl')
    Invoke-ResizeCapture -Phase Check -RunDirectory $run
    Assert-True ((Get-Content (Join-Path $run 'check.json') -Raw | ConvertFrom-Json).SameRun) 'Check did not persist'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-tle' }).Count -eq 0) 'Trace loss suppression requested'
    $script:failOn = 'tracestats'
    Assert-Throws { Invoke-ResizeCapture -Phase Check -RunDirectory $run } 'simulated'
    Assert-True (-not (Get-Content (Join-Path $run 'check.json') -Raw | ConvertFrom-Json).SameRun) 'Stale successful Check survived decoder failure'
    $script:failOn = ''
    'missing anchors' | Set-Content -LiteralPath (Join-Path $run 'stdout.log')
    Assert-Throws { Invoke-ResizeCapture -Phase Check -RunDirectory $run } 'expected startup/shutdown'

    function Start-Process {
        $process = [pscustomobject]@{ Id = 42; ExitCode = $null }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($Timeout) return $false }
        $process | Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value { $script:closeRequested = $true; return $true }
        return $process
    }
    $script:closeRequested = $false
    $run = New-TestRun
    Write-ResizeCaptureJson (Join-Path $run 'collector.json') @{
        State = 'recording'; DeadlineUtc = [DateTimeOffset]::UtcNow.AddSeconds(90).ToString('o')
    }
    Assert-Throws { Invoke-ResizeCapture -Phase App -RunDirectory $run } 'Left running'
    Assert-True $script:closeRequested 'Deadline did not request graceful close'
    Assert-True ((Get-Content (Join-Path $run 'app.json') -Raw | ConvertFrom-Json).State -eq 'failed') 'Hung app treated as completed'
    Write-Host 'Resize capture lifecycle, overlap, privilege, identity and failure tests passed.'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
