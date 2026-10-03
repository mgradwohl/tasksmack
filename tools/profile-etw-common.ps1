# Helpers for profile-etw.ps1's app and bench modes, dot-sourced by it and by
# test-profile-etw.ps1. Kept free of WPR calls so they can be tested without an ETW session.

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
