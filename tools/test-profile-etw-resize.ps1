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

    # Run directories inside the worktree must be git-ignored, or Prepare's own output is
    # stamped into the binary as configureSourceState=dirty and reported by checkout-status.txt
    # as a source change. Uses the real repository and real `git check-ignore` (read-only, and
    # deliberately before the mocks below), including a path that does not exist yet -- the
    # guard runs before Prepare creates the directory.
    $repo = Split-Path -Parent $PSScriptRoot
    Assert-Throws { Assert-ResizeRunDirectoryIgnored -RunDirectory (Join-Path $repo 'not-ignored-run') -RepoRoot $repo } 'not git-ignored'
    Assert-Throws { Assert-ResizeRunDirectoryIgnored -RunDirectory (Join-Path $repo 'perf-data\notmatching') -RepoRoot $repo } 'not git-ignored'
    foreach ($allowed in @('perf-data\resize-001', 'perf-data\resize-042', 'build\resize-captures\p01')) {
        Assert-ResizeRunDirectoryIgnored -RunDirectory (Join-Path $repo $allowed) -RepoRoot $repo
    }
    # Outside the worktree there is no provenance to pollute, so no git query and no refusal.
    Assert-ResizeRunDirectoryIgnored -RunDirectory (Join-Path ([IO.Path]::GetTempPath()) 'resize-outside') -RepoRoot $repo

    $script:elevated = $false
    function Get-ResizeCaptureIdentity { @{ Elevated = $script:elevated; LauncherPid = $PID } }
    $script:calls = [Collections.Generic.List[object]]::new()
    $script:failOn = ''
    function Invoke-ResizeCaptureCommand {
        param([string]$Exe, [string[]]$Arguments, [string]$Log)
        $script:calls.Add(@{ Exe = $Exe; Arguments = $Arguments })
        if ($script:failOn -and $Arguments -contains $script:failOn) { throw "simulated $script:failOn failure" }
        'mock diagnostic output' | Set-Content -LiteralPath $Log -Encoding utf8
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
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -RepoRoot $repo } 'requires a separately'
    $script:elevated = $true
    Assert-Throws { Invoke-ResizeCapture -Phase App -RunDirectory $run } 'normal-user'
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 601 -RepoRoot $repo } '15-600'

    Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo
    $saved = Get-Content -LiteralPath (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json
    Assert-True ($saved.State -eq 'saved') 'Collector did not save'
    $start = @($script:calls | Where-Object { $_.Arguments -contains '-start' })
    $stop = @($script:calls | Where-Object { $_.Arguments -contains '-stop' })
    Assert-True ($start.Count -eq 1 -and $stop.Count -eq 1) 'Wrong start/stop count'
    Assert-True ($start[0].Arguments[-1] -eq $saved.Instance -and $stop[0].Arguments[-1] -eq $saved.Instance) 'Instance ownership lost'
    Assert-True ($start[0].Arguments[-2] -eq '-instancename') 'Instance option must be last'
    # Default is now the focused profile (#912), not the three built-in Verbose profiles.
    Assert-True (@($start[0].Arguments | Where-Object { $_ -like '*TaskSmackResize.wprp!TaskSmackResize' }).Count -eq 1) 'Focused profile not started'
    foreach ($heavy in @('GeneralProfile.Verbose', 'GPU.Verbose', 'DesktopComposition.Verbose')) {
        Assert-True (-not ($start[0].Arguments -contains $heavy)) "Focused default still started $heavy"
    }
    Assert-True ($saved.ProviderSet -eq 'focused') 'Provider set not recorded'
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo } 'already exists'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-cancel' }).Count -eq 0) 'Unexpected cancel'

    # A failed start must not stop any recording; a failed status must still stop ours.
    $script:calls.Clear()
    $script:failOn = '-start'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo } 'simulated'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-stop' }).Count -eq 0) 'Stopped after failed start'
    Assert-True ((Get-Content (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json).State -eq 'failed') 'Start failure not preserved'
    $script:calls.Clear()
    $script:failOn = '-status'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo } 'simulated'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-stop' }).Count -eq 1) 'Status failure leaked recording'
    $script:failOn = '-stop'
    $run = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo } 'simulated'
    Assert-True ((Get-Content (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json).State -eq 'stop-failed') 'Stop failure not preserved'

    # Hash mismatch must stop launch before creating a process.
    $script:elevated = $false
    $script:failOn = ''
    $file = Join-Path $run 'sample.exe'
    'original' | Set-Content -LiteralPath $file -Encoding utf8
    $manifest = @{ Files = @(@{ RelativePath = 'sample.exe'; Sha256 = (Get-FileHash $file).Hash }) }
    Assert-ResizeCaptureFiles $run $manifest
    'changed' | Set-Content -LiteralPath $file -Encoding utf8
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
        ) | Set-Content -LiteralPath $RedirectStandardOutput -Encoding utf8
        'retained stderr' | Set-Content -LiteralPath $RedirectStandardError -Encoding utf8
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
    'mock ETL' | Set-Content -LiteralPath (Join-Path $run 'trace.etl') -Encoding utf8
    Invoke-ResizeCapture -Phase Check -RunDirectory $run
    Assert-True ((Get-Content (Join-Path $run 'check.json') -Raw | ConvertFrom-Json).SameRun) 'Check did not persist'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-tle' }).Count -eq 0) 'Trace loss suppression requested'
    $script:failOn = 'tracestats'
    Assert-Throws { Invoke-ResizeCapture -Phase Check -RunDirectory $run } 'simulated'
    Assert-True (-not (Get-Content (Join-Path $run 'check.json') -Raw | ConvertFrom-Json).SameRun) 'Stale successful Check survived decoder failure'
    $script:failOn = ''
    'missing anchors' | Set-Content -LiteralPath (Join-Path $run 'stdout.log') -Encoding utf8
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
    # A missing Windows Performance Toolkit must not consume the run's one-shot collector claim.
    # Resolving wpr after the claim left a zero-byte collector.json, so every retry failed with
    # 'already exists' and no failure metadata was retained, stranding a good prepared snapshot.
    $script:elevated = $true
    $script:failOn = ''
    $script:calls.Clear()
    $run = New-TestRun
    function Get-Command { param($Name, $ErrorAction) throw "mock: '$Name' is not recognized" }
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo } 'not recognized'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $run 'collector.json'))) 'Missing wpr consumed the collector claim'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-start' }).Count -eq 0) 'Started a recording without wpr'
    # The prepared run must still be usable once the toolkit is available.
    function Get-Command { param($Name, $ErrorAction) @{ Source = "mock-$Name.exe" } }
    Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -RepoRoot $repo
    Assert-True ((Get-Content (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json).State -eq 'saved') 'Run unusable after a missing-wpr failure'


    # ---- Ring-buffer mode -------------------------------------------------------------
    # Pure predicates first, independent of the capture lifecycle.
    # The defining difference from file mode: the operator saves the buffer the moment a freeze
    # is seen, so the app legitimately exits AFTER the ETL is written. File mode must still
    # reject that shape, or the two rules would be interchangeable and the guarantee meaningless.
    $ringCollector = @{ State = 'saved'; StartUtc = '2026-09-09T03:00:00Z'; StopRequestedUtc = '2026-09-09T03:02:00Z' }
    $lateApp = @{ State = 'exited'; ExitCode = 0; StartUtc = '2026-09-09T03:00:30Z'; EndUtc = '2026-09-09T03:05:00Z' }
    Assert-True (Test-ResizeRingCoverage $ringCollector $lateApp) 'Ring run rejected an app that exited after the save'
    Assert-True (-not (Test-ResizeCaptureOverlap $ringCollector $lateApp)) 'File-mode rule accepted a post-save exit'
    # App must still start inside the recording and before the save, and exit cleanly.
    Assert-True (-not (Test-ResizeRingCoverage $ringCollector @{ State = 'exited'; ExitCode = 0; StartUtc = '2026-09-09T02:59:00Z'; EndUtc = '2026-09-09T03:05:00Z' })) 'Ring accepted an app started before the recording'
    Assert-True (-not (Test-ResizeRingCoverage $ringCollector @{ State = 'exited'; ExitCode = 0; StartUtc = '2026-09-09T03:03:00Z'; EndUtc = '2026-09-09T03:05:00Z' })) 'Ring accepted an app started after the save'
    Assert-True (-not (Test-ResizeRingCoverage $ringCollector @{ State = 'exited'; ExitCode = 7; StartUtc = '2026-09-09T03:00:30Z'; EndUtc = '2026-09-09T03:05:00Z' })) 'Ring accepted a failed app'
    Assert-True (-not (Test-ResizeRingCoverage @{ State = 'recording'; StartUtc = '2026-09-09T03:00:00Z'; StopRequestedUtc = '2026-09-09T03:02:00Z' } $lateApp)) 'Ring accepted an unsaved collector'

    # Save request signalling, exercised directly so the bounded wait stays fast.
    $waitRun = New-TestRun
    Assert-True (-not (Wait-ResizeSaveRequest -RunDirectory $waitRun -TimeoutSeconds 1)) 'Wait reported a save nobody requested'
    'now' | Set-Content -LiteralPath (Join-Path $waitRun 'save.request') -Encoding utf8
    Assert-True (Wait-ResizeSaveRequest -RunDirectory $waitRun -TimeoutSeconds 1) 'Wait missed an existing save request'

    # Collect -Buffering Ring must omit -filemode (that is what selects WPR's memory buffer)
    # and must stop on the save request rather than only on the deadline.
    $script:elevated = $true
    $script:failOn = ''
    $script:calls.Clear()
    $run = New-TestRun
    function Wait-ResizeSaveRequest { param([string]$RunDirectory, [int]$TimeoutSeconds) return $true }
    Invoke-ResizeCapture -Phase Collect -RunDirectory $run -DurationSeconds 15 -Buffering Ring -RepoRoot $repo
    $ringSaved = Get-Content -LiteralPath (Join-Path $run 'collector.json') -Raw | ConvertFrom-Json
    Assert-True ($ringSaved.State -eq 'saved') 'Ring collector did not save'
    Assert-True ($ringSaved.Buffering -eq 'ring') 'Ring buffering not recorded'
    Assert-True ($ringSaved.SaveTrigger -eq 'requested') 'Save trigger not recorded'
    $ringStart = @($script:calls | Where-Object { $_.Arguments -contains '-start' })[0]
    Assert-True (-not ($ringStart.Arguments -contains '-filemode')) 'Ring mode still passed -filemode'
    Assert-True ($ringStart.Arguments[-2] -eq '-instancename') 'Instance option must remain last in ring mode'
    Assert-True (@($ringStart.Arguments | Where-Object { $_ -like '*TaskSmackResize.wprp!TaskSmackResize' }).Count -eq 1) 'Ring mode dropped the focused profile'
    # -filemode selects a profile's .File variant, so a ring capture's retained profile details
    # must omit it or they describe a variant the recording is not using.
    $ringDetail = @($script:calls | Where-Object { $_.Arguments -contains '-profiledetails' })[0]
    Assert-True (-not ($ringDetail.Arguments -contains '-filemode')) 'Ring mode documented the File variant'
    # File mode must be unchanged.
    $script:calls.Clear()
    $run2 = New-TestRun
    Invoke-ResizeCapture -Phase Collect -RunDirectory $run2 -DurationSeconds 15 -RepoRoot $repo
    $fileStart = @($script:calls | Where-Object { $_.Arguments -contains '-start' })[0]
    Assert-True ($fileStart.Arguments -contains '-filemode') 'File mode lost -filemode'
    $fileDetail = @($script:calls | Where-Object { $_.Arguments -contains '-profiledetails' })[0]
    Assert-True ($fileDetail.Arguments -contains '-filemode') 'File mode stopped documenting the File variant'
    Assert-True ((Get-Content (Join-Path $run2 'collector.json') -Raw | ConvertFrom-Json).Buffering -eq 'file') 'File buffering not recorded'

    # Save phase: normal-user, ring-only, single-shot.
    $script:elevated = $false
    $saveRun = New-TestRun
    Write-ResizeCaptureJson (Join-Path $saveRun 'collector.json') @{ State = 'saved'; Buffering = 'ring' }
    Assert-Throws { Invoke-ResizeCapture -Phase Save -RunDirectory $saveRun } 'not recording'
    Write-ResizeCaptureJson (Join-Path $saveRun 'collector.json') @{ State = 'recording'; Buffering = 'file' }
    Assert-Throws { Invoke-ResizeCapture -Phase Save -RunDirectory $saveRun } 'only to a ring-buffer'
    Write-ResizeCaptureJson (Join-Path $saveRun 'collector.json') @{ State = 'recording'; Buffering = 'ring' }
    Invoke-ResizeCapture -Phase Save -RunDirectory $saveRun
    Assert-True (Test-Path -LiteralPath (Join-Path $saveRun 'save.request')) 'Save did not signal the collector'
    Assert-Throws { Invoke-ResizeCapture -Phase Save -RunDirectory $saveRun } 'already requested'

    # Check on a ring run: accepts the post-save exit and must carry the depth caveat, because
    # timestamps cannot prove the memory buffer still reaches back to the stall.
    $checkRun = New-TestRun
    $ringStartUtc = [DateTimeOffset]::UtcNow.AddSeconds(-120).ToString('o')
    Write-ResizeCaptureJson (Join-Path $checkRun 'collector.json') @{
        State = 'saved'; Buffering = 'ring'; SaveTrigger = 'requested'
        StartUtc = $ringStartUtc; StopRequestedUtc = [DateTimeOffset]::UtcNow.AddSeconds(-30).ToString('o')
    }
    Write-ResizeCaptureJson (Join-Path $checkRun 'app.json') @{
        State = 'exited'; ExitCode = 0; Pid = 4242
        StartUtc = [DateTimeOffset]::UtcNow.AddSeconds(-110).ToString('o')
        EndUtc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    @(
        'ResizePerfAnchor: pid=4242 uiTid=99 clock=QPC frequency=10000000'
        'ResizePerfAnchor: pid=4242 uiTid=99 clock=QPC frequency=10000000'
        'ResizePerfWallSummary: loops=10 loopMax=1.0 ms'
    ) | Set-Content -LiteralPath (Join-Path $checkRun 'stdout.log') -Encoding utf8
    'mock ETL' | Set-Content -LiteralPath (Join-Path $checkRun 'trace.etl') -Encoding utf8
    Invoke-ResizeCapture -Phase Check -RunDirectory $checkRun
    $ringCheck = Get-Content -LiteralPath (Join-Path $checkRun 'check.json') -Raw | ConvertFrom-Json
    Assert-True $ringCheck.SameRun 'Ring Check rejected a valid ring run'
    Assert-True ($ringCheck.Buffering -eq 'ring') 'Ring Check did not record buffering'
    Assert-True ($ringCheck.Diagnostics -like '*RING MODE*') 'Ring Check dropped the buffer-depth caveat'

    # ---- Provider set selection (#912) ------------------------------------------------
    # The heavyweight set must stay reachable, and must be an explicit choice rather than the
    # default: as the default it measured ~48% of its own logging inside a recorded stall.
    $script:elevated = $true
    $script:failOn = ''
    $script:calls.Clear()
    $verboseRun = New-TestRun
    Invoke-ResizeCapture -Phase Collect -RunDirectory $verboseRun -DurationSeconds 15 -RepoRoot $repo -ProviderSet Verbose
    $vStart = @($script:calls | Where-Object { $_.Arguments -contains '-start' })[0]
    foreach ($heavy in @('GeneralProfile.Verbose', 'GPU.Verbose', 'DesktopComposition.Verbose')) {
        Assert-True ($vStart.Arguments -contains $heavy) "Verbose opt-in dropped $heavy"
    }
    Assert-True (@($vStart.Arguments | Where-Object { $_ -like '*TaskSmackResize.wprp*' }).Count -eq 0) 'Verbose opt-in also started the focused profile'
    Assert-True ((Get-Content (Join-Path $verboseRun 'collector.json') -Raw | ConvertFrom-Json).ProviderSet -eq 'verbose') 'Verbose provider set not recorded'

    # The resolved providers/keywords of the profile actually used must be retained, so a
    # capture's configuration is recoverable from its own artifacts (#912). A focused profile
    # spec is a path plus '!Name', which cannot be a filename verbatim.
    $script:calls.Clear()
    $detailRun = New-TestRun
    Invoke-ResizeCapture -Phase Collect -RunDirectory $detailRun -DurationSeconds 15 -RepoRoot $repo
    Assert-True (Test-Path -LiteralPath (Join-Path $detailRun 'profiledetails-1.txt')) 'Focused profile details not retained'
    $detailCall = @($script:calls | Where-Object { $_.Arguments -contains '-profiledetails' })[0]
    Assert-True (@($detailCall.Arguments | Where-Object { $_ -like '*TaskSmackResize.wprp!TaskSmackResize' }).Count -eq 1) 'Profile details taken for the wrong profile'

    # A missing profile file must fail before the recording starts, not half-way through.
    $script:calls.Clear()
    $missingRun = New-TestRun
    Assert-Throws { Invoke-ResizeCapture -Phase Collect -RunDirectory $missingRun -DurationSeconds 15 -RepoRoot (Join-Path $root 'no-such-repo') } 'Provider profile is missing'
    Assert-True (@($script:calls | Where-Object { $_.Arguments -contains '-start' }).Count -eq 0) 'Started a recording without a provider profile'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $missingRun 'collector.json'))) 'Missing provider profile consumed the collector claim'

    Write-Host 'Resize capture lifecycle, overlap, ring-buffer, privilege, identity and failure tests passed.'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
