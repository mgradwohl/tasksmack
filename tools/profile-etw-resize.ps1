# Dot-sourced by profile-etw.ps1. Collector and app deliberately run in separate terminals:
# the elevated collector never executes a binary or command from the shared run manifest.
Set-StrictMode -Version Latest

function Get-ResizeCaptureIdentity {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    @{
        User = $identity.Name
        Sid = $identity.User.Value
        Elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        LauncherPid = $PID
        Utc = [DateTimeOffset]::UtcNow.ToString('o')
        Qpc = [Diagnostics.Stopwatch]::GetTimestamp()
        QpcFrequency = [Diagnostics.Stopwatch]::Frequency
    }
}

function Write-ResizeCaptureJson {
    param([string]$Path, $Value)
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Invoke-ResizeCaptureCommand {
    param([string]$Exe, [string[]]$Arguments, [string]$Log)
    $PSNativeCommandUseErrorActionPreference = $false
    $before = [DateTimeOffset]::UtcNow.ToString('o')
    & $Exe @Arguments > $Log 2>&1
    $code = $LASTEXITCODE
    Write-ResizeCaptureJson "$Log.json" @{
        Executable = $Exe; Arguments = $Arguments; StartUtc = $before
        EndUtc = [DateTimeOffset]::UtcNow.ToString('o'); ExitCode = $code
    }
    if ($code -ne 0) {
        throw "$Exe exited $code. Retained diagnostics: $Log"
    }
}

function Assert-ResizeCaptureFiles {
    param([string]$RunDirectory, $Manifest)
    foreach ($file in $Manifest.Files) {
        $path = Join-Path $RunDirectory $file.RelativePath
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Sha256) {
            throw "Capture artifact changed: $path"
        }
    }
}

function Test-ResizeCaptureOverlap {
    param($Collector, $App)
    return $Collector.State -eq 'saved' -and $App.State -eq 'exited' -and $App.ExitCode -eq 0 -and
        [DateTimeOffset]::Parse($App.StartUtc) -ge [DateTimeOffset]::Parse($Collector.StartUtc) -and
        [DateTimeOffset]::Parse($App.EndUtc) -le [DateTimeOffset]::Parse($Collector.StopRequestedUtc)
}

# A run directory inside the worktree that git does not ignore corrupts the provenance this
# capture exists to establish: Prepare writes configure.log before CMake evaluates
# GIT_SOURCE_STATE, so the binary is stamped configureSourceState=dirty by the capture's own
# output, and checkout-status.txt then reports diagnostic artifacts as source changes. Refuse
# such a directory up front rather than producing artifacts whose provenance describes itself.
function Assert-ResizeRunDirectoryIgnored {
    param([string]$RunDirectory, [string]$RepoRoot)
    $root = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\')
    $run = [IO.Path]::GetFullPath($RunDirectory)
    if (-not $run.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        return  # Outside the worktree: nothing to pollute.
    }
    # Probe a file path inside the run directory, not the directory itself: the guard runs
    # before the directory exists, and `git check-ignore` cannot match a directory-only
    # pattern (a .gitignore entry with a trailing slash) against a not-yet-existing path.
    # configure.log is the first artifact Prepare writes, so this is the exact question we mean.
    $probe = Join-Path $run 'configure.log'
    $PSNativeCommandUseErrorActionPreference = $false
    & git -C $RepoRoot check-ignore --quiet -- $probe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw ("Run directory is inside the worktree but not git-ignored: $run`n" +
               'Capture output would be stamped into version.h as configureSourceState=dirty and ' +
               'reported by checkout-status.txt as a source change. Add it to .gitignore ' +
               '(perf-data/resize-*/ and build/ are already ignored) or choose a path outside the repository.')
    }
}

