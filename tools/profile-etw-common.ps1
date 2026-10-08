# Helpers for profile-etw.ps1's app and bench modes, dot-sourced by it and by
# test-profile-etw.ps1. Kept free of WPR calls so they can be tested without an ETW session.

# Read-CMakeCache and ConvertTo-CMakeUpper, shared with bench.ps1 (#1479).
. (Join-Path $PSScriptRoot 'cmake-cache.ps1')

if (-not ('TaskSmackProfile.TokenInfo' -as [type])) {
    Add-Type -Namespace TaskSmackProfile -Name TokenInfo -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern System.IntPtr OpenProcess(uint access, bool inherit, int pid);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(System.IntPtr handle);
[System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
public static extern bool OpenProcessToken(System.IntPtr process, uint access, out System.IntPtr token);
[System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
public static extern bool GetTokenInformation(System.IntPtr token, int infoClass, System.IntPtr info, int length, out int returned);
[System.Runtime.InteropServices.DllImport("advapi32.dll")]
public static extern System.IntPtr GetSidSubAuthorityCount(System.IntPtr sid);
[System.Runtime.InteropServices.DllImport("advapi32.dll")]
public static extern System.IntPtr GetSidSubAuthority(System.IntPtr sid, uint index);

// The mandatory-label RID of a process's token (e.g. 0x2000 Medium, 0x3000 High), or -1.
public static long IntegrityRid(int pid)
{
    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const uint TOKEN_QUERY = 0x0008;
    const int TokenIntegrityLevel = 25;
    System.IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
    if (process == System.IntPtr.Zero) { return -1; }
    try
    {
        System.IntPtr token;
        if (!OpenProcessToken(process, TOKEN_QUERY, out token)) { return -1; }
        try
        {
            int needed;
            GetTokenInformation(token, TokenIntegrityLevel, System.IntPtr.Zero, 0, out needed);
            if (needed <= 0) { return -1; }
            System.IntPtr buffer = System.Runtime.InteropServices.Marshal.AllocHGlobal(needed);
            try
            {
                if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, needed, out needed)) { return -1; }
                // TOKEN_MANDATORY_LABEL starts with SID_AND_ATTRIBUTES, whose first field is the SID pointer.
                System.IntPtr sid = System.Runtime.InteropServices.Marshal.ReadIntPtr(buffer);
                int count = System.Runtime.InteropServices.Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                if (count <= 0) { return -1; }
                return (uint)System.Runtime.InteropServices.Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buffer); }
        }
        finally { CloseHandle(token); }
    }
    finally { CloseHandle(process); }
}
'@
}

function ConvertTo-IntegrityLevelName {
    # Names for the well-known mandatory-label RIDs; anything else is reported as hex so it is
    # never mislabelled.
    param([long]$Rid)
    switch ($Rid) {
        -1 { return 'Unknown' }
        0x0000 { return 'Untrusted' }
        0x1000 { return 'Low' }
        0x2000 { return 'Medium' }
        0x2100 { return 'MediumPlus' }
        0x3000 { return 'High' }
        0x4000 { return 'System' }
        default { return ('0x{0:X}' -f $Rid) }
    }
}

function Get-ProcessIntegrityLevel {
    # The *measured* integrity level of a running process (#872): what the capture actually
    # profiled, not what the launcher assumed it would be.
    param([Parameter(Mandatory = $true)][int]$ProcessId)
    return ConvertTo-IntegrityLevelName ([TaskSmackProfile.TokenInfo]::IntegrityRid($ProcessId))
}

