# Tests for tools/bench.ps1 against a stub benchmark binary (#1423): a failing or crashing
# benchmark fails the script, partial output is redacted or deleted, output that cannot be redacted
# after exit 0 is deleted, native-command error promotion does not bypass that, and an extra
# --benchmark_out is refused. Registered in CTest on Windows with PowerShell 7; can also run
# directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function ConvertTo-CommandLine {
    # Start-Process takes one command line: quote each argument (none here ends in a backslash).
    param([string[]]$Arguments)
    return (@($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' ')
}

$benchScript = Join-Path $PSScriptRoot 'bench.ps1'
$hostExe = (Get-Process -Id $PID).Path
# Under a non-ASCII directory name, so the paths the script and the stub wrapper handle hold
# Unicode characters.
$unicode = "$([char]0x00FC)n$([char]0x00EF)c$([char]0x00F8)d$([char]0x00E9)"
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-bench-tests-$unicode-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # The stub benchmark binary: an ASCII batch file running a small script that writes Google
    # Benchmark-shaped JSON (holding this machine's real host name and a full executable path, as
    # the real binary does) to the last --benchmark_out, then exits with STUB_EXIT.
    # STUB_OUTPUT=partial writes truncated JSON, as a crash mid-run does. The script is Python when
    # an interpreter is at hand (CTest passes CMake's as TASKSMACK_TEST_PYTHON): it starts in a
    # fraction of a PowerShell start-up. Otherwise it is PowerShell.
    $binDir = Join-Path $root 'bin'
    New-Item -ItemType Directory -Path $binDir | Out-Null
    $python = if ($env:TASKSMACK_TEST_PYTHON -and (Test-Path -LiteralPath $env:TASKSMACK_TEST_PYTHON)) { $env:TASKSMACK_TEST_PYTHON }
    else { @(Get-Command python3, python -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1 -ExpandProperty Source) }
    if ($python) {
        $stubScript = Join-Path $binDir 'stub.py'
        Set-Content -LiteralPath $stubScript -Encoding utf8 -Value @'
import json, os, socket, sys
out = [a for a in sys.argv[1:] if a.startswith("--benchmark_out=")][-1][len("--benchmark_out="):]
body = json.dumps({
    "context": {"host_name": os.environ.get("COMPUTERNAME") or socket.gethostname(), "executable": "C:\\some\\dir\\TaskSmackBenchmarks.exe"},
    "benchmarks": [{"name": "BM_X_median", "run_name": "BM_X", "aggregate_name": "median", "real_time": 11.0, "time_unit": "ns"}],
}, indent=2)
if os.environ.get("STUB_OUTPUT") == "partial":
    body = body[:60]
with open(out, "w", encoding="utf-8") as stream:
    stream.write(body)
sys.exit(int(os.environ.get("STUB_EXIT") or 0))
'@
        $stubHost = $python
        $stubHostArgs = ''
    }
    else {
        $stubScript = Join-Path $binDir 'stub.ps1'
        Set-Content -LiteralPath $stubScript -Encoding utf8 -Value @'
$out = [Environment]::GetCommandLineArgs() | Where-Object { $_ -like '--benchmark_out=*' } | Select-Object -Last 1
$out = $out.Substring('--benchmark_out='.Length)
$body = [ordered]@{
    context    = [ordered]@{ host_name = [Environment]::MachineName; executable = 'C:\some\dir\TaskSmackBenchmarks.exe' }
    benchmarks = @([ordered]@{ name = 'BM_X_median'; run_name = 'BM_X'; aggregate_name = 'median'; real_time = 11.0; time_unit = 'ns' })
} | ConvertTo-Json -Depth 5
if ($env:STUB_OUTPUT -eq 'partial') { $body = $body.Substring(0, 60) }
Set-Content -LiteralPath $out -Value $body -Encoding utf8
exit [int]$env:STUB_EXIT
'@
        $stubHost = $hostExe
        $stubHostArgs = '-NoProfile -File'
    }
    $stub = Join-Path $binDir 'TaskSmackBenchmarks.cmd'
    # The wrapper is ASCII (cmd.exe reads it in the console code page), so the interpreter and stub
    # script paths, which can hold Unicode characters, come from inherited environment variables.
    Set-Content -LiteralPath $stub -Encoding ascii -Value "@`"%TASKSMACK_STUB_HOST%`" %TASKSMACK_STUB_HOST_ARGS% `"%TASKSMACK_STUB_SCRIPT%`" %*`r`n@exit /b %ERRORLEVEL%"

    function Use-StubEnvironment {
        # Run $Body with the stub's variables set, putting the previous values back afterwards.
        param([hashtable]$Variables, [scriptblock]$Body)
        $all = @{ TASKSMACK_STUB_HOST = $stubHost; TASKSMACK_STUB_HOST_ARGS = $stubHostArgs; TASKSMACK_STUB_SCRIPT = $stubScript }
        foreach ($name in $Variables.Keys) { $all[$name] = $Variables[$name] }
        $saved = @{}
        foreach ($name in $all.Keys) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $all[$name])
        }
        try { & $Body }
        finally {
            foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        }
    }
    function Get-Results([string]$Name) {
        return , @(Get-ChildItem -LiteralPath (Join-Path $root $Name) -Filter '*.json' -ErrorAction SilentlyContinue)
    }

    # Every PowerShell start costs over a second, so the runs share processes and run side by side:
    #  - two `pwsh -File` runs, a failing and a successful stub, for the script's own exit code;
    #  - one PowerShell host running the other scenarios one after another with `& bench.ps1`, the
    #    way a PowerShell session calls it: the script's top level runs in full each time, and a
    #    failure is the terminating error it throws.
    $quote = { param([string]$s) "'" + $s.Replace("'", "''") + "'" }
    $scenarios = @(
        # A crash mid-run, from a session with native-command error promotion on: the exit code
        # must still be read, and the truncated output deleted.
        @{ Name = 'crashed'; StubExit = '5'; StubOutput = 'partial'; Promote = $true
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'crashed')) '--benchmark_filter=BM_X'" }
        # Exit 0 with output that cannot be redacted.
        @{ Name = 'garbled'; StubExit = '0'; StubOutput = 'partial'
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'garbled')) '--benchmark_filter=BM_X'" }
        # An extra --benchmark_out / --benchmark_out_format, refused before the stub runs. A
        # relative path, run from $root, so a file written anyway would be found.
        @{ Name = 'override-out'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-out')) '--benchmark_filter=BM_X' '--benchmark_out=elsewhere.json'" }
        @{ Name = 'override-format'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-format')) '--benchmark_filter=BM_X' '--benchmark_out_format=csv'" }
    )
    $scenarioFile = Join-Path $root 'scenarios.json'
    $scenarioResults = Join-Path $root 'scenario-results.json'
    $scenarios | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $scenarioFile -Encoding utf8
    $driver = Join-Path $root 'driver.ps1'
    Set-Content -LiteralPath $driver -Encoding utf8 -Value @'
param([string]$Scenarios, [string]$Results)
$ErrorActionPreference = 'Stop'
$outcomes = foreach ($scenario in (Get-Content -LiteralPath $Scenarios -Raw | ConvertFrom-Json)) {
    $env:STUB_EXIT = $scenario.StubExit
    $env:STUB_OUTPUT = $scenario.StubOutput
    $PSNativeCommandUseErrorActionPreference = [bool]($scenario.PSObject.Properties['Promote'] -and $scenario.Promote)
    $here = Get-Location
    if ($scenario.PSObject.Properties['Cwd']) { Set-Location -LiteralPath $scenario.Cwd }
    $threw = $false
    try { $log = & ([scriptblock]::Create($scenario.Command)) *>&1 | Out-String }
    catch { $threw = $true; $log = "$($_ | Out-String)" }
    finally { Set-Location $here }
    [pscustomobject]@{ Name = $scenario.Name; Threw = $threw; Log = $log }
}
$outcomes | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Results -Encoding utf8
'@

    $processes = @{}
    foreach ($run in @(@{ Name = 'failed'; Exit = '3' }, @{ Name = 'ok'; Exit = '0' })) {
        Use-StubEnvironment @{ STUB_EXIT = $run.Exit; STUB_OUTPUT = 'full' } {
            $processes[$run.Name] = Start-Process -FilePath $hostExe -PassThru -WindowStyle Hidden `
                -RedirectStandardOutput (Join-Path $root "$($run.Name).log") -RedirectStandardError (Join-Path $root "$($run.Name).err") `
                -ArgumentList (ConvertTo-CommandLine @('-NoProfile', '-File', $benchScript, 'fake-preset', '-BenchmarkBinary', $stub, '-OutputDirectory', (Join-Path $root $run.Name), '--benchmark_filter=BM_X'))
        }
    }
    Use-StubEnvironment @{} {
        $processes['shared'] = Start-Process -FilePath $hostExe -PassThru -WindowStyle Hidden -ArgumentList (ConvertTo-CommandLine @('-NoProfile', '-File', $driver, $scenarioFile, $scenarioResults))
    }
    foreach ($process in $processes.Values) { $null = $process.Handle }  # keeps ExitCode readable after exit
    foreach ($process in $processes.Values) { $process.WaitForExit() }
    function Get-ProcessLog([string]$Name) { (Get-Content -LiteralPath (Join-Path $root "$Name.log") -Raw) + (Get-Content -LiteralPath (Join-Path $root "$Name.err") -Raw) }
    $outcomes = @{}
    foreach ($outcome in (Get-Content -LiteralPath $scenarioResults -Raw -Encoding utf8 | ConvertFrom-Json)) { $outcomes[$outcome.Name] = $outcome }
    Assert-True ($outcomes.Count -eq $scenarios.Count) "The shared host ran $($outcomes.Count) of $($scenarios.Count) scenarios"

    # ── A benchmark binary that fails makes the script fail ────────────────────────────────
    $failedLog = Get-ProcessLog 'failed'
    Assert-True ($processes['failed'].ExitCode -ne 0) "bench.ps1 reported success for a benchmark that exited 3:`n$failedLog"
    Assert-True ($failedLog -like '*exited with code 3*') "The failure must name the benchmark's exit code:`n$failedLog"
    Assert-True ($failedLog -notlike '*Results written to*') "A failed run must not be reported as written:`n$failedLog"
    # The parseable partial result is kept, redacted.
    $failed = Get-Results 'failed'
    Assert-True ($failed.Count -eq 1) 'A parseable partial result must be kept (redacted)'
    Assert-True ((Get-Content -LiteralPath $failed[0].FullName -Raw | ConvertFrom-Json).context.host_name -eq 'redacted') 'A failed run''s output must still be redacted'

    # ── A crash leaves truncated JSON: deleted, and the failure still reported (with promotion) ─
    $crashed = $outcomes['crashed']
    Assert-True $crashed.Threw "bench.ps1 reported success for a crashed benchmark:`n$($crashed.Log)"
    Assert-True ($crashed.Log -like '*exited with code 5*') "Native error promotion bypassed the exit-code handling:`n$($crashed.Log)"
    Assert-True ((Get-Results 'crashed').Count -eq 0) 'Unparseable partial output must be deleted'

    # ── Exit 0 with output that cannot be redacted: deleted, and the script fails ───────────
    $garbled = $outcomes['garbled']
    Assert-True $garbled.Threw "Unredactable output was reported as a success:`n$($garbled.Log)"
    Assert-True ($garbled.Log -like '*could not be redacted*') "The failure must say why:`n$($garbled.Log)"
    Assert-True ((Get-Results 'garbled').Count -eq 0) 'Unredactable output must be deleted'

    # ── An extra --benchmark_out / --benchmark_out_format is refused before launch ───────────
    foreach ($name in @('override-out', 'override-format')) {
        $refused = $outcomes[$name]
        Assert-True $refused.Threw "bench.ps1 accepted the override ($name):`n$($refused.Log)"
        Assert-True ($refused.Log -like '*-OutputDirectory*') "The refusal must point to -OutputDirectory ($name):`n$($refused.Log)"
        Assert-True ((Get-Results $name).Count -eq 0) "Nothing may be written ($name)"
    }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $root 'elsewhere.json'))) 'The benchmark ran and wrote elsewhere.json'

    # ── A successful run exits 0 with its output redacted ──────────────────────────────────
    $okLog = Get-ProcessLog 'ok'
    Assert-True ($processes['ok'].ExitCode -eq 0) "A successful run must exit 0:`n$okLog"
    Assert-True ($okLog -like '*Results written to*') "A successful run must report its results:`n$okLog"
    $ok = Get-Results 'ok'
    Assert-True ($ok.Count -eq 1) "Expected one result:`n$okLog"
    $okJson = Get-Content -LiteralPath $ok[0].FullName -Raw | ConvertFrom-Json
    Assert-True ($okJson.context.host_name -eq 'redacted' -and $okJson.context.executable -eq 'TaskSmackBenchmarks.exe') 'Result context must be redacted'

    Write-Host 'bench.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
