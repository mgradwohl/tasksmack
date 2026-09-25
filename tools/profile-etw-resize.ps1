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

function Invoke-ResizeCapture {
    param(
        [string]$Phase, [string]$RunDirectory, [string]$Preset, [switch]$SkipBuild,
        [int]$DurationSeconds, [string]$RepoRoot
    )
    $ErrorActionPreference = 'Stop'
    if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        throw 'resize mode requires -RunDirectory (a new directory for each run).'
    }
    $RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
    $identity = Get-ResizeCaptureIdentity
    if ($Phase -ne 'Collect' -and $identity.Elevated) {
        throw 'Run Prepare/App/Check in a normal-user terminal; only Collect may be elevated.'
    }
    if ($Phase -eq 'Check') {
        Write-ResizeCaptureJson (Join-Path $RunDirectory 'check.json') @{
            CheckedBy = $identity; SameRun = $false; Diagnostics = 'Validation pending or failed; see command error.'
        }
    }

    if ($Phase -eq 'Prepare') {
        if (Test-Path -LiteralPath $RunDirectory) { throw "Run directory already exists: $RunDirectory" }
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
        # CreateNew prevents simultaneous/repeated collectors from claiming this run.
        $claim = [IO.File]::Open($collectorPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $claim.Dispose()
        $wpr = (Get-Command wpr -ErrorAction Stop).Source
        $instance = "TaskSmackResize-$([guid]::NewGuid().ToString('N'))"
        $collector = @{
            State = 'starting'; Identity = $identity; Instance = $instance
            Profiles = @('GeneralProfile.Verbose', 'GPU.Verbose', 'DesktopComposition.Verbose')
            StartUtc = $null; StopRequestedUtc = $null; EndUtc = $null; Error = $null
        }
        Write-ResizeCaptureJson $collectorPath $collector
        $started = $false
        try {
            Invoke-ResizeCaptureCommand whoami @('/all') (Join-Path $RunDirectory 'collector-token.txt')
            foreach ($profile in $collector.Profiles) {
                Invoke-ResizeCaptureCommand $wpr @('-profiledetails', $profile, '-filemode') `
                    (Join-Path $RunDirectory "$profile.txt")
            }
            Invoke-ResizeCaptureCommand $wpr @(
                '-start', 'GeneralProfile.Verbose', '-start', 'GPU.Verbose', '-start', 'DesktopComposition.Verbose',
                '-filemode', '-instancename', $instance
            ) (Join-Path $RunDirectory 'wpr-start.log')
            $started = $true
            $collector.StartUtc = [DateTimeOffset]::UtcNow.ToString('o')
            $collector.DeadlineUtc = [DateTimeOffset]::UtcNow.AddSeconds($DurationSeconds).ToString('o')
            $collector.State = 'recording'
            Write-ResizeCaptureJson $collectorPath $collector
            Write-Host "Recording $instance for $DurationSeconds seconds. Run resize -Phase App in the normal-user terminal now."
            Start-Sleep -Seconds $DurationSeconds
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
    if (-not (Test-ResizeCaptureOverlap $collector $app)) { throw 'Incomplete capture: app did not exit successfully entirely inside this ETW recording.' }
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
        Diagnostics = 'Review required: trace-statistics.txt, wpr logs, scheduler stacks and GPU/DWM provider presence in WPA. No automatic loss-free claim.'
    }
    Write-Host 'Same-run artifacts verified. Review trace-statistics.txt and wpr-*.log for loss/decoder diagnostics before attribution.'
    Write-Host "Symbols: $($manifest.SymbolPath). In WPA verify UI TID, CSwitch/ReadyThread stacks, DxgKrnl and DWM events."
}