function Resolve-BenchmarkFilterMatches {
    # Classify the output of `TaskSmackBenchmarks.exe --benchmark_filter=<f> --benchmark_list_tests=true`
    # (#874), mirroring profile-perf.sh: no match is an error -- the capture would only measure
    # benchmark startup and shutdown -- and several matches are a warning, because Google
    # Benchmark fills the same minimum time per benchmark, so a cheap benchmark is looped more and
    # gets as many samples as an expensive one regardless of its real-world importance.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$ListOutput,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    $names = @($ListOutput | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($names.Count -eq 0) {
        return [pscustomobject]@{
            Severity = 'error'
            Names    = $names
            Message  = "Benchmark filter '$Filter' matches no benchmarks. Capturing would only measure benchmark startup/shutdown noise. Run TaskSmackBenchmarks.exe --benchmark_list_tests=true to see valid names."
        }
    }
    if ($names.Count -gt 1) {
        $list = ($names | ForEach-Object { "  - $_" }) -join [Environment]::NewLine
        return [pscustomobject]@{
            Severity = 'warning'
            Names    = $names
            Message  = "Benchmark filter '$Filter' matches $($names.Count) benchmarks:$([Environment]::NewLine)$list$([Environment]::NewLine)Profiling several benchmarks together can produce misleading relative percentages when their per-call costs differ a lot: each gets the same minimum time, so cheap ones are looped more. For hotspot attribution on one function, pass a -BenchmarkFilter matching exactly one benchmark."
        }
    }
    return [pscustomobject]@{ Severity = 'ok'; Names = $names; Message = "Benchmark filter '$Filter' matches $($names[0])." }
}