# Ring-buffer coverage. Deliberately weaker than Test-ResizeCaptureOverlap and NOT a
# substitute for it: in ring mode the operator saves the buffer the moment a freeze is seen,
# while the app is usually still running, so the app legitimately exits AFTER the ETL is
# written. What must still hold is that the app started inside the recording and before the
# save. Whether the ring actually still reaches back to the stall cannot be decided from
# timestamps -- the buffer is memory-bounded and silently discards its oldest events -- so
# Check reports that as a required manual verification rather than claiming coverage.
function Test-ResizeRingCoverage {
    param($Collector, $App)
    if ($Collector.State -ne 'saved' -and $Collector.State -ne 'saved-with-errors') { return $false }
    if ($App.State -ne 'exited' -or $App.ExitCode -ne 0) { return $false }
    $appStart = [DateTimeOffset]::Parse($App.StartUtc)
    return $appStart -ge [DateTimeOffset]::Parse($Collector.StartUtc) -and
        $appStart -le [DateTimeOffset]::Parse($Collector.StopRequestedUtc)
}

# Ring mode records until an operator who just saw a freeze asks for the buffer to be written,
# rather than stopping on a timer and hoping a rare stall landed inside a fixed window. The
# request is a file so the normal-user terminal can trigger the elevated collector without
# either terminal holding a handle to the other; the bounded deadline still applies.
function Wait-ResizeSaveRequest {
    param([string]$RunDirectory, [int]$TimeoutSeconds)
    $request = Join-Path $RunDirectory 'save.request'
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $request) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

