# Tests for profile-etw-common.ps1 (profile-etw.ps1's app and bench modes). No ETW sessions,
# UAC prompts or application windows. Registered in CTest on Windows with PowerShell 7; can also
# run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'profile-etw-common.ps1')

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

# #874: a filter matching nothing is an error, several matches a warning, one match fine.
$none = Resolve-BenchmarkFilterMatches -ListOutput @() -Filter 'BM_Nope$'
Assert-True ($none.Severity -eq 'error') 'Zero matches must be an error'
Assert-True ($none.Message -like "*matches no benchmarks*") 'Zero-match message'
$blank = Resolve-BenchmarkFilterMatches -ListOutput @('', '   ') -Filter 'BM_Nope$'
Assert-True ($blank.Severity -eq 'error') 'Blank list output must count as zero matches'

$one = Resolve-BenchmarkFilterMatches -ListOutput @('BM_SystemModel_Refresh') -Filter 'BM_SystemModel_Refresh$'
Assert-True ($one.Severity -eq 'ok') 'One match must be accepted'
Assert-True (@($one.Names).Count -eq 1 -and $one.Names[0] -eq 'BM_SystemModel_Refresh') 'Matched name lost'

$many = Resolve-BenchmarkFilterMatches -ListOutput @('BM_A', 'BM_B', 'BM_C') -Filter 'BM_.*'
Assert-True ($many.Severity -eq 'warning') 'Several matches must warn'
Assert-True (@($many.Names).Count -eq 3) 'Matched names lost'
Assert-True ($many.Message -like '*matches 3 benchmarks*' -and $many.Message -like '*- BM_B*') 'Warning must list the matches'