function Get-BenchmarkFilterMatches {
    # Lists the benchmarks a filter selects by asking the benchmark binary itself, so the count is
    # exactly what the capture would run.
    param(
        [Parameter(Mandatory = $true)][string]$BinaryPath,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    # Names come from stdout only. Google Benchmark reports a non-matching filter on stderr
    # ("Failed to match any benchmarks against regex: ...") and still exits 0, so merging the two
    # streams would count that message as a benchmark name.
    $stderrPath = [IO.Path]::GetTempFileName()
    try {
        $output = & $BinaryPath "--benchmark_filter=$Filter" '--benchmark_list_tests=true' 2>$stderrPath
        $exitCode = $LASTEXITCODE
        $stderrText = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
    }
    finally {
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
    if ($exitCode -ne 0) {
        throw "Failed to list benchmarks from $BinaryPath (exit $exitCode): $(@($output) -join ' ') $stderrText".Trim()
    }
    return Resolve-BenchmarkFilterMatches -ListOutput @($output | ForEach-Object { "$_" }) -Filter $Filter
}

function Resolve-CaptureOutputDirectory {
    # Where app/bench artifacts go, as an absolute path: the elevated children start in the repo
    # root, so a relative -OutputDirectory must be resolved against the caller's location before it
    # is passed on, or they would write somewhere else.
    param([AllowEmptyString()][string]$OutputDirectory, [Parameter(Mandatory = $true)][string]$RepoRoot)
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { return (Join-Path $RepoRoot 'perf-data') }
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
}

function Get-UnfinishedTargetReason {
    # Why an app was still running when the script stopped waiting for it. A fixed-duration run
    # waits 30 s after its forced stop; only an interactive run waits the 4 hours.
    param([int]$DurationSeconds)
    if ($DurationSeconds -gt 0) { return 'still running 30 s after the forced stop at -DurationSeconds' }
    return 'still running after the 4-hour wait'
}

function Assert-CollectorOutcome {
    # Fails the capture once the collector has been told to stop and its status is known. An
    # error from waiting for the collector or running the target is rethrown, with any collector
    # failure added so it is never hidden behind that error; otherwise a collector failure alone
    # is thrown.
    param([System.Management.Automation.ErrorRecord]$PendingError, [string]$CollectorFailure)
    if ($null -ne $PendingError) {
        if ($CollectorFailure) {
            throw [System.Exception]::new("$($PendingError.Exception.Message) The ETW collector also failed: $CollectorFailure", $PendingError.Exception)
        }
        throw $PendingError
    }
    if ($CollectorFailure) { throw $CollectorFailure }
}

function Get-CollectorErrorDetail {
    # The error the collector recorded in the control directory, as " Collector error: ...", or
    # an empty string. Its console window closes when it exits, so this is the only place it
    # survives -- at startup or after it was told to stop.
    param([Parameter(Mandatory = $true)][string]$ControlDirectory)
    $errorFile = Join-Path $ControlDirectory 'collector-error.txt'
    if (Test-Path -LiteralPath $errorFile) { return " Collector error: $((Get-Content -LiteralPath $errorFile -Raw).Trim())" }
    return ''
}

function ConvertTo-QuotedArgument {
    # One argument quoted by the Windows C runtime's command-line rules (CommandLineToArgvW), so
    # spaces, quotes and trailing backslashes survive: a backslash run is doubled before a quote
    # and at the end, and an embedded quote is escaped.
    param([AllowEmptyString()][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new('"')
    $backslashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $backslashes++; continue }
        if ($ch -eq '"') { [void]$builder.Append('\', 2 * $backslashes + 1).Append('"') }
        else { [void]$builder.Append('\', $backslashes).Append($ch) }
        $backslashes = 0
    }
    [void]$builder.Append('\', 2 * $backslashes).Append('"')
    return $builder.ToString()
}

function ConvertTo-CommandLine {
    # Start-Process -ArgumentList joins an array with spaces and no quoting, so a path containing
    # a space (a checkout under "C:\Users\First Last\...") was split into two arguments. Pass it
    # this single, correctly quoted string instead.
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Arguments)
    return (@($Arguments | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join ' ')
}

function Wait-CollectorMarker {
    # Waits for the elevated collector to write a marker file, failing early if the collector
    # process has already exited (UAC denied, or wpr -start failed) instead of waiting out the
    # whole timeout.
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [System.Diagnostics.Process]$Collector,
        [string]$What = 'collector'
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path -LiteralPath $Path)) {
        if ($Collector -and $Collector.HasExited) {
            $detail = Get-CollectorErrorDetail -ControlDirectory (Split-Path -Parent $Path)
            if (-not $detail) { $detail = ' The UAC prompt may have been denied, or wpr failed; see the collector log.' }
            throw "The elevated $What exited (code $($Collector.ExitCode)) before writing $Path.$detail"
        }
        if ((Get-Date) -gt $deadline) {
            throw "Timed out after $TimeoutSeconds s waiting for the elevated $What to write $Path."
        }
        Start-Sleep -Milliseconds 250
    }
}

function Get-PresetBuildFlags {
    # The compile flags a preset's build tree was configured with, read from its CMakeCache.txt,
    # so a capture records which binary it measured (#1186): the preset name alone does not say
    # (win-release and win-optimized are both "Release", with very different flags).
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    $flags = [ordered]@{
        BuildType      = $null
        CxxFlags       = $null
        CxxConfigFlags = $null
        Ipo            = $null
        Source         = $cachePath
    }
    if (-not (Test-Path -LiteralPath $cachePath)) {
        $flags.Source = "no CMakeCache.txt at $cachePath"
        return $flags
    }
    # Parsed by CMake's own rules, so a custom build type's CMAKE_CXX_FLAGS_ASAN-UBSAN, and quoted
    # keys, are read too (#1479).
    $cache = Read-CMakeCache -Path $cachePath
    $flags.BuildType = $cache['CMAKE_BUILD_TYPE']
    $flags.CxxFlags = $cache['CMAKE_CXX_FLAGS']
    if ($flags.BuildType) { $flags.CxxConfigFlags = $cache["CMAKE_CXX_FLAGS_$(ConvertTo-CMakeUpper $flags.BuildType)"] }
    # A preset can cache CMAKE_INTERPROCEDURAL_OPTIMIZATION itself; otherwise CompilerOptions.cmake sets
    # it as a plain variable from the TASKSMACK_ENABLE_IPO option (ON by default), which is cached --
    # so a normal win-release cache has LTO on with no CMAKE_INTERPROCEDURAL_OPTIMIZATION entry (#1372 review).
    $flags.Ipo = if ($cache.ContainsKey('CMAKE_INTERPROCEDURAL_OPTIMIZATION')) { $cache['CMAKE_INTERPROCEDURAL_OPTIMIZATION'] }
    elseif ($cache.ContainsKey('TASKSMACK_ENABLE_IPO')) { "$($cache['TASKSMACK_ENABLE_IPO']) (TASKSMACK_ENABLE_IPO, where the compiler supports it)" }
    else { 'unknown' }
    return $flags
}

function Format-PresetBuildFlags {
    # One line naming the preset and the flags it builds with, logged at the start of a capture.
    param([Parameter(Mandatory = $true)][string]$Preset, [Parameter(Mandatory = $true)]$Flags)
    if (-not $Flags.BuildType) { return "Preset: $Preset (compile flags unknown: $($Flags.Source))" }
    $cxx = "$($Flags.CxxFlags) $($Flags.CxxConfigFlags)".Trim()
    return "Preset: $Preset (CMAKE_BUILD_TYPE=$($Flags.BuildType); CXX flags: '$cxx'; IPO/LTO: $($Flags.Ipo))"
}

function Wait-AppMainWindow {
    # True once the process has a main (visible top-level) window; false if it exits first or no
    # window appears within the timeout.
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process, [int]$TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not $Process.HasExited) {
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) { return $true }
        if ((Get-Date) -ge $deadline) { return $false }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Invoke-AppCapture {
    # The app-mode lifecycle (#1186). Launches the app and lets it warm up -- its main window
    # exists, then -WarmupSeconds more -- before the trace starts, so font, theme and
    # first-enumeration costs are not in the profile (-IncludeStartup records them deliberately).
    # With -DurationSeconds the trace is stopped before the script closes the app, so the forced
    # shutdown is not in it either.
    #
    # A run that does not cover what was asked fails: the app exiting during warm-up or while the
    # trace starts; exiting before a fixed -DurationSeconds window ends, whatever its exit code
    # (it was supposed to be still running); or, interactively, being closed with a nonzero exit
    # code. The failure is returned in .Failure rather than thrown, so the caller can still write
    # the manifest before failing the capture.
    #
    # StartTrace/StopTrace start and stop the recording (WPR directly, or the elevated collector).
    # StopTrace runs exactly once if StartTrace returned, also when an error is thrown, and as soon
    # as the app exits early, so a crash does not leave the recording running.
    param(
        [Parameter(Mandatory = $true)][string]$BinaryPath,
        [Parameter(Mandatory = $true)][scriptblock]$StartTrace,
        [Parameter(Mandatory = $true)][scriptblock]$StopTrace,
        [int]$DurationSeconds = 0,
        [int]$WarmupSeconds = 5,
        [switch]$IncludeStartup,
        [int]$MainWindowTimeoutSeconds = 60,
        [int]$InteractiveTimeoutSeconds = 14400,
        [ValidateSet('Normal', 'Hidden')][string]$WindowStyle = 'Normal'
    )
    $result = [ordered]@{
        Binary         = $BinaryPath
        Pid            = $null
        IntegrityLevel = 'Unknown'
        ExitCode       = $null
        EndReason      = $null
        Failure        = $null
        IncludeStartup = [bool]$IncludeStartup
        WarmupSeconds  = if ($IncludeStartup) { 0 } else { $WarmupSeconds }
        MainWindowSeen = $null
        StartUtc       = (Get-Date).ToUniversalTime().ToString('o')
        TraceStartUtc  = $null
        TraceStopUtc   = $null
        EndUtc         = $null
    }
    $proc = $null
    $traceRunning = $false
    $completed = $false
    # Dot-sourced (. $stopTraceNow), so it updates $traceRunning in this scope.
    $stopTraceNow = {
        if ($traceRunning) {
            $traceRunning = $false
            & $StopTrace | Out-Host
            $result.TraceStopUtc = (Get-Date).ToUniversalTime().ToString('o')
        }
    }
    $startTraceNow = {
        & $StartTrace | Out-Host
        $traceRunning = $true
        $result.TraceStartUtc = (Get-Date).ToUniversalTime().ToString('o')
    }
    try {
        if ($IncludeStartup) { . $startTraceNow }
        $proc = Start-Process -FilePath $BinaryPath -PassThru -WindowStyle $WindowStyle
        $null = $proc.Handle # keep a handle so ExitCode is available after exit
        $result.Pid = $proc.Id
        $result.IntegrityLevel = Get-ProcessIntegrityLevel -ProcessId $proc.Id
        Write-Host "Launched app PID: $($proc.Id) (integrity: $($result.IntegrityLevel))"

        if (-not $IncludeStartup) {
            Write-Host "Warm-up: waiting for the main window (up to $MainWindowTimeoutSeconds s), then $WarmupSeconds s more, before recording starts (-IncludeStartup records startup too)."
            $result.MainWindowSeen = Wait-AppMainWindow -Process $proc -TimeoutSeconds $MainWindowTimeoutSeconds
            if (-not $proc.HasExited -and -not $result.MainWindowSeen) {
                Write-Warning "No main window appeared within $MainWindowTimeoutSeconds s; starting the trace after the warm-up anyway."
            }
            if (-not $proc.HasExited -and $WarmupSeconds -gt 0) { $null = $proc.WaitForExit($WarmupSeconds * 1000) }
            if ($proc.HasExited) {
                $result.EndReason = 'exited during warm-up, before the trace started'
                $result.Failure = "The app exited during warm-up (exit code $($proc.ExitCode)) before recording started, so nothing was captured. It most likely crashed at startup."
            }
            else {
                . $startTraceNow
                if ($proc.HasExited) {
                    $result.EndReason = 'exited while the trace was starting'
                    $result.Failure = "The app exited (exit code $($proc.ExitCode)) while the trace was starting, so the trace does not cover a running app. It most likely crashed."
                }
            }
        }

        if (-not $result.Failure) {
            if ($DurationSeconds -gt 0) {
                Write-Host "Recording for $DurationSeconds second(s), then closing the app automatically."
                $windowStart = Get-Date
                if ($proc.WaitForExit($DurationSeconds * 1000)) {
                    $elapsed = [int]((Get-Date) - $windowStart).TotalSeconds
                    $result.EndReason = 'exited during the -DurationSeconds capture window'
                    $result.Failure = "The app exited (exit code $($proc.ExitCode)) after $elapsed s of the $DurationSeconds s capture window, so the trace does not cover the requested window. It most likely crashed."
                }
                else {
                    # The expected path: the app is still running. Stop recording first, so the
                    # forced shutdown is not in the trace, then close it.
                    . $stopTraceNow
                    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                    # Stop-Process returns before the process is gone; wait, so its status is final.
                    if ($proc.WaitForExit(30000)) {
                        $result.EndReason = 'closed by the script after -DurationSeconds (the exit code is from that forced stop)'
                    }
                }
            }
            else {
                Write-Host 'Exercise the application, then close it to finish the trace.'
                if ($proc.WaitForExit($InteractiveTimeoutSeconds * 1000)) {
                    $result.EndReason = 'exited'
                    if ($proc.ExitCode -ne 0) {
                        $result.Failure = "The app exited with code $($proc.ExitCode) (0x$('{0:X8}' -f $proc.ExitCode)), not 0, so the capture ends in a crash or error exit rather than a normal close."
                    }
                }
            }
        }
        $completed = $true
    }
    finally {
        . $stopTraceNow
        # On an error (e.g. the trace could not be started), do not leave behind an app this run
        # launched: nothing is recording it.
        if (-not $completed -and $null -ne $proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            $null = $proc.WaitForExit(30000)
        }
    }
    if ($proc.HasExited) { $result.ExitCode = $proc.ExitCode }
    elseif (-not $result.EndReason) { $result.EndReason = Get-UnfinishedTargetReason -DurationSeconds $DurationSeconds }
    $result.EndUtc = (Get-Date).ToUniversalTime().ToString('o')
    return $result
}