function Invoke-ResizeCapture {
    param(
        [string]$Phase, [string]$RunDirectory, [string]$Preset, [switch]$SkipBuild,
        [int]$DurationSeconds, [string]$RepoRoot, [string]$Buffering = 'File',
        [string]$ProviderSet = 'Focused'
    )
    $ErrorActionPreference = 'Stop'
    if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        throw 'resize mode requires -RunDirectory (a new directory for each run).'
    }
    $RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
    $identity = Get-ResizeCaptureIdentity
    if ($Phase -ne 'Collect' -and $identity.Elevated) {
        throw ('Run Prepare/App/Save/Check in a normal-user terminal; only Collect may be elevated. ' +
               'An elevated target changes the workload being measured (see #872), so this is refused rather ' +
               'than warned about. Open a separate non-elevated PowerShell 7 window for this phase.')
    }
    if ($Phase -eq 'Check') {
        Write-ResizeCaptureJson (Join-Path $RunDirectory 'check.json') @{
            CheckedBy = $identity; SameRun = $false; Diagnostics = 'Validation pending or failed; see command error.'
        }
    }

    if ($Phase -eq 'Prepare') {
        if (Test-Path -LiteralPath $RunDirectory) { throw "Run directory already exists: $RunDirectory" }
        Assert-ResizeRunDirectoryIgnored -RunDirectory $RunDirectory -RepoRoot $RepoRoot
        if (-not $Preset) { $Preset = 'win-optimized' }
        New-Item -ItemType Directory -Path $RunDirectory | Out-Null
        if (-not $SkipBuild) {
            Push-Location $RepoRoot
            try {
                Invoke-ResizeCaptureCommand cmake @('--preset', $Preset) (Join-Path $RunDirectory 'configure.log')
                Invoke-ResizeCaptureCommand cmake @('--build', '--preset', $Preset) (Join-Path $RunDirectory 'build.log')
            }
            finally { Pop-Location }
        }
        $build = Join-Path $RepoRoot "build\$Preset"
        $bin = Join-Path $build 'bin'
        $snapshot = Join-Path $RunDirectory 'binary'
        New-Item -ItemType Directory -Path $snapshot | Out-Null
        # Keep assets/sidecar DLLs with the exact executable rather than running a mutable build output.
        Copy-Item -Path (Join-Path $bin '*') -Destination $snapshot -Recurse
        if (-not (Test-Path -LiteralPath (Join-Path $snapshot 'TaskSmack.exe'))) { throw 'TaskSmack.exe is missing.' }
        Copy-Item -LiteralPath (Join-Path $build 'CMakeCache.txt') -Destination $RunDirectory
        Copy-Item -LiteralPath (Join-Path $build 'generated\version.h') -Destination $RunDirectory
        Invoke-ResizeCaptureCommand git @('-C', $RepoRoot, 'rev-parse', 'HEAD') (Join-Path $RunDirectory 'checkout-head.txt')
        Invoke-ResizeCaptureCommand git @('-C', $RepoRoot, 'status', '--porcelain') (Join-Path $RunDirectory 'checkout-status.txt')
        $files = @(Get-ChildItem -LiteralPath $snapshot -File -Recurse | ForEach-Object {
            @{
                RelativePath = [IO.Path]::GetRelativePath($RunDirectory, $_.FullName)
                Length = $_.Length
                Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        })
        $pdbs = @($files | Where-Object { $_.RelativePath -like '*.pdb' })
        Write-ResizeCaptureJson (Join-Path $RunDirectory 'run.json') @{
            Schema = 1; PreparedBy = $identity; Preset = $Preset; SkipBuild = [bool]$SkipBuild
            Files = $files; PdbCount = $pdbs.Count
            SymbolPath = $snapshot
            Provenance = 'version.h is configure-time metadata; checkout-*.txt describes capture-time checkout, not necessarily binary source.'
        }
        if ($pdbs.Count -eq 0) { Write-Warning 'No PDB in binary snapshot. Timing-only run; use win-profile for symbol-rich attribution.' }
        Write-Host "Prepared $RunDirectory. In a separate elevated terminal run resize -Phase Collect with this directory."
        return
    }

    $manifest = Get-Content -LiteralPath (Join-Path $RunDirectory 'run.json') -Raw | ConvertFrom-Json
    $collectorPath = Join-Path $RunDirectory 'collector.json'
    $appPath = Join-Path $RunDirectory 'app.json'
    $tracePath = Join-Path $RunDirectory 'trace.etl'
    if ($Phase -eq 'Collect') {
        if (-not $identity.Elevated) { throw 'Collect requires a separately opened elevated terminal. It never launches the app.' }
        if ($DurationSeconds -eq 0) { $DurationSeconds = 180 }
        if ($DurationSeconds -lt 15 -or $DurationSeconds -gt 600) { throw 'Collector duration must be 15-600 seconds.' }
        # Resolve wpr BEFORE the one-shot claim below. Resolving after it means a missing
        # Windows Performance Toolkit consumes the run's only collector claim, leaves a
        # zero-byte collector.json, and makes every retry fail with "already exists" with no
        # failure metadata retained -- stranding an otherwise good prepared snapshot.
        $wpr = (Get-Command wpr -ErrorAction Stop).Source
        $ring = $Buffering -eq 'Ring'
        # Focused is the default because the original three-Verbose-profile set accounted for
        # 48.1% of the sampled CPU inside the 1064 ms stall it was recording (#912): it measured
        # its own logging more than the stall. TaskSmackResize.wprp keeps the scheduler and
        # sampling data attribution needs, with the GPU/compositor providers, and drops
        # GeneralProfile's stack-walked Win32k provider and its DiskIO/DPC/Interrupt/fault
        # keywords. Verbose remains available for cases that genuinely need the wider set.
        $focused = $ProviderSet -ne 'Verbose'
        if ($focused) {
            $wprp = Join-Path $RepoRoot 'tools\TaskSmackResize.wprp'
            if (-not (Test-Path -LiteralPath $wprp)) { throw "Provider profile is missing: $wprp" }
            # wpr selects the .File or .Memory variant of this profile from the presence of
            # -filemode below, exactly as it does for the built-in profiles.
            $profiles = @("$wprp!TaskSmackResize")
        }
        else {
            $profiles = @('GeneralProfile.Verbose', 'GPU.Verbose', 'DesktopComposition.Verbose')
        }
        # CreateNew prevents simultaneous/repeated collectors from claiming this run.
        $claim = [IO.File]::Open($collectorPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $claim.Dispose()
        $instance = "TaskSmackResize-$([guid]::NewGuid().ToString('N'))"
        $collector = @{
            State = 'starting'; Identity = $identity; Instance = $instance
            ProviderSet = if ($focused) { 'focused' } else { 'verbose' }
            Profiles = $profiles
            Buffering = if ($ring) { 'ring' } else { 'file' }
            StartUtc = $null; StopRequestedUtc = $null; EndUtc = $null; Error = $null
            SaveTrigger = $null
        }
        Write-ResizeCaptureJson $collectorPath $collector
        $started = $false
        try {
            Invoke-ResizeCaptureCommand whoami @('/all') (Join-Path $RunDirectory 'collector-token.txt')
            # Retains the resolved providers, keywords and stack settings for the profile actually
            # used, so a capture's configuration is recoverable from its own artifacts instead of
            # having to be inferred from the samples afterwards (#912). A focused profile spec is a
            # path plus '!Name', so the log filename is sanitised rather than derived verbatim.
            $detailIndex = 0
            foreach ($profileName in $profiles) {
                ++$detailIndex
                $safe = if ($focused) { "profiledetails-$detailIndex" } else { $profileName }
                Invoke-ResizeCaptureCommand $wpr @('-profiledetails', $profileName, '-filemode') `
                    (Join-Path $RunDirectory "$safe.txt")
            }
            # Omitting -filemode selects WPR's in-memory ring buffer: events accumulate and the
            # oldest are overwritten until -stop writes whatever the buffer still holds.
            $startArgs = @()
            foreach ($profileName in $profiles) { $startArgs += @('-start', $profileName) }
            if (-not $ring) { $startArgs += '-filemode' }
            $startArgs += @('-instancename', $instance)
            Invoke-ResizeCaptureCommand $wpr $startArgs (Join-Path $RunDirectory 'wpr-start.log')
            $started = $true
            $collector.StartUtc = [DateTimeOffset]::UtcNow.ToString('o')
            $collector.DeadlineUtc = [DateTimeOffset]::UtcNow.AddSeconds($DurationSeconds).ToString('o')
            $collector.State = 'recording'
            Write-ResizeCaptureJson $collectorPath $collector
            if ($ring) {
                Write-Host "Ring buffer recording $instance (deadline $DurationSeconds s). Run resize -Phase App in the normal-user terminal,"
                Write-Host 'resize until you SEE a freeze, then run resize -Phase Save there to write the buffer.'
                $requested = Wait-ResizeSaveRequest -RunDirectory $RunDirectory -TimeoutSeconds $DurationSeconds
                $collector.SaveTrigger = if ($requested) { 'requested' } else { 'deadline' }
            }
            else {
                Write-Host "Recording $instance for $DurationSeconds seconds. Run resize -Phase App in the normal-user terminal now."
                Start-Sleep -Seconds $DurationSeconds
                $collector.SaveTrigger = 'deadline'
            }
        }
        catch {
            $collector.State = 'failed'
            $collector.Error = $_.ToString()
            throw
        }
        finally {
            try {
                if ($started) {
                    $collector.StopRequestedUtc = [DateTimeOffset]::UtcNow.ToString('o')
                    Invoke-ResizeCaptureCommand $wpr @('-status', 'collectors', '-details', '-instancename', $instance) `
                        (Join-Path $RunDirectory 'wpr-status.log')
                }
            }
            catch {
                $collector.Error = $_.ToString()
                throw
            }
            finally {
                try {
                    if ($started) {
                        Invoke-ResizeCaptureCommand $wpr @('-stop', $tracePath, '-instancename', $instance) `
                            (Join-Path $RunDirectory 'wpr-stop.log')
                        $collector.State = if ($collector.Error) { 'saved-with-errors' } else { 'saved' }
                    }
                }
                catch {
                    $collector.State = 'stop-failed'
                    $collector.Error = "$_ Recovery: wpr -stop `"$tracePath`" -instancename $instance"
                    throw
                }
                finally {
                    $collector.EndUtc = [DateTimeOffset]::UtcNow.ToString('o')
                    Write-ResizeCaptureJson $collectorPath $collector
                }
            }
        }
        return
    }

    # Save runs the instant the operator sees a freeze, so it deliberately skips the snapshot
    # hash re-verification: every second spent hashing is ring-buffer depth overwritten. Check
    # still verifies hashes afterwards.
    if ($Phase -eq 'Save') {
        $collector = Get-Content -LiteralPath $collectorPath -Raw | ConvertFrom-Json
        if ($collector.State -ne 'recording') { throw "Collector is not recording (state '$($collector.State)'); nothing to save." }
        if ($collector.Buffering -ne 'ring') { throw 'Save applies only to a ring-buffer collector started with -Buffering Ring.' }
        $request = Join-Path $RunDirectory 'save.request'
        if (Test-Path -LiteralPath $request) { throw 'A save was already requested for this run.' }
        Set-Content -LiteralPath $request -Value ([DateTimeOffset]::UtcNow.ToString('o')) -Encoding utf8
        Write-Host 'Save requested. The elevated collector will stop and write trace.etl; then close the app and run Check.'
        return
    }

    Assert-ResizeCaptureFiles $RunDirectory $manifest
    $collector = Get-Content -LiteralPath $collectorPath -Raw | ConvertFrom-Json
    if ($Phase -eq 'App') {
        if ($collector.State -ne 'recording') { throw 'Collector is not recording. Start Collect first.' }
        $remaining = ([DateTimeOffset]::Parse($collector.DeadlineUtc) - [DateTimeOffset]::UtcNow).TotalSeconds
        if ($remaining -lt 15) { throw 'Less than 15 seconds remain; prepare a fresh run.' }
        $claim = [IO.File]::Open($appPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $claim.Dispose()
        $app = @{ State = 'starting'; Identity = $identity; Pid = $null; StartUtc = $null; EndUtc = $null; ExitCode = $null; Error = $null }
        $previousTrace = $env:TASKSMACK_TRACE_RESIZE_PERF
        $previousLog = $env:TASKSMACK_LOG_LEVEL
        try {
            Invoke-ResizeCaptureCommand whoami @('/all') (Join-Path $RunDirectory 'app-launcher-token.txt')
            $env:TASKSMACK_TRACE_RESIZE_PERF = '1'
            $env:TASKSMACK_LOG_LEVEL = 'info'
            $app.StartUtc = [DateTimeOffset]::UtcNow.ToString('o')
            $process = Start-Process -FilePath (Join-Path $RunDirectory 'binary\TaskSmack.exe') -WorkingDirectory $RunDirectory `
                -RedirectStandardOutput (Join-Path $RunDirectory 'stdout.log') `
                -RedirectStandardError (Join-Path $RunDirectory 'stderr.log') -PassThru
            $app.Pid = $process.Id
            $app.PrivilegeSource = 'Inherited non-elevated launcher token; no RunAs. See app-launcher-token.txt.'
            $app.State = 'running'
            Write-ResizeCaptureJson $appPath $app
            Write-Host "App PID $($process.Id), normal-user. Resize for 20-30 seconds, then close the app."
            if (-not $process.WaitForExit([int](($remaining - 5) * 1000))) {
                $null = $process.CloseMainWindow()
                if (-not $process.WaitForExit(5000)) {
                    throw "App PID $($process.Id) did not close before the capture deadline. Left running; close it manually."
                }
            }
            # The timed overload need not wait for asynchronous redirected-output handlers.
            $null = $process.WaitForExit()
            $app.ExitCode = $process.ExitCode
            $app.EndUtc = [DateTimeOffset]::UtcNow.ToString('o')
            $app.State = 'exited'
            if ($null -eq $app.ExitCode -or $app.ExitCode -ne 0) { throw "App exit status '$($app.ExitCode)' is not successful; retain this incomplete run." }
        }
        catch {
            $app.Error = $_.ToString()
            if ($app.State -ne 'exited') { $app.State = 'failed' }
            throw
        }
        finally {
            $env:TASKSMACK_TRACE_RESIZE_PERF = $previousTrace
            $env:TASKSMACK_LOG_LEVEL = $previousLog
            Write-ResizeCaptureJson $appPath $app
        }
        return
    }

    $app = Get-Content -LiteralPath $appPath -Raw | ConvertFrom-Json
    $ringRun = $collector.PSObject.Properties.Name -contains 'Buffering' -and $collector.Buffering -eq 'ring'
    if ($ringRun) {
        if (-not (Test-ResizeRingCoverage $collector $app)) {
            throw 'Incomplete ring capture: app did not start inside the recording before the save, or did not exit successfully.'
        }
    }
    elseif (-not (Test-ResizeCaptureOverlap $collector $app)) {
        throw 'Incomplete capture: app did not exit successfully entirely inside this ETW recording.'
    }
    if (-not (Test-Path -LiteralPath $tracePath) -or (Get-Item -LiteralPath $tracePath).Length -eq 0) { throw 'ETL is missing or empty.' }
    $stdout = Get-Content -LiteralPath (Join-Path $RunDirectory 'stdout.log') -Raw
    $anchors = @([regex]::Matches($stdout, "ResizePerfAnchor: pid=$($app.Pid) uiTid=\d+ clock=QPC "))
    if ($anchors.Count -lt 2 -or -not $stdout.Contains('ResizePerfWallSummary:')) {
        throw 'Incomplete app diagnostics: expected startup/shutdown QPC anchors and wall summary for the captured PID.'
    }
    $xperf = Get-Command xperf -ErrorAction SilentlyContinue
    if (-not $xperf) { throw 'xperf unavailable: same-run artifacts retained but loss/decoder validation is still required.' }
    Invoke-ResizeCaptureCommand $xperf.Source @('-i', $tracePath, '-a', 'tracestats') (Join-Path $RunDirectory 'trace-statistics.txt')
    Write-ResizeCaptureJson (Join-Path $RunDirectory 'check.json') @{
        CheckedBy = $identity; SameRun = $true
        TraceSha256 = (Get-FileHash -LiteralPath $tracePath -Algorithm SHA256).Hash
        Buffering = if ($ringRun) { 'ring' } else { 'file' }
        # Absent on collector.json written before ring mode existed; StrictMode makes a bare
        # property read on a PSCustomObject fatal, so probe rather than assume.
        SaveTrigger = if ($collector.PSObject.Properties.Name -contains 'SaveTrigger') { $collector.SaveTrigger } else { $null }
        Diagnostics = if ($ringRun) {
            'Review required: trace-statistics.txt, wpr logs, scheduler stacks and GPU/DWM provider presence in WPA. ' +
            'No automatic loss-free claim. RING MODE: the buffer is memory-bounded and silently overwrites its oldest ' +
            'events, so timestamps cannot show whether the ETL still reaches the stall. Confirm in WPA that the ETL ' +
            'time range actually covers the app-log counter interval before attributing anything.'
        } else {
            'Review required: trace-statistics.txt, wpr logs, scheduler stacks and GPU/DWM provider presence in WPA. No automatic loss-free claim.'
        }
    }
    Write-Host 'Same-run artifacts verified. Review trace-statistics.txt and wpr-*.log for loss/decoder diagnostics before attribution.'
    if ($ringRun) {
        Write-Host 'RING MODE: verify in WPA that the ETL time range still covers the stall; the buffer discards its oldest events.'
    }
    Write-Host "Symbols: $($manifest.SymbolPath). In WPA verify UI TID, CSwitch/ReadyThread stacks, DxgKrnl and DWM events."
}