# Listing goes through the real benchmark binary's exit code: a failing lister throws.
$hostExe = (Get-Process -Id $PID).Path
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-etw-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $fakeLister = Join-Path $root 'lister.cmd'
    Set-Content -LiteralPath $fakeLister -Value "@echo BM_One`r`n@echo BM_Two`r`n@exit /b 0" -Encoding ascii
    $listed = Get-BenchmarkFilterMatches -BinaryPath $fakeLister -Filter 'BM_.*'
    Assert-True ($listed.Severity -eq 'warning' -and @($listed.Names).Count -eq 2) 'Lister output not parsed'
    # Google Benchmark reports a non-matching filter on stderr and exits 0; that message must not
    # be counted as a benchmark name.
    $noMatchLister = Join-Path $root 'nomatch.cmd'
    Set-Content -LiteralPath $noMatchLister -Value "@echo Failed to match any benchmarks against regex: BM_Nope 1>&2`r`n@exit 0" -Encoding ascii
    $noMatch = Get-BenchmarkFilterMatches -BinaryPath $noMatchLister -Filter 'BM_Nope$'
    Assert-True ($noMatch.Severity -eq 'error') "A stderr-only no-match report was counted as a benchmark: $(@($noMatch.Names) -join ',')"
    $failingLister = Join-Path $root 'failing.cmd'
    Set-Content -LiteralPath $failingLister -Value "@echo boom`r`n@exit /b 3" -Encoding ascii
    Assert-Throws { Get-BenchmarkFilterMatches -BinaryPath $failingLister -Filter 'BM_.*' } 'exit 3'

    # #872: the integrity level is measured from the process's own token.
    # Compared with the token's own mandatory label as whoami reports it, not inferred from
    # administrator membership, so this holds for any account (e.g. LocalSystem is System).
    $own = Get-ProcessIntegrityLevel -ProcessId $PID
    # The label's SID (S-1-16-<RID>) is language-independent, unlike its display name, and covers
    # every level including Medium Plus (0x2100).
    # Windows' whoami by full path: with Git for Windows' usr\bin first on PATH (a CTest run from Git
    # Bash), a bare `whoami` is GNU coreutils' whoami, which rejects /groups.
    $whoami = Join-Path $env:SystemRoot 'System32\whoami.exe'
    $label = (& $whoami /groups | Select-String 'S-1-16-(\d+)' | Select-Object -First 1)
    Assert-True ($null -ne $label) 'whoami /groups reported no mandatory label SID'
    $expected = ConvertTo-IntegrityLevelName ([long]$label.Matches[0].Groups[1].Value)
    Assert-True ($own -eq $expected) "This process's integrity measured as '$own', whoami reports '$expected' ($($label.Matches[0].Value))"
    Assert-True ((Get-ProcessIntegrityLevel -ProcessId 999999) -eq 'Unknown') 'A missing process must be Unknown'
    Assert-True ((ConvertTo-IntegrityLevelName 0x2000) -eq 'Medium' -and (ConvertTo-IntegrityLevelName 0x3000) -eq 'High') 'Well-known RIDs'
    Assert-True ((ConvertTo-IntegrityLevelName 0x2500) -eq '0x2500') 'An unknown RID must be shown as hex, not mislabelled'

    # Arguments survive a real process boundary intact: spaces (a checkout under a profile with a
    # space in it), embedded quotes, trailing backslashes and empty strings.
    $echo = Join-Path $root 'echo-args.ps1'
    $echoOut = Join-Path $root 'args.json'
    # The raw argv as Windows splits it (what a benchmark binary receives), not PowerShell's own
    # parameter binding, which would read '--benchmark_out=C:\...' as '-name:value'.
    # The path goes inside a single-quoted literal, so its apostrophes are doubled (a TEMP under
    # C:\Users\O'Brien would otherwise make the child script invalid).
    $echoOutLiteral = $echoOut.Replace("'", "''")
    Set-Content -LiteralPath $echo -Value "[Environment]::GetCommandLineArgs() | Select-Object -Skip 4 | ConvertTo-Json -AsArray | Set-Content -LiteralPath '$echoOutLiteral' -Encoding utf8"
    $tricky = @('--benchmark_out=C:\Users\First Last\perf data\x.json', 'BM_(A|B)$', 'say "hi"', 'C:\trailing\', '', 'plain')
    $commandLine = ConvertTo-CommandLine (@('-NoProfile', '-File', $echo) + $tricky)
    $echoProc = Start-Process -FilePath $hostExe -ArgumentList $commandLine -PassThru -WindowStyle Hidden
    $echoProc.WaitForExit()
    $received = @(Get-Content -LiteralPath $echoOut -Raw | ConvertFrom-Json)
    Assert-True ($received.Count -eq $tricky.Count) "Expected $($tricky.Count) arguments, got $($received.Count): $($received -join ' | ')"
    for ($i = 0; $i -lt $tricky.Count; $i++) {
        Assert-True ($received[$i] -ceq $tricky[$i]) "Argument $i arrived as [$($received[$i])], sent [$($tricky[$i])]"
    }
    Assert-True ((ConvertTo-QuotedArgument 'plain') -ceq 'plain') 'A simple argument is not quoted'

    # -OutputDirectory becomes absolute against the caller's location, because the elevated children
    # start in the repo root and would resolve a relative path there instead.
    Push-Location $root
    try {
        Assert-True ((Resolve-CaptureOutputDirectory -OutputDirectory 'runs\a' -RepoRoot 'C:\repo') -eq (Join-Path $root 'runs\a')) 'Relative output resolves against the caller'
        Assert-True ((Resolve-CaptureOutputDirectory -OutputDirectory 'C:\abs\out' -RepoRoot 'C:\repo') -eq 'C:\abs\out') 'Absolute output is kept'
        Assert-True ((Resolve-CaptureOutputDirectory -OutputDirectory '' -RepoRoot 'C:\repo') -eq 'C:\repo\perf-data') 'Default is perf-data in the repo'
    }
    finally { Pop-Location }

    # A collector's recorded error is reported after it has stopped, too.
    $controlDir = Join-Path $root 'control'
    New-Item -ItemType Directory -Path $controlDir | Out-Null
    Assert-True ((Get-CollectorErrorDetail -ControlDirectory $controlDir) -eq '') 'No error file means no detail'
    Set-Content -LiteralPath (Join-Path $controlDir 'collector-error.txt') -Value 'wpr -stop failed'
    Assert-True ((Get-CollectorErrorDetail -ControlDirectory $controlDir) -eq ' Collector error: wpr -stop failed') 'Recorded error detail'

    # The orchestrator fails fast when the collector exits before it starts recording.
    $exited = Start-Process -FilePath $hostExe -ArgumentList @('-NoProfile', '-Command', 'exit 5') -PassThru -WindowStyle Hidden
    $null = $exited.Handle
    $exited.WaitForExit()
    Assert-Throws { Wait-CollectorMarker -Path (Join-Path $root 'never') -TimeoutSeconds 30 -Collector $exited } 'exited (code 5)'
    # A collector that recorded why it failed has that reason reported.
    Set-Content -LiteralPath (Join-Path $root 'collector-error.txt') -Value 'wpr: profile not found'
    Assert-Throws { Wait-CollectorMarker -Path (Join-Path $root 'never') -TimeoutSeconds 30 -Collector $exited } 'Collector error: wpr: profile not found'
    Assert-Throws { Wait-CollectorMarker -Path (Join-Path $root 'never') -TimeoutSeconds 1 } 'Timed out'
    $marker = Join-Path $root 'started'
    Set-Content -LiteralPath $marker -Value '{}'
    Wait-CollectorMarker -Path $marker -TimeoutSeconds 1

    # An app still running at the end names the wait that actually ran: 30 s after a forced stop
    # for a fixed-duration run, the 4-hour wait only for an interactive one.
    Assert-True ((Get-UnfinishedTargetReason -DurationSeconds 45) -like '*forced stop*') 'Fixed-duration runs must not report the 4-hour wait'
    Assert-True ((Get-UnfinishedTargetReason -DurationSeconds 0) -like '*4-hour wait*') 'Interactive runs report the 4-hour wait'

    # A collector failure is never hidden by an earlier error from the orchestrator: both are
    # reported, and the original exception is kept as the inner one.
    $pending = try { throw 'target launch failed' } catch { $_ }
    Assert-CollectorOutcome -PendingError $null -CollectorFailure $null
    Assert-Throws { Assert-CollectorOutcome -PendingError $null -CollectorFailure 'collector exit 7' } 'collector exit 7'
    Assert-Throws { Assert-CollectorOutcome -PendingError $pending -CollectorFailure $null } 'target launch failed'
    $combined = try { Assert-CollectorOutcome -PendingError $pending -CollectorFailure 'collector exit 7'; $null } catch { $_ }
    Assert-True ($null -ne $combined) 'A pending error with a collector failure must throw'
    Assert-True ($combined.ToString() -like '*target launch failed*' -and $combined.ToString() -like '*collector exit 7*') "Both failures must be reported: $combined"
    Assert-True ($combined.Exception.InnerException.Message -eq 'target launch failed') 'The pending exception must be kept'

    # ── #1186: the preset's compile flags are read from its build tree ────────────────────────
    $fakeBuild = Join-Path $root 'build-fake'
    New-Item -ItemType Directory -Path $fakeBuild | Out-Null
    Set-Content -LiteralPath (Join-Path $fakeBuild 'CMakeCache.txt') -Encoding ascii -Value @(
        '// comment'
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_CXX_FLAGS:STRING=-fms-compatibility'
        'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -march=x86-64-v3'
        'CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=ON'
    )
    $flags = Get-PresetBuildFlags -BuildDirectory $fakeBuild
    Assert-True ($flags.BuildType -eq 'Release' -and $flags.CxxConfigFlags -eq '-O3 -DNDEBUG -march=x86-64-v3' -and $flags.Ipo -eq 'ON') "Flags not read: $($flags | ConvertTo-Json -Compress)"
    $flagLine = Format-PresetBuildFlags -Preset 'win-optimized' -Flags $flags
    Assert-True ($flagLine -like '*win-optimized*-fms-compatibility -O3 -DNDEBUG -march=x86-64-v3*IPO/LTO: ON*') "Flag line: $flagLine"
    # A normal win-release cache has no CMAKE_INTERPROCEDURAL_OPTIMIZATION entry: LTO comes from the
    # cached TASKSMACK_ENABLE_IPO option, and must not be reported as OFF (#1372 review).
    $releaseBuild = Join-Path $root 'build-release-fake'
    New-Item -ItemType Directory -Path $releaseBuild | Out-Null
    Set-Content -LiteralPath (Join-Path $releaseBuild 'CMakeCache.txt') -Encoding ascii -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG'
        'TASKSMACK_ENABLE_IPO:BOOL=ON'
    )
    $releaseFlags = Get-PresetBuildFlags -BuildDirectory $releaseBuild
    Assert-True ($releaseFlags.Ipo -like 'ON*TASKSMACK_ENABLE_IPO*') "IPO from the option not read: $($releaseFlags.Ipo)"
    Assert-True ((Format-PresetBuildFlags -Preset 'win-release' -Flags $releaseFlags) -notlike '*IPO/LTO: OFF*') 'A default release cache must not report LTO off'
    $noFlags = Get-PresetBuildFlags -BuildDirectory (Join-Path $root 'no-such-build')
    Assert-True ($null -eq $noFlags.BuildType -and (Format-PresetBuildFlags -Preset 'x' -Flags $noFlags) -like '*compile flags unknown*') 'A missing build tree must be reported as unknown flags'

    # ── #1186: app lifecycle -- warm-up before the trace, and a crash fails the capture ──────
    # Stub apps (batch files, run hidden): one exits at once with a code, one exits with a code
    # once the capture window has begun, one runs until it is killed. Pinging localhost is the
    # sleep, as timeout.exe needs a console input.
    #
    # Nothing here may depend on how fast a process starts or exits: under a parallel ctest run
    # a cmd.exe can take well over a second to start and exit (#1438). So the "exits later"
    # stubs wait for a release file, written once the capture has passed its checks that the app
    # is still running (see Invoke-StubCapture), rather than sleeping for a fixed time.
    $exitNow0 = Join-Path $root 'exit-now-0.cmd'
    Set-Content -LiteralPath $exitNow0 -Encoding ascii -Value "@exit /b 0"
    $exitNow3 = Join-Path $root 'exit-now-3.cmd'
    Set-Content -LiteralPath $exitNow3 -Encoding ascii -Value "@exit /b 3"
    # The release file sits next to the stubs, which find it through %~dp0 (their own directory),
    # so no path has to survive the ASCII batch file.
    $releaseFile = Join-Path $root 'release-exit-later'
    function New-ExitLaterStub([string]$Leaf, [int]$Code) {
        $path = Join-Path $root $Leaf
        Set-Content -LiteralPath $path -Encoding ascii -Value @(
            '@echo off'
            ':wait'
            "if exist `"%~dp0release-exit-later`" exit /b $Code"
            'ping -n 2 127.0.0.1 >nul'
            'goto wait'
        )
        return $path
    }
    $exitLater3 = New-ExitLaterStub -Leaf 'exit-later-3.cmd' -Code 3
    $exitLater0 = New-ExitLaterStub -Leaf 'exit-later-0.cmd' -Code 0
    $runForever = Join-Path $root 'run-forever.cmd'
    Set-Content -LiteralPath $runForever -Encoding ascii -Value "@echo off`r`n:loop`r`nping -n 2 127.0.0.1 >nul`r`ngoto loop"

    # The recording is a log of events, so the order of trace start/stop against the app is checked.
    $script:events = [System.Collections.Generic.List[string]]::new()
    $script:stubLeaf = $null
    function Test-StubRunning([string]$Leaf) {
        # Each match's own exit state decides, not its presence in the list: a process that has
        # exited can still be listed for a while, and longer under load, while a handle to it is
        # open, as Invoke-AppCapture keeps one (#1438).
        @(Get-CimInstance Win32_Process -Filter "Name = 'cmd.exe'" | Where-Object { $_.CommandLine -like "*$Leaf*" } | Where-Object {
                try { -not (Get-Process -Id $_.ProcessId -ErrorAction Stop).HasExited } catch { $false }
            }).Count -gt 0
    }
    $traceStart = { $script:events.Add('start') }
    $traceStop = { $script:events.Add("stop(app running: $(Test-StubRunning $script:stubLeaf))") }
    function Invoke-StubCapture {
        param([string]$App, [int]$DurationSeconds, [int]$WarmupSeconds = 0, [switch]$IncludeStartup, [scriptblock]$StartTrace = $traceStart)
        $script:events.Clear()
        Remove-Item -LiteralPath $releaseFile -ErrorAction SilentlyContinue
        # The "exits later" stubs are released only once Invoke-AppCapture has entered the capture
        # window: it announces the window ('Recording for ...' or 'Exercise the application ...')
        # after its post-trace-start HasExited check, so a released stub can no longer exit before
        # that check. Releasing them from the trace start raced it. Commands resolve through the
        # caller's scopes, so this Write-Host shadows the cmdlet only inside this call.
        function Write-Host {
            param([Parameter(Position = 0)][object]$Object)
            $text = "$Object"
            if ($text -like 'Recording for *' -or $text -like 'Exercise the application*') {
                Set-Content -LiteralPath $releaseFile -Value 'go'
            }
            Microsoft.PowerShell.Utility\Write-Host $text
        }
        $script:stubLeaf = Split-Path -Leaf $App
        $watch = [Diagnostics.Stopwatch]::StartNew()
        # 3>$null: the no-main-window warning is expected for a hidden stub.
        $r = Invoke-AppCapture -BinaryPath $App -StartTrace $StartTrace -StopTrace $traceStop -DurationSeconds $DurationSeconds `
            -WarmupSeconds $WarmupSeconds -IncludeStartup:$IncludeStartup -MainWindowTimeoutSeconds 0 -InteractiveTimeoutSeconds 60 `
            -WindowStyle Hidden 3>$null
        $r.Elapsed = $watch.Elapsed.TotalSeconds
        $r.Released = Test-Path -LiteralPath $releaseFile
        return $r
    }

    # Exits at once with 0, or with an error code: fails during warm-up, and the trace is never
    # started (no UAC prompt for a run that already failed). The warm-up is a wait for the app
    # to exit, so a long one costs nothing when the stub exits promptly; a 1 s warm-up failed
    # this check whenever a loaded machine took longer than that to run the stub (#1438).
    foreach ($case in @(@{ App = $exitNow0; Code = 0 }, @{ App = $exitNow3; Code = 3 })) {
        foreach ($duration in @(0, 30)) {
            $r = Invoke-StubCapture -App $case.App -DurationSeconds $duration -WarmupSeconds 30
            Assert-True ($r.Failure -like '*exited during warm-up*') "Immediate exit ($($case.Code), duration $duration) must fail in warm-up: $($r.Failure)"
            Assert-True ($r.ExitCode -eq $case.Code) "Exit code $($r.ExitCode), expected $($case.Code)"
            Assert-True ($script:events.Count -eq 0) "The trace must not start for an app that died in warm-up: $($script:events -join ', ')"
        }
    }

    # Exits with an error during a fixed window: fails, and the trace is stopped at once rather
    # than at the end of the 30 s window.
    $r = Invoke-StubCapture -App $exitLater3 -DurationSeconds 30 -WarmupSeconds 0
    Assert-True $r.Released "The stub was never released: Invoke-AppCapture no longer announces the capture window as Invoke-StubCapture expects ($($r.EndReason))"
    Assert-True ($r.Failure -like '*capture window*' -and $r.ExitCode -eq 3) "A crash mid-window must fail: $($r.Failure) (exit $($r.ExitCode))"
    Assert-True (($script:events -join ',') -eq 'start,stop(app running: False)') "Trace events: $($script:events -join ', ')"
    Assert-True ($r.Elapsed -lt 20) "The crash must end the capture promptly, took $($r.Elapsed) s"
    # Exiting with 0 before the window ends fails too: it was meant to be running.
    $r = Invoke-StubCapture -App $exitLater0 -DurationSeconds 30 -WarmupSeconds 0
    Assert-True ($r.Failure -like '*capture window*' -and $r.ExitCode -eq 0) "An early clean exit in a fixed window must fail: $($r.Failure)"

    # Still running at the end of the window: success; the trace stops before the app is closed.
    $r = Invoke-StubCapture -App $runForever -DurationSeconds 2 -WarmupSeconds 1
    Assert-True ($null -eq $r.Failure) "A full window must succeed: $($r.Failure)"
    Assert-True ($null -ne $r.ExitCode -and $r.EndReason -like '*closed by the script*') "The app must be closed by the script: $($r.EndReason)"
    Assert-True (($script:events -join ',') -eq 'start,stop(app running: True)') "The trace must stop while the app still runs: $($script:events -join ', ')"
    Assert-True ($null -ne $r.TraceStartUtc -and $r.TraceStartUtc -gt $r.StartUtc -and $r.WarmupSeconds -eq 1) 'The trace must start after the launch and warm-up'

    # Interactive (no -DurationSeconds): a normal close succeeds, an error exit fails.
    $r = Invoke-StubCapture -App $exitLater0 -DurationSeconds 0 -WarmupSeconds 0
    Assert-True ($null -eq $r.Failure -and $r.ExitCode -eq 0) "A clean interactive close must succeed: $($r.Failure)"
    $r = Invoke-StubCapture -App $exitLater3 -DurationSeconds 0 -WarmupSeconds 0
    Assert-True ($r.Failure -like '*code 3*') "An interactive error exit must fail: $($r.Failure)"
    Assert-True (($script:events -join ',') -eq 'start,stop(app running: False)') "Trace events: $($script:events -join ', ')"

    # -IncludeStartup starts the trace before the launch, so even an immediate exit is recorded.
    $r = Invoke-StubCapture -App $exitNow0 -DurationSeconds 0 -IncludeStartup
    Assert-True ($null -eq $r.Failure -and $r.WarmupSeconds -eq 0 -and $r.IncludeStartup) "IncludeStartup run: $($r.Failure)"
    Assert-True ($script:events[0] -eq 'start' -and $script:events.Count -eq 2) "IncludeStartup trace events: $($script:events -join ', ')"

    # The trace failing to start (UAC denied): the error propagates, nothing is stopped, and the
    # launched app is not left running.
    $failed = try { Invoke-StubCapture -App $runForever -DurationSeconds 30 -WarmupSeconds 0 -StartTrace { throw 'UAC denied' }; $null } catch { $_ }
    Assert-True ($null -ne $failed -and $failed.ToString() -like '*UAC denied*') "A failed trace start must throw: $failed"
    Assert-True ($script:events.Count -eq 0) "Nothing to stop after a failed start: $($script:events -join ', ')"
    Assert-True (-not (Test-StubRunning 'run-forever.cmd')) 'The app launched for a failed capture was left running'

    # The elevated-run role cannot run without -ElevatedTarget, which names and labels the capture
    # as elevated. Checked before elevation, so this needs no elevated token.
    $profileScript = Join-Path $PSScriptRoot 'profile-etw.ps1'

    # ── #1186 end to end: profile-etw.ps1 app -SkipTrace against the stubs ───────────────────
    # The whole orchestrator path minus WPR and UAC: a crashing app makes the script exit
    # nonzero and print no TRACE= (nor the dry run's success line); a running app succeeds.
    function Invoke-DryRun([string]$App, [string]$Name, [int]$DurationSeconds) {
        $out = & $hostExe -NoProfile -File $profileScript app -SkipTrace -SkipBuild -TargetPath $App -OutputDirectory (Join-Path $root 'dry') `
            -Timestamp $Name -DurationSeconds $DurationSeconds -WarmupSeconds 0 -MainWindowTimeoutSeconds 0 *>&1 | Out-String
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $out }
    }
    # The script's own dry-run trace start does not release the "exits later" stub, so release it
    # up front: it then exits with 3 during the warm-up or the window, and either is a failed capture.
    Set-Content -LiteralPath $releaseFile -Value 'go'
    foreach ($case in @(@{ App = $exitNow0; Name = 'now0' }, @{ App = $exitNow3; Name = 'now3' }, @{ App = $exitLater3; Name = 'later3' })) {
        $run = Invoke-DryRun -App $case.App -Name $case.Name -DurationSeconds 30
        Assert-True ($run.ExitCode -ne 0) "A crashing app ($($case.Name)) must fail the script: $($run.Output)"
        Assert-True ($run.Output -like '*Capture failed*') "Expected the capture-failed error ($($case.Name)): $($run.Output)"
        Assert-True ($run.Output -notmatch '(?m)^(TRACE|DRY_RUN)=') "A failed capture must not report success ($($case.Name)): $($run.Output)"
        Assert-True (Test-Path -LiteralPath (Join-Path $root "dry\etw-app-$($case.Name).manifest.json")) "The manifest must still be written ($($case.Name))"
    }
    $run = Invoke-DryRun -App $runForever -Name 'ok' -DurationSeconds 2
    Assert-True ($run.ExitCode -eq 0 -and $run.Output -match '(?m)^DRY_RUN=ok') "A running app must succeed: $($run.Output)"
    Assert-True ($run.Output -match 'Preset: win-release') "The default app preset must be win-release and logged: $($run.Output)"
    $okManifest = Get-Content -LiteralPath (Join-Path $root 'dry\etw-app-ok.manifest.json') -Raw | ConvertFrom-Json
    Assert-True ($okManifest.Preset -eq 'win-release' -and $null -eq $okManifest.Target.Failure) "Manifest: $($okManifest | ConvertTo-Json -Compress -Depth 5)"

    $unlabelled = & $hostExe -NoProfile -File $profileScript app -Role ElevatedRun -SkipBuild -OutputDirectory $root 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -ne 0 -and $unlabelled -like '*requires -ElevatedTarget*') "Expected the -ElevatedTarget refusal: $unlabelled"

    # ── Role lifecycle (#872): profile-etw.ps1 itself, against a stub wpr first on PATH ──────
    # The collector and the elevated-terminal refusal need an elevated token (GitHub's Windows
    # runners have one); the stub records every call and writes the trace file on -stop, so no
    # ETW session is started.
    $elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $elevated) {
        Write-Host 'SKIP: profile-etw.ps1 role lifecycle tests need an elevated token.'
    }
    else {
        $stubDir = Join-Path $root 'stub'
        New-Item -ItemType Directory -Path $stubDir | Out-Null
        $stubLog = Join-Path $root 'wpr-calls.log'
        Set-Content -LiteralPath (Join-Path $stubDir 'wpr.cmd') -Encoding ascii -Value @(
            '@echo off'
            'echo %*>>"%WPR_STUB_LOG%"'
            'if /i "%~1"=="-start" exit /b %WPR_STUB_START_EXIT%'
            # WPR_STUB_STOP_FAIL_ONCE names a flag file: while it exists, -stop deletes it and fails.
            'if /i "%~1"=="-stop" if defined WPR_STUB_STOP_FAIL_ONCE if exist "%WPR_STUB_STOP_FAIL_ONCE%" (del "%WPR_STUB_STOP_FAIL_ONCE%" & exit /b 7)'
            'if /i "%~1"=="-stop" echo stub-trace> "%~2"'
            'exit /b 0'
        )
        $savedPath = $env:PATH
        $env:PATH = "$stubDir;$savedPath"
        $env:WPR_STUB_LOG = $stubLog

        function Start-StubCollector([string]$Name, [int]$TimeoutSeconds, [int]$StartExit) {
            $env:WPR_STUB_START_EXIT = "$StartExit"
            $control = Join-Path $root "control-$Name"
            $out = Join-Path $root "out-$Name"
            New-Item -ItemType Directory -Path $control, $out | Out-Null
            $collectorArgs = @('-NoProfile', '-File', $profileScript, '-Mode', 'app', '-Role', 'Collector', '-ControlDirectory', $control, '-OutputDirectory', $out, '-Timestamp', $Name, '-CollectorTimeoutSeconds', "$TimeoutSeconds")
            $process = Start-Process -FilePath $hostExe -ArgumentList (ConvertTo-CommandLine $collectorArgs) -PassThru -WindowStyle Hidden
            $null = $process.Handle
            return [pscustomobject]@{ Process = $process; Control = $control; Trace = (Join-Path $out "etw-app-$Name.etl") }
        }
        function Get-StubCalls { if (Test-Path -LiteralPath $stubLog) { @(Get-Content -LiteralPath $stubLog) } else { @() } }

        try {
            # A normal run: starts WPR, signals it started, stops WPR on request, exits 0.
            Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
            $normal = Start-StubCollector -Name 'normal' -TimeoutSeconds 120 -StartExit 0
            Wait-CollectorMarker -Path (Join-Path $normal.Control 'collector-started.json') -TimeoutSeconds 60 -Collector $normal.Process
            Set-Content -LiteralPath (Join-Path $normal.Control 'stop-requested') -Value 'now'
            Assert-True ($normal.Process.WaitForExit(60000)) 'Collector did not exit after the stop request'
            Assert-True ($normal.Process.ExitCode -eq 0) "Collector exit code $($normal.Process.ExitCode)"
            Assert-True (Test-Path -LiteralPath (Join-Path $normal.Control 'collector-done.json')) 'No done marker'
            Assert-True (Test-Path -LiteralPath $normal.Trace) 'Trace not saved'
            $calls = Get-StubCalls
            $starts = @($calls | Where-Object { $_ -like '-start *' })
            $stops = @($calls | Where-Object { $_ -like '-stop *' })
            Assert-True ($starts.Count -eq 1 -and $stops.Count -eq 1) "Expected one start and one stop: $($calls -join ' | ')"
            Assert-True (($starts[0] -split ' ')[-1] -eq ($stops[0] -split ' ')[-1]) 'Start and stop used different instance names'

            # wpr -start fails: no stop is attempted, the error is recorded, the exit is nonzero.
            Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
            $failed = Start-StubCollector -Name 'startfail' -TimeoutSeconds 120 -StartExit 5
            Assert-True ($failed.Process.WaitForExit(60000)) 'Collector hung after a failed start'
            Assert-True ($failed.Process.ExitCode -ne 0) 'A failed wpr -start must fail the collector'
            Assert-True ((Get-CollectorErrorDetail -ControlDirectory $failed.Control) -like '*exit code 5*') "Recorded error: $(Get-CollectorErrorDetail -ControlDirectory $failed.Control)"
            Assert-True (@(Get-StubCalls | Where-Object { $_ -like '-stop *' }).Count -eq 0) 'Stopped a recording that never started'
            Assert-True (-not (Test-Path -LiteralPath (Join-Path $failed.Control 'collector-started.json'))) 'Signalled started after a failed start'

            # No stop request within the deadline: the trace is saved, but the capture fails.
            Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
            $late = Start-StubCollector -Name 'timeout' -TimeoutSeconds 3 -StartExit 0
            Assert-True ($late.Process.WaitForExit(60000)) 'Collector did not give up at its deadline'
            Assert-True ($late.Process.ExitCode -ne 0) 'A deadline must fail the capture'
            Assert-True ((Get-CollectorErrorDetail -ControlDirectory $late.Control) -like '*truncated*') "Recorded error: $(Get-CollectorErrorDetail -ControlDirectory $late.Control)"
            Assert-True (Test-Path -LiteralPath $late.Trace) 'The trace must still be saved at the deadline'

            # The first wpr -stop fails: the session is not left recording -- the stop is retried
            # once for the same instance, and the capture still fails.
            $failFlag = Join-Path $root 'stop-fail-once'
            $env:WPR_STUB_STOP_FAIL_ONCE = $failFlag
            function Assert-StopRetried([string]$What) {
                $calls = Get-StubCalls
                $starts = @($calls | Where-Object { $_ -like '-start *' })
                $stops = @($calls | Where-Object { $_ -like '-stop *' })
                Assert-True ($starts.Count -eq 1 -and $stops.Count -eq 2) "${What}: expected one start and a retried stop: $($calls -join ' | ')"
                Assert-True ((@($stops | ForEach-Object { ($_ -split ' ')[-1] }) | Select-Object -Unique) -eq ($starts[0] -split ' ')[-1]) "${What}: the retry must stop the same instance"
                Assert-True (-not (Test-Path -LiteralPath $failFlag)) "${What}: the failing stop was never attempted"
            }
            Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
            Set-Content -LiteralPath $failFlag -Value 'fail'
            $env:WPR_STUB_START_EXIT = '0'
            $retried = Start-StubCollector -Name 'stopfail' -TimeoutSeconds 120 -StartExit 0
            Wait-CollectorMarker -Path (Join-Path $retried.Control 'collector-started.json') -TimeoutSeconds 60 -Collector $retried.Process
            Set-Content -LiteralPath (Join-Path $retried.Control 'stop-requested') -Value 'now'
            Assert-True ($retried.Process.WaitForExit(60000)) 'Collector hung after a failed stop'
            Assert-True ($retried.Process.ExitCode -ne 0) 'A failed wpr -stop must fail the collector'
            Assert-StopRetried 'collector'
            Assert-True (Test-Path -LiteralPath $retried.Trace) 'The retried stop must save the trace'

            # The -ElevatedTarget child (ElevatedRun role) starts WPR through the app/bench
            # callbacks, after the warm-up, and has the same guarded final stop.
            function Invoke-ElevatedRun([string]$Mode, [string]$App, [string]$Name) {
                $appArgs = if ($Mode -eq 'app') { @('-DurationSeconds', '2', '-WarmupSeconds', '0', '-MainWindowTimeoutSeconds', '0') } else { @() }
                $out = & $hostExe -NoProfile -File $profileScript -Mode $Mode -Role ElevatedRun -ElevatedTarget -SkipBuild -TargetPath $App `
                    -OutputDirectory (Join-Path $root 'elevated') -Timestamp $Name @appArgs *>&1 | Out-String
                return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $out }
            }
            Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
            $ok = Invoke-ElevatedRun -Mode app -App $runForever -Name 'er-ok'
            Assert-True ($ok.ExitCode -eq 0 -and $ok.Output -match 'ETW_TRACE=') "Elevated app run: $($ok.Output)"
            $okCalls = @(Get-StubCalls)
            Assert-True (@($okCalls | Where-Object { $_ -like '-start *' }).Count -eq 1 -and @($okCalls | Where-Object { $_ -like '-stop *' }).Count -eq 1) "Elevated app run calls: $($okCalls -join ' | ')"
            $okManifest = Get-Content -LiteralPath (Join-Path $root 'elevated\etw-app-elevated-er-ok.manifest.json') -Raw | ConvertFrom-Json
            Assert-True ([datetime]$okManifest.Target.TraceStartUtc -gt [datetime]$okManifest.Target.StartUtc) 'The elevated run must start the trace after launching the app'

            foreach ($case in @(@{ Mode = 'app'; App = $runForever }, @{ Mode = 'bench'; App = $exitNow0 })) {
                Remove-Item -LiteralPath $stubLog -ErrorAction SilentlyContinue
                Set-Content -LiteralPath $failFlag -Value 'fail'
                $run = Invoke-ElevatedRun -Mode $case.Mode -App $case.App -Name "er-stopfail-$($case.Mode)"
                Assert-True ($run.ExitCode -ne 0 -and $run.Output -like '*exit code 7*') "Elevated $($case.Mode) run with a failed stop must fail: $($run.Output)"
                Assert-True ($run.Output -notmatch 'ETW_TRACE=') "Elevated $($case.Mode) run with a failed stop must not report a trace"
                Assert-StopRetried "elevated $($case.Mode)"
            }
            Assert-True (-not (Test-StubRunning 'run-forever.cmd')) 'The elevated run left the app running'
        }
        finally {
            $env:PATH = $savedPath
            Remove-Item Env:WPR_STUB_LOG, Env:WPR_STUB_START_EXIT, Env:WPR_STUB_STOP_FAIL_ONCE -ErrorAction SilentlyContinue
        }

        # From an elevated terminal the orchestrator refuses, before building or prompting.
        $refusal = & $hostExe -NoProfile -File $profileScript app -SkipBuild -OutputDirectory $root 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -ne 0 -and $refusal -like '*normal (non-elevated) terminal*') "Expected the elevated-terminal refusal: $refusal"
    }
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'profile-etw tests passed'
