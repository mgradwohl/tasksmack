# Tests for tools/bench.ps1 against a stub benchmark binary: a failing run fails the script
# (#1423), and every run writes a provenance manifest with no host or user name (#1424).
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function ConvertTo-RaceCommandLine {
    # Start-Process takes one command line: quote each argument (none here ends in a backslash).
    param([string[]]$Arguments)
    return (@($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' ')
}

function Find-IdentityLeaks {
    # Every string value (never an object key) of a decoded manifest that still holds a token
    # (user or host name) standing alone between the identity pass's separators, or contains one
    # of the paths. Tokens under 3 characters are not checked, as the identity pass leaves them.
    param($Value, [string[]]$Tokens, [string[]]$Paths)
    $leaks = [System.Collections.Generic.List[string]]::new()
    $separated = '\s/\\"''=:,;'
    $visit = {
        param($Node)
        if ($null -eq $Node) { return }
        if ($Node -is [string]) {
            foreach ($token in $Tokens) {
                if ($token -and $token.Length -ge 3 -and [regex]::IsMatch($Node, "(?<![^$separated])" + [regex]::Escape($token) + "(?![^$separated])", 'IgnoreCase')) {
                    $leaks.Add("'$token' in [$Node]")
                }
            }
            foreach ($path in $Paths) {
                if ($path -and $Node.IndexOf($path, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $leaks.Add("'$path' in [$Node]") }
            }
            return
        }
        if ($Node -is [System.Management.Automation.PSCustomObject]) {
            foreach ($property in $Node.PSObject.Properties) { & $visit $property.Value }
            return
        }
        if ($Node -is [System.Collections.IDictionary]) {
            foreach ($item in $Node.Values) { & $visit $item }
            return
        }
        if ($Node -is [System.Collections.IEnumerable]) {
            foreach ($item in $Node) { & $visit $item }
        }
    }
    & $visit $Value
    return , $leaks
}

$benchScript = Join-Path $PSScriptRoot 'bench.ps1'
$repoRootPath = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$repoRootForward = $repoRootPath.Replace('\', '/')
$hostExe = (Get-Process -Id $PID).Path
# Under a non-ASCII directory name (#1445 review), so every path the scripts and the stub wrapper
# handle holds Unicode characters.
$unicode = "$([char]0x00FC)n$([char]0x00EF)c$([char]0x00F8)d$([char]0x00E9)"
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-bench-tests-$unicode-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # A fake build tree, so the manifest's build fields are read from a CMakeCache.txt the way
    # they are for build/<preset>. It was configured by two CMake versions, as a reused tree is:
    # only the cache's own (4.1.0) names the compiler.
    $buildDir = Join-Path $root 'build\fake-preset'
    $binDir = Join-Path $buildDir 'bin'
    New-Item -ItemType Directory -Path $binDir, (Join-Path $buildDir 'CMakeFiles\4.0.0'), (Join-Path $buildDir 'CMakeFiles\4.1.0') | Out-Null
    $U = [Environment]::UserName
    $H = $env:USERPROFILE
    # (input, expected) pairs: every absolute-path form the scrubber handles, each holding the user
    # name, and the prefix maps quoted every way (#1445 review). The same list as
    # tests/tools/test_bench_sh.py. -DBUILT_BY=<user name> is no path: the final identity pass
    # catches it (for a user name of at least 3 characters).
    $flagForms = @(
        , @('-fms-compatibility', '-fms-compatibility')
        , @("-IC:/Users/$U/a/inc", '-I<abs>/inc')
        , @("-isystemC:\Users\$U\b\inc", '-isystem<abs>/inc')
        , @("-idirafter\\fileserver\Users\$U\c\inc", '-idirafter<abs>/inc')
        , @("-iquote//fileserver/Users/$U/d/inc", '-iquote<abs>/inc')
        , @("-I//bench-host/Users/$U/sdk/include", '-I<abs>/include')
        , @("-imsvc\\?\C:\Users\$U\e\inc", '-imsvc<abs>/inc')
        , @("/I\\.\C:\Users\$U\f\inc", '/I<abs>/inc')
        , @("/I/home/$U/g/inc", '/I<abs>/inc')
        , @("/IC:\Users\$U\sdk\include", '/I<abs>/include')
        , @("-L/home/$U/lib", '-L<abs>/lib')
        , @("-B/Users/$U/bin", '-B<abs>/bin')
        , @('--sysroot=/root/sysroot', '--sysroot=<abs>/sysroot')
        , @("-fprofile-use=/home/$U/p.profdata", '-fprofile-use=<abs>/p.profdata')
        , @("-fprofile-instr-use=C:/Users/$U/q.profdata", '-fprofile-instr-use=<abs>/q.profdata')
        , @("-fprofile-use /home/$U/r.profdata", '-fprofile-use <abs>/r.profdata')
        , @("-fprofile-use C:\Users\$U\pgo\other.profdata", '-fprofile-use <abs>/other.profdata')
        , @("-fdebug-prefix-map=$H\src=/src", '-fdebug-prefix-map=<abs>/src=/src')
        , @("-ffile-prefix-map=C:/Users/$U/src=//buildhost/Users/$U/out", '-ffile-prefix-map=<abs>/src=<abs>/out')
        , @("-isystem /opt/$U/include", '-isystem <abs>/include')
        , @("-I`"C:/Users/$U/My Includes/inc`"", '-I"<abs>/inc"')
        , @("`"-isystem/home/$U/with space/inc`"", '"-isystem<abs>/inc"')
        , @('-I~/sdk/include', '-I<abs>/include')
        , @("-I ~$U/sdk/include", '-I <abs>/include')
        , @("-fprofile-instr-use=`"$repoRootForward/profiles/tasksmack.profdata`"", '-fprofile-instr-use="<source>/profiles/tasksmack.profdata"')
        , @('/DWIN32 /W3 /EHsc -DNAME=value -std=c++23 /std:c++latest -O3', '/DWIN32 /W3 /EHsc -DNAME=value -std=c++23 /std:c++latest -O3')
        , @("-Wl,-rpath,/home/$U/lib", '-Wl,-rpath,<abs>/lib')
        , @('-fsanitize-ignorelist=dir/x/y.txt', '-fsanitize-ignorelist=dir/x/y.txt')
        , @("-ffile-prefix-map=/opt/$U/source=/mapped/source", '-ffile-prefix-map=<abs>/source=<abs>/source')
        , @("-ffile-prefix-map=`"/opt/$U/source=/mapped/source`"", '-ffile-prefix-map="<abs>/source=<abs>/source"')
        , @("`"-fdebug-prefix-map=/home/$U/My Src=/build/out dir`"", '"-fdebug-prefix-map=<abs>/My Src=<abs>/out dir"')
        , @("-fmacro-prefix-map=`"/home/$U/src dir=/out/dir`"", '-fmacro-prefix-map="<abs>/src dir=<abs>/dir"')
        , @("-fprofile-prefix-map='C:\Users\$U\a b=D:\x\y'", "-fprofile-prefix-map='<abs>/a b=<abs>/y'")
        , @("-ffile-prefix-map=/home/$U/a=`"/x/new dir`"", '-ffile-prefix-map=<abs>/a="<abs>/new dir"')
        , @("-DDATA=foo:C:/Users/$U/data", '-DDATA=foo:<abs>/data')
        , @("/LIBPATH:C:\Users\$U\lib", '/LIBPATH:<abs>/lib')
        , @('-B/root/bin/x', '-B<abs>/x')
        , @('/Iinclude/common', '/Iinclude/common')
        , @('/FIinclude/config.h', '/FIinclude/config.h')
        , @('/LIBPATH:build/lib', '/LIBPATH:build/lib')
        , @("/I/home/$U/inc", '/I<abs>/inc')
        , @("/IC:/Users/$U/inc", '/I<abs>/inc')
        , @("/Users/$U/proj/config.h", '<abs>/config.h')
        , @("/DDIR=/home/$U/x", '/DDIR=<abs>/x')
        , @("-DAUTHOR=Jos$([char]0x00E9) -I`"C:/S$([char]0x00F8)urce $([char]0x00DC)/inc`"", "-DAUTHOR=Jos$([char]0x00E9) -I`"<abs>/inc`"")
        , @("-DBUILT_BY=$U", $(if ($U.Length -ge 3) { '-DBUILT_BY=<user>' } else { "-DBUILT_BY=$U" }))
    )
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_GENERATOR:INTERNAL=Ninja'
        "CMAKE_CXX_COMPILER:FILEPATH=C:\Users\$U\llvm\bin\clang++.exe"
        "CMAKE_CXX_FLAGS:STRING=$(@($flagForms | ForEach-Object { $_[0] }) -join ' ')"
        "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -fprofile-instr-use=`"$($repoRootForward)/profiles/tasksmack.profdata`" -fprofile-use=$H\x.profdata"
        'TASKSMACK_ENABLE_IPO:BOOL=ON'
        'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4'
        'CMAKE_CACHE_MINOR_VERSION:INTERNAL=1'
        'CMAKE_CACHE_PATCH_VERSION:INTERNAL=0'
    )
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeFiles\4.0.0\CMakeCXXCompiler.cmake') -Encoding utf8 -Value @(
        'set(CMAKE_CXX_COMPILER_ID "Clang")'
        'set(CMAKE_CXX_COMPILER_VERSION "21.1.0")'
    )
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeFiles\4.1.0\CMakeCXXCompiler.cmake') -Encoding utf8 -Value @(
        'set(CMAKE_CXX_COMPILER_ID "Clang")'
        'set(CMAKE_CXX_COMPILER_VERSION "22.1.8")'
    )

    # The stub benchmark binary: an ASCII batch file running a small script that writes Google
    # Benchmark-shaped JSON (holding this machine's real host name and a full executable path, as
    # the real binary does) to the last --benchmark_out, sleeps STUB_SLEEP_MS, then exits with
    # STUB_EXIT. STUB_OUTPUT=partial writes truncated JSON, as a crash mid-run does. The script is
    # Python when an interpreter is at hand (CTest passes CMake's as TASKSMACK_TEST_PYTHON): it
    # starts in a fraction of a PowerShell start-up. Otherwise it is PowerShell.
    $python = if ($env:TASKSMACK_TEST_PYTHON -and (Test-Path -LiteralPath $env:TASKSMACK_TEST_PYTHON)) { $env:TASKSMACK_TEST_PYTHON }
    else { @(Get-Command python3, python -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1 -ExpandProperty Source) }
    if ($python) {
        $stubScript = Join-Path $binDir 'stub.py'
        Set-Content -LiteralPath $stubScript -Encoding utf8 -Value @'
import json, os, socket, sys, time
out = [a for a in sys.argv[1:] if a.startswith("--benchmark_out=")][-1][len("--benchmark_out="):]
body = json.dumps({
    "context": {"host_name": os.environ.get("COMPUTERNAME") or socket.gethostname(), "executable": "C:\\some\\dir\\TaskSmackBenchmarks.exe"},
    "benchmarks": [
        {"name": "BM_X", "run_name": "BM_X", "run_type": "iteration", "repetition_index": 0, "real_time": 10.0, "time_unit": "ns"},
        {"name": "BM_X", "run_name": "BM_X", "run_type": "iteration", "repetition_index": 1, "real_time": 12.0, "time_unit": "ns"},
        {"name": "BM_X_median", "run_name": "BM_X", "run_type": "aggregate", "aggregate_name": "median", "real_time": 11.0, "time_unit": "ns"},
    ],
}, indent=2)
time.sleep(int(os.environ.get("STUB_SLEEP_MS") or 0) / 1000)
if os.environ.get("STUB_OUTPUT") == "partial":
    body = body[:60]
if os.environ.get("STUB_OUTPUT") != "none":
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
# The last --benchmark_out wins, as in Google Benchmark's flag parsing.
$out = [Environment]::GetCommandLineArgs() | Where-Object { $_ -like '--benchmark_out=*' } | Select-Object -Last 1
$out = $out.Substring('--benchmark_out='.Length)
$body = [ordered]@{
    context    = [ordered]@{ host_name = [Environment]::MachineName; executable = 'C:\some\dir\TaskSmackBenchmarks.exe' }
    benchmarks = @(
        [ordered]@{ name = 'BM_X'; run_name = 'BM_X'; run_type = 'iteration'; repetition_index = 0; real_time = 10.0; time_unit = 'ns' }
        [ordered]@{ name = 'BM_X'; run_name = 'BM_X'; run_type = 'iteration'; repetition_index = 1; real_time = 12.0; time_unit = 'ns' }
        [ordered]@{ name = 'BM_X_median'; run_name = 'BM_X'; run_type = 'aggregate'; aggregate_name = 'median'; real_time = 11.0; time_unit = 'ns' }
    )
} | ConvertTo-Json -Depth 5
if ($env:STUB_SLEEP_MS) { Start-Sleep -Milliseconds ([int]$env:STUB_SLEEP_MS) }
if ($env:STUB_OUTPUT -eq 'partial') { $body = $body.Substring(0, 60) }
if ($env:STUB_OUTPUT -ne 'none') { Set-Content -LiteralPath $out -Value $body -Encoding utf8 }
exit [int]$env:STUB_EXIT
'@
        $stubHost = $hostExe
        $stubHostArgs = '-NoProfile -File'
    }
    $stub = Join-Path $binDir 'TaskSmackBenchmarks.cmd'
    # The wrapper is ASCII (cmd.exe reads it in the console code page), so the interpreter and stub
    # script paths, which can hold Unicode characters, come from inherited environment variables
    # that Use-StubEnvironment sets (and restores).
    Set-Content -LiteralPath $stub -Encoding ascii -Value "@`"%TASKSMACK_STUB_HOST%`" %TASKSMACK_STUB_HOST_ARGS% `"%TASKSMACK_STUB_SCRIPT%`" %*`r`n@exit /b %ERRORLEVEL%"

    function Use-StubEnvironment {
        # Run $Body with the stub's variables set, putting the previous values back afterwards.
        param([hashtable]$Variables, [scriptblock]$Body)
        $all = @{ TASKSMACK_STUB_HOST = $stubHost; TASKSMACK_STUB_HOST_ARGS = $stubHostArgs; TASKSMACK_STUB_SCRIPT = $stubScript; STUB_SLEEP_MS = $null }
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

    # bench.ps1's functions and switch lists, loaded in-process for the cases that need no process
    # of their own (the end-to-end runs below cover the script's top level and its wiring).
    $benchAst = [System.Management.Automation.Language.Parser]::ParseFile($benchScript, [ref]$null, [ref]$null)
    foreach ($statement in $benchAst.EndBlock.Statements) {
        $isFunction = $statement -is [System.Management.Automation.Language.FunctionDefinitionAst]
        $isScriptVariable = $statement -is [System.Management.Automation.Language.AssignmentStatementAst] -and $statement.Left.Extent.Text -like '$script:*'
        if ($isFunction -or $isScriptVariable) { . ([scriptblock]::Create($statement.Extent.Text)) }
    }

    # ═════════════════════════ End to end: bench.ps1 runs against the stub ═════════════════════════
    # Every PowerShell start costs over a second, so the end-to-end runs share as few processes as
    # possible and run side by side:
    #  - one `pwsh -File` run with a failing stub, for the script's own non-zero exit code (#1423);
    #  - four `pwsh -File` runs racing into one directory, which also show a success exits 0;
    #  - one PowerShell host running the other scenarios one after another with `& bench.ps1`, the
    #    way a PowerShell session calls it: the script's top level runs in full each time, and a
    #    failure is the terminating error it throws.
    $quote = { param([string]$s) "'" + $s.Replace("'", "''") + "'" }
    function Get-RunFiles([string]$Directory) {
        [pscustomobject]@{
            Result   = @(Get-ChildItem -LiteralPath $Directory -Filter '*.json' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.manifest.json' })
            Manifest = @(Get-ChildItem -LiteralPath $Directory -Filter '*.manifest.json' -ErrorAction SilentlyContinue)
        }
    }

    # The scenarios for the shared host: a name, the bench.ps1 call, the stub's behaviour, and
    # optionally a working directory and native-command error promotion.
    $checkout = Join-Path $root "ch$([char]0x00E9)ckout"
    $scenarios = @(
        # #1445 review: a crash mid-run, from a session with native-command error promotion on.
        @{ Name = 'crashed'; StubExit = '5'; StubOutput = 'partial'; Promote = $true
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'crashed')) '--benchmark_filter=BM_X'" }
        # #1424: a successful run, with this machine's own host name in the args (#1445 review).
        @{ Name = 'ok'; StubExit = '0'; StubOutput = 'full'
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'ok')) '--benchmark_filter=BM_X' $(& $quote "--benchmark_context=runner=$([Environment]::MachineName)")" }
        # #1445 review: an extra --benchmark_out / --benchmark_out_format is refused before launch.
        # A relative path, run from $root, so a file written anyway would be found.
        @{ Name = 'override-out'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-out')) '--benchmark_filter=BM_X' '--benchmark_out=elsewhere.json'" }
        @{ Name = 'override-format'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-format')) '--benchmark_filter=BM_X' '--benchmark_out_format=csv'" }
        # #1445 review: bench.ps1 copied into a checkout under a Unicode path, outside git.
        @{ Name = 'uni'; StubExit = '0'; StubOutput = 'full'
            Command = "& $(& $quote (Join-Path $checkout 'tools\bench.ps1')) fake-preset -BenchmarkBinary $(& $quote (Join-Path $checkout 'build\uni\bin\TaskSmackBenchmarks.cmd')) -OutputDirectory $(& $quote (Join-Path $root 'uni')) '--benchmark_filter=BM_X'" }
        # Self-review: `& bench.ps1 -- --benchmark_filter=...` with no preset, as documented; the
        # stub exits 0 with output that cannot be redacted.
        @{ Name = 'separator'; StubExit = '0'; StubOutput = 'partial'
            Command = "& $(& $quote $benchScript) -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'separator')) -- '--benchmark_filter=BM_X'" }
    )
    New-Item -ItemType Directory -Path (Join-Path $checkout 'tools'), (Join-Path $checkout 'build\uni\bin') | Out-Null
    Copy-Item -LiteralPath $benchScript -Destination (Join-Path $checkout 'tools')
    Copy-Item -LiteralPath $stub -Destination (Join-Path $checkout 'build\uni\bin')
    $checkoutForward = $checkout.Replace('\', '/')
    Set-Content -LiteralPath (Join-Path $checkout 'build\uni\CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -fprofile-instr-use=`"$checkoutForward/profiles/tasksmack.profdata`" -DAUTHOR=Jos$([char]0x00E9)"
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

    # Race (#1445 review): results already sit under every name the racers could pick in the next
    # 30 seconds, and four runs start together into the same directory; the stub sleeps before it
    # writes, so without an up-front claim they would all pick the same name in the same second.
    $raceDir = Join-Path $root 'race'
    New-Item -ItemType Directory -Path $raceDir | Out-Null
    $start = Get-Date
    $placeholders = foreach ($offset in 0..30) {
        $path = Join-Path $raceDir "fake-preset-$($start.AddSeconds($offset).ToString('yyyyMMdd-HHmmss')).json"
        Set-Content -LiteralPath $path -Value 'placeholder' -Encoding utf8
        $path
    }

    # Start everything, each with its own stub variables, then wait for all of it.
    $failedDir = Join-Path $root 'failed'
    $failedLog = Join-Path $root 'failed.log'
    $failedErr = Join-Path $root 'failed.err'
    $processes = [System.Collections.Generic.List[object]]::new()
    Use-StubEnvironment @{} {
        $processes.Add(@{ Name = 'shared host'; Process = Start-Process -FilePath $hostExe -PassThru -WindowStyle Hidden -ArgumentList (
                    ConvertTo-RaceCommandLine @('-NoProfile', '-File', $driver, $scenarioFile, $scenarioResults)) })
    }
    Use-StubEnvironment @{ STUB_EXIT = '3'; STUB_OUTPUT = 'full' } {
        $processes.Add(@{ Name = 'failed'; Process = Start-Process -FilePath $hostExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $failedLog -RedirectStandardError $failedErr -ArgumentList (
                    ConvertTo-RaceCommandLine @('-NoProfile', '-File', $benchScript, 'fake-preset', '-BenchmarkBinary', $stub, '-OutputDirectory', $failedDir, '--benchmark_filter=BM_X')) })
    }
    Use-StubEnvironment @{ STUB_EXIT = '0'; STUB_OUTPUT = 'full'; STUB_SLEEP_MS = '400' } {
        foreach ($i in 1..4) {
            $processes.Add(@{ Name = "racer $i"; Process = Start-Process -FilePath $hostExe -PassThru -WindowStyle Hidden -ArgumentList (
                        ConvertTo-RaceCommandLine @('-NoProfile', '-File', $benchScript, 'fake-preset', '-BenchmarkBinary', $stub, '-OutputDirectory', $raceDir, '--benchmark_filter=BM_X')) })
        }
    }
    foreach ($entry in $processes) { $null = $entry.Process.Handle }  # keeps ExitCode readable after exit
    foreach ($entry in $processes) { $entry.Process.WaitForExit() }
    $outcomes = @{}
    foreach ($outcome in (Get-Content -LiteralPath $scenarioResults -Raw -Encoding utf8 | ConvertFrom-Json)) {
        $files = Get-RunFiles (Join-Path $root $outcome.Name)
        $outcomes[$outcome.Name] = [pscustomobject]@{ ExitCode = [int]$outcome.Threw; Log = $outcome.Log; Result = $files.Result; Manifest = $files.Manifest }
    }
    Assert-True ($outcomes.Count -eq $scenarios.Count) "The shared host ran $($outcomes.Count) of $($scenarios.Count) scenarios"

    # ── #1423: a benchmark binary that fails makes the script fail ──────────────────────────
    $failedProcess = ($processes | Where-Object { $_.Name -eq 'failed' }).Process
    $failedFiles = Get-RunFiles $failedDir
    $failed = [pscustomobject]@{
        ExitCode = $failedProcess.ExitCode
        Log      = (Get-Content -LiteralPath $failedLog -Raw) + (Get-Content -LiteralPath $failedErr -Raw)
        Result   = $failedFiles.Result
        Manifest = $failedFiles.Manifest
    }
    Assert-True ($failed.ExitCode -ne 0) "bench.ps1 reported success for a benchmark that exited 3:`n$($failed.Log)"
    Assert-True ($failed.Log -like '*exited with code 3*') "The failure must name the benchmark's exit code:`n$($failed.Log)"
    Assert-True ($failed.Log -notlike '*Results written to*') "A failed run must not be reported as written:`n$($failed.Log)"
    # The partial result is still redacted, and the manifest records the exit code.
    Assert-True ($failed.Result.Count -eq 1) 'A parseable partial result must be kept (redacted)'
    $failedJson = Get-Content -LiteralPath $failed.Result[0].FullName -Raw | ConvertFrom-Json
    Assert-True ($failedJson.context.host_name -eq 'redacted') 'A failed run''s output must still be redacted'
    Assert-True ($failed.Manifest.Count -eq 1) 'A failed run must still write its manifest'
    Assert-True ((Get-Content -LiteralPath $failed.Manifest[0].FullName -Raw | ConvertFrom-Json).exit_code -eq 3) 'The manifest must record the exit code'

    # A crash mid-run leaves truncated JSON that cannot be redacted: it is deleted, never kept with
    # the host name in it, and the script still fails with the benchmark's exit code -- also from a
    # session with native-command error promotion on (#1445 review), where the failure must still
    # go through the same path rather than throw before the exit code is read.
    $crashed = $outcomes['crashed']
    Assert-True ($crashed.ExitCode -ne 0) "bench.ps1 reported success for a crashed benchmark:`n$($crashed.Log)"
    Assert-True ($crashed.Log -like '*exited with code 5*') "Native error promotion bypassed the exit-code handling:`n$($crashed.Log)"
    Assert-True ($crashed.Result.Count -eq 0) 'Unparseable partial output must be deleted'
    Assert-True ($crashed.Manifest.Count -eq 1 -and (Get-Content -LiteralPath $crashed.Manifest[0].FullName -Raw | ConvertFrom-Json).exit_code -eq 5) "The manifest must be written with native error promotion on:`n$($crashed.Log)"

    # ── #1424: a successful run writes a provenance manifest beside the result ───────────────
    $ok = $outcomes['ok']
    Assert-True ($ok.ExitCode -eq 0) "A successful run must succeed:`n$($ok.Log)"
    Assert-True ($ok.Result.Count -eq 1 -and $ok.Manifest.Count -eq 1) "Expected one result and one manifest:`n$($ok.Log)"
    Assert-True ($ok.Manifest[0].Name -eq ($ok.Result[0].BaseName + '.manifest.json')) "Manifest $($ok.Manifest[0].Name) is not the result's sidecar"
    # Raw repetitions survive: the iteration rows are still in the result next to the median.
    $okJson = Get-Content -LiteralPath $ok.Result[0].FullName -Raw | ConvertFrom-Json
    Assert-True (@($okJson.benchmarks | Where-Object { $_.run_type -eq 'iteration' }).Count -eq 2) 'Per-repetition rows must be kept'
    Assert-True ($okJson.context.host_name -eq 'redacted' -and $okJson.context.executable -eq 'TaskSmackBenchmarks.exe') 'Result context must be redacted'

    $manifestText = Get-Content -LiteralPath $ok.Manifest[0].FullName -Raw
    $manifest = $manifestText | ConvertFrom-Json
    $expectedKeys = @('schema_version', 'generator', 'created_utc', 'preset', 'result_file', 'exit_code', 'git', 'binary', 'build', 'benchmark', 'machine')
    $actualKeys = @($manifest.PSObject.Properties.Name)
    Assert-True ((Compare-Object $expectedKeys $actualKeys) -eq $null) "Manifest keys: $($actualKeys -join ', ')"
    foreach ($section in @{
            git       = @('commit', 'branch', 'dirty')
            binary    = @('name', 'sha256')
            build     = @('build_type', 'generator', 'compiler', 'compiler_id', 'compiler_version', 'cxx_flags', 'cxx_flags_config', 'ipo')
            benchmark = @('args', 'raw_repetitions', 'report_aggregates_only')
            machine   = @('label', 'cpu_model', 'logical_cores', 'os_name', 'os_version', 'arch')
        }.GetEnumerator()) {
        $keys = @($manifest.($section.Key).PSObject.Properties.Name)
        Assert-True ((Compare-Object $section.Value $keys) -eq $null) "Manifest $($section.Key) keys: $($keys -join ', ')"
    }
    Assert-True ($manifest.exit_code -eq 0 -and $manifest.preset -eq 'fake-preset' -and $manifest.result_file -eq $ok.Result[0].Name) 'Manifest run fields'
    # Outside a git checkout (a source archive) or without git, the git fields are all unknown by
    # design; otherwise all known. Never a mix (#1445 review).
    $gitKnown = $null -ne $manifest.git.commit
    Assert-True ($(if ($gitKnown) { $manifest.git.commit -match '^[0-9a-f]{40}$' -and $manifest.git.branch -is [string] -and $manifest.git.dirty -is [bool] }
        else { $null -eq $manifest.git.branch -and $null -eq $manifest.git.dirty })) "Git provenance must be all known or all unknown: $($manifest.git | ConvertTo-Json -Compress)"
    Assert-True ($manifest.binary.name -eq 'TaskSmackBenchmarks.cmd') 'Binary name must be the leaf only'
    Assert-True ($manifest.binary.sha256 -eq (Get-FileHash -LiteralPath $stub -Algorithm SHA256).Hash.ToLowerInvariant()) 'Binary SHA-256'
    Assert-True ($manifest.build.build_type -eq 'Release' -and $manifest.build.compiler -eq 'clang++.exe' -and $manifest.build.compiler_id -eq 'Clang' -and
        $manifest.build.compiler_version -eq '22.1.8' -and $manifest.build.ipo -eq 'ON') "Build provenance: $($manifest.build | ConvertTo-Json -Compress)"
    Assert-True ($manifest.benchmark.raw_repetitions -eq $true -and $manifest.benchmark.report_aggregates_only -eq $false) 'Raw repetitions must be kept'
    $recordedArgs = @($manifest.benchmark.args)
    Assert-True ($recordedArgs -notcontains '--benchmark_report_aggregates_only=true') 'Aggregates-only reporting must not be forced on'
    Assert-True ($recordedArgs -contains '--benchmark_filter=BM_X' -and $recordedArgs -contains '--benchmark_repetitions=10') "Benchmark args: $($recordedArgs -join ' ')"
    Assert-True ($recordedArgs -contains "--benchmark_out=$($ok.Result[0].Name)") 'The output path must be reduced to its file name'
    if ([Environment]::MachineName.Length -ge 3) {
        Assert-True ($recordedArgs -contains '--benchmark_context=runner=<host>') "Host name in args: $($recordedArgs -join ' ')"
    }
    Assert-True ($manifest.machine.logical_cores -eq [Environment]::ProcessorCount -and $manifest.machine.os_name) 'Machine class'

    # Absolute paths in flags: the checkout's become <source>/..., others <abs>/<file name>; every
    # form of $flagForms goes through the manifest writer here.
    Assert-True ($manifest.build.cxx_flags_config -ceq '-O3 -DNDEBUG -fprofile-instr-use="<source>/profiles/tasksmack.profdata" -fprofile-use=<abs>/x.profdata') "cxx_flags_config: $($manifest.build.cxx_flags_config)"
    $expectedFlags = @($flagForms | ForEach-Object { $_[1] }) -join ' '
    if ($manifest.build.cxx_flags -cne $expectedFlags) {
        $got = $manifest.build.cxx_flags
        $diff = @($flagForms | Where-Object { -not $got.Contains($_[1]) } | ForEach-Object { "$($_[0]) -> expected $($_[1])" })
        throw "cxx_flags: $got`nForms not scrubbed as expected:`n$($diff -join "`n")"
    }

    # No host name, user name or user-profile path anywhere in the manifest: the user and host
    # names as tokens, the profile, temp and checkout paths as substrings.
    $identityPaths = @($env:USERPROFILE, [IO.Path]::GetTempPath().TrimEnd('\'), $repoRootPath, $repoRootForward, 'C:/Users', 'C:\Users')
    if ($env:USERPROFILE) { $identityPaths += $env:USERPROFILE.Replace('\', '/') }
    $leaks = Find-IdentityLeaks $manifest -Tokens @([Environment]::MachineName, [Environment]::UserName, $env:COMPUTERNAME, $env:USERNAME) -Paths $identityPaths
    Assert-True ($leaks.Count -eq 0) "The manifest leaks: $($leaks -join '; ')"
    Assert-True ($manifestText -notmatch 'host_?name|user_?name') 'The manifest must not have host or user name fields'

    # ── #1445 review: an extra --benchmark_out/--benchmark_out_format is refused before launch ──
    # Google Benchmark takes the last value, so the run would write somewhere the redaction and the
    # manifest never look (with the real host name in it). Both are refused before the stub runs.
    $elsewhere = Join-Path $root 'elsewhere.json'
    foreach ($name in @('override-out', 'override-format')) {
        $refused = $outcomes[$name]
        Assert-True ($refused.ExitCode -ne 0) "bench.ps1 accepted the override ($name):`n$($refused.Log)"
        Assert-True ($refused.Log -like '*-OutputDirectory*') "The refusal must point to -OutputDirectory ($name):`n$($refused.Log)"
        Assert-True ($refused.Result.Count -eq 0 -and $refused.Manifest.Count -eq 0) "Nothing may be written ($name)"
    }
    Assert-True (-not (Test-Path -LiteralPath $elsewhere)) "The benchmark ran and wrote '$elsewhere'"

    # ── #1445 review: a checkout under a Unicode path, outside git ─────────────────────────────
    # Its own checkout maps to <source> through the UTF-8 cache, and the missing git repository
    # leaves the git fields unknown, not an error.
    $uni = $outcomes['uni']
    Assert-True ($uni.ExitCode -eq 0 -and $uni.Manifest.Count -eq 1) "Unicode-checkout run failed:`n$($uni.Log)"
    $uniManifest = Get-Content -LiteralPath $uni.Manifest[0].FullName -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-True ($uniManifest.build.cxx_flags_config -ceq "-O3 -fprofile-instr-use=`"<source>/profiles/tasksmack.profdata`" -DAUTHOR=Jos$([char]0x00E9)") "Unicode checkout flags: $($uniManifest.build.cxx_flags_config)"
    Assert-True ($null -eq $uniManifest.git.commit -and $null -eq $uniManifest.git.branch -and $null -eq $uniManifest.git.dirty) "Outside git, the git fields must be unknown: $($uniManifest.git | ConvertTo-Json -Compress)"

    # ── Self-review: `& bench.ps1 -- --benchmark_filter=...` from PowerShell, as documented ─────
    # PowerShell ends its own parameters at "--", so the first benchmark flag binds to -Preset: the
    # default preset must be used and the flag reach the benchmark. Output that cannot be redacted
    # after exit 0 must be deleted, not left behind, and fail the script.
    $separator = $outcomes['separator']
    Assert-True ($separator.ExitCode -ne 0) "Unredactable output was reported as a success:`n$($separator.Log)"
    Assert-True ($separator.Log -like '*could not be redacted*') "The failure must say why:`n$($separator.Log)"
    Assert-True ($separator.Result.Count -eq 0) 'Unredactable output must be deleted'
    Assert-True ($separator.Manifest.Count -eq 1 -and $separator.Manifest[0].Name -like 'win-benchmark-*.manifest.json') "The default preset must be used: $(@($separator.Manifest.Name) -join ', ')"
    $separatorArgs = @((Get-Content -LiteralPath $separator.Manifest[0].FullName -Raw | ConvertFrom-Json).benchmark.args)
    Assert-True ($separatorArgs -contains '--benchmark_filter=BM_X') "The flag must reach the benchmark: $($separatorArgs -join ' ')"

    # ── #1445 review: concurrent runs claim distinct output names, never an earlier one's ─────
    foreach ($entry in @($processes | Where-Object { $_.Name -like 'racer *' })) {
        Assert-True ($entry.Process.ExitCode -eq 0) "$($entry.Name) exited $($entry.Process.ExitCode); a successful run must exit 0"
    }
    foreach ($path in $placeholders) { Assert-True ((Get-Content -LiteralPath $path -Raw).Trim() -eq 'placeholder') "An earlier result was overwritten: $path" }
    $raceResults = @(Get-ChildItem -LiteralPath $raceDir -Filter '*.json' | Where-Object { $_.Name -notlike '*.manifest.json' -and $placeholders -notcontains $_.FullName })
    $raceManifests = @(Get-ChildItem -LiteralPath $raceDir -Filter '*.manifest.json')
    Assert-True ($raceResults.Count -eq 4 -and $raceManifests.Count -eq 4) "Four concurrent runs must leave four results and manifests: $(@($raceResults.Name) + @($raceManifests.Name) -join ', ')"
    Assert-True (@($raceResults | Where-Object { $_.Name -notmatch '^fake-preset-\d{8}-\d{6}-\d+\.json$' }).Count -eq 0) "Every run must have moved to a suffix: $(@($raceResults.Name) -join ', ')"
    foreach ($manifestFile in $raceManifests) {
        $claimed = (Get-Content -LiteralPath $manifestFile.FullName -Raw | ConvertFrom-Json).result_file
        Assert-True ($manifestFile.Name -eq ([IO.Path]::GetFileNameWithoutExtension($claimed) + '.manifest.json') -and (Test-Path -LiteralPath (Join-Path $raceDir $claimed))) "Manifest $($manifestFile.Name) names $claimed"
    }

    # ═════════════════════════ In-process: bench.ps1's own functions ═════════════════════════

    # ── #1445 review: the effective --benchmark_report_aggregates_only ─────────────────────────
    # The last occurrence wins, parsed as Google Benchmark does (bare flag = true; f/n/0, false, no
    # and off = false, case-insensitively), over a default from BENCHMARK_REPORT_AGGREGATES_ONLY.
    $flag = '--benchmark_report_aggregates_only'
    $savedAggregates = $env:BENCHMARK_REPORT_AGGREGATES_ONLY
    try {
        $env:BENCHMARK_REPORT_AGGREGATES_ONLY = $null
        foreach ($case in @(
                @{ Args = @("$flag=true", "$flag=FALSE"); Expected = $false }
                @{ Args = @("$flag=no", $flag); Expected = $true }
                @{ Args = @("$flag=Yes", "$flag=0"); Expected = $false }
                @{ Args = @("$flag=off", "$flag=t"); Expected = $true }
                @{ Args = @('--benchmark_filter=BM_X', "${flag}_x=true"); Expected = $false }
            )) {
            $got = Get-EffectiveReportAggregatesOnly $case.Args
            Assert-True ($got -eq $case.Expected) "$($case.Args -join ' '): report_aggregates_only=$got; expected $($case.Expected)"
        }
        $env:BENCHMARK_REPORT_AGGREGATES_ONLY = 'yes'
        Assert-True ((Get-EffectiveReportAggregatesOnly @('--benchmark_filter=BM_X')) -eq $true) 'The environment default must apply'
        Assert-True ((Get-EffectiveReportAggregatesOnly @("$flag=0")) -eq $false) 'An argument must override the environment default'
    }
    finally { $env:BENCHMARK_REPORT_AGGREGATES_ONLY = $savedAggregates }

    # ── #1445 review: a compiler directory that does not match the cache's CMake is not guessed ─
    $staleDir = Join-Path $root 'build\stale-preset'
    New-Item -ItemType Directory -Path (Join-Path $staleDir 'CMakeFiles\4.0.0') | Out-Null
    Set-Content -LiteralPath (Join-Path $staleDir 'CMakeFiles\4.0.0\CMakeCXXCompiler.cmake') -Encoding utf8 -Value 'set(CMAKE_CXX_COMPILER_ID "Clang")', 'set(CMAKE_CXX_COMPILER_VERSION "21.1.0")'
    $staleCache = @{ CMAKE_CACHE_MAJOR_VERSION = '4'; CMAKE_CACHE_MINOR_VERSION = '2'; CMAKE_CACHE_PATCH_VERSION = '0' }
    Assert-True ($null -eq (Get-CMakeCompilerFile -BuildDirectory $staleDir -Cache $staleCache)) 'A stale compiler directory must not be used'
    Assert-True ($null -eq (Get-CMakeCompilerFile -BuildDirectory $staleDir -Cache @{})) 'Without a cache version the compiler is unknown'
    $matched = Get-CMakeCompilerFile -BuildDirectory $buildDir -Cache @{ CMAKE_CACHE_MAJOR_VERSION = '4'; CMAKE_CACHE_MINOR_VERSION = '1'; CMAKE_CACHE_PATCH_VERSION = '0' }
    Assert-True ($matched -eq (Join-Path $buildDir 'CMakeFiles\4.1.0\CMakeCXXCompiler.cmake')) "The cache's own CMake directory must be read: $matched"

    # ── #1445 review: a multi-config tree keeps the benchmark in bin/<Config>/ ─────────────────
    $multiDir = Join-Path $root 'build\multi-preset'
    New-Item -ItemType Directory -Path (Join-Path $multiDir 'bin\RelWithDebInfo'), (Join-Path $multiDir 'CMakeFiles\4.1.0') | Out-Null
    Copy-Item -LiteralPath $stub -Destination (Join-Path $multiDir 'bin\RelWithDebInfo')
    Set-Content -LiteralPath (Join-Path $multiDir 'CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING='
        'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config'
        'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG'
        'CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG'
        'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4'
        'CMAKE_CACHE_MINOR_VERSION:INTERNAL=1'
        'CMAKE_CACHE_PATCH_VERSION:INTERNAL=0'
    )
    Set-Content -LiteralPath (Join-Path $multiDir 'CMakeFiles\4.1.0\CMakeCXXCompiler.cmake') -Encoding utf8 -Value 'set(CMAKE_CXX_COMPILER_ID "Clang")', 'set(CMAKE_CXX_COMPILER_VERSION "22.1.8")'
    $tree = Find-BuildTree -Binary (Join-Path $multiDir 'bin\RelWithDebInfo\TaskSmackBenchmarks.cmd')
    Assert-True ($tree.Directory -eq $multiDir -and $tree.Config -eq 'RelWithDebInfo') "Multi-config tree: $($tree | ConvertTo-Json -Compress)"
    $flatTree = Find-BuildTree -Binary $stub
    Assert-True ($flatTree.Directory -eq $buildDir -and $null -eq $flatTree.Config) "Flat tree: $($flatTree | ConvertTo-Json -Compress)"
    Assert-True ($null -eq (Find-BuildTree -Binary (Join-Path $root 'nowhere\bin\x.exe'))) 'No cache means no tree'
    $repoRoot = $repoRootPath
    $benchBin = Join-Path $multiDir 'bin\RelWithDebInfo\TaskSmackBenchmarks.cmd'
    $multiBuild = Get-BuildProvenance
    Assert-True ($multiBuild.build_type -eq 'RelWithDebInfo' -and $multiBuild.generator -eq 'Ninja Multi-Config' -and
        $multiBuild.cxx_flags_config -eq '-O2 -g -DNDEBUG' -and $multiBuild.compiler_version -eq '22.1.8') "Multi-config build provenance: $($multiBuild | ConvertTo-Json -Compress)"

    # ── Self-review: on a non-ASCII branch, the branch name arrives intact ─────────────────────
    # git writes UTF-8 ref names; tests/tools/test_bench_sh.py checks the same for bench.sh.
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $branchName = "f$([char]0x00EB)ature"
        & git -C $checkout init -q -b $branchName 2>&1 | Out-Null
        & git -C $checkout -c user.name=bench-test -c user.email=bench-test@example.invalid commit -q --allow-empty -m init 2>&1 | Out-Null
        $repoRoot = $checkout
        $branchGit = Get-GitProvenance
        Assert-True ($branchGit.branch -ceq $branchName -and $branchGit.commit -match '^[0-9a-f]{40}$' -and $branchGit.dirty -eq $false) "Unicode branch: $($branchGit | ConvertTo-Json -Compress)"
    }
    $repoRoot = $repoRootPath

    # ── #1445 review: the leak check itself, with a controlled user and home ────────────────────
    # A correctly scrubbed manifest must pass for a user whose name is also a flag word or a JSON
    # key (root: --sysroot=; build: the "build" section), and a real leak must still be found.
    $clean = '{"build": {"build_type": "Release", "cxx_flags": "--sysroot=<abs>/sysroot -DBUILD=1 -DCMAKE_BUILD=on"}, "benchmark": {"args": ["--benchmark_filter=BM_Build"]}}' | ConvertFrom-Json
    foreach ($user in @('root', 'build')) {
        $found = Find-IdentityLeaks $clean -Tokens @($user) -Paths @("/home/$user", "C:\Users\$user")
        Assert-True ($found.Count -eq 0) "A clean manifest was reported as leaking for user '${user}': $($found -join '; ')"
        foreach ($leaky in @("-DBUILT_BY=$user", "E:/Users/$user/x", "/home/$user/src")) {
            $dirty = [pscustomobject]@{ build = [pscustomobject]@{ cxx_flags = "-O2 $leaky" } }
            Assert-True ((Find-IdentityLeaks $dirty -Tokens @($user) -Paths @("/home/$user")).Count -gt 0) "'$leaky' was not reported for user '${user}'"
        }
    }
    # The checker covers an injected host name the same way.
    $hostTokens = @('bench-host-123', 'bench-host-123.example.com')
    $hostClean = [pscustomobject]@{ benchmark = [pscustomobject]@{ args = @('--benchmark_context=runner=<host>', '--benchmark_filter=BM_bench-host-123x') } }
    Assert-True ((Find-IdentityLeaks $hostClean -Tokens $hostTokens -Paths @()).Count -eq 0) 'A redacted host name was reported'
    foreach ($leaky in @('--benchmark_context=runner=bench-host-123', 'ssh://bench-host-123.example.com/x')) {
        $dirty = [pscustomobject]@{ benchmark = [pscustomobject]@{ args = @($leaky) } }
        Assert-True ((Find-IdentityLeaks $dirty -Tokens $hostTokens -Paths @()).Count -gt 0) "'$leaky' was not reported as a host-name leak"
    }

    # ── #1445 review: the identity pass leaves a user name that is also a flag word alone ──────
    $identityCases = @(
        , @('-DBUILD=1 -DBUILD_TYPE=Release -DCMAKE_BUILD=on --benchmark_filter=BM_Build', '-DBUILD=1 -DBUILD_TYPE=Release -DCMAKE_BUILD=on --benchmark_filter=BM_Build')
        , @('-DBUILT_BY=build', '-DBUILT_BY=<user>')
        , @('E:/Users/build/x D:\Users\Build\y', 'E:/Users/<user>/x D:\Users\<user>\y')
        , @('C:\Users\build\src C:/Users/build/src', '<home>\src <home>/src')
    )
    foreach ($case in $identityCases) {
        $got = Hide-Identity $case[0] -Homes @('C:\Users\build') -User 'build' -Hosts @()
        Assert-True ($got -ceq $case[1]) "Identity pass for user 'build': [$($case[0])] became [$got], expected [$($case[1])]"
    }
    # A user name under 3 characters is never replaced on its own; a home prefix always is.
    $short = Hide-Identity '-DX=ab /home/ab/src' -Homes @('/home/ab') -User 'ab' -Hosts @()
    Assert-True ($short -ceq '-DX=ab <home>/src') "Short user name: $short"

    # ── #1445 review: host names (short and FQDN) are hidden the same way, injected here ──────
    $hostCases = @(
        , @('--benchmark_context=runner=bench-host-123', '--benchmark_context=runner=<host>')
        , @('--benchmark_context=runner=BENCH-HOST-123.example.com', '--benchmark_context=runner=<host>')
        , @('ssh://bench-host-123.example.com/x', 'ssh://<host>/x')
        , @('xbench-host-123y -DHOST_bench-host-123', 'xbench-host-123y -DHOST_bench-host-123')
    )
    foreach ($case in $hostCases) {
        $got = Hide-Identity $case[0] -Homes @() -User 'someone' -Hosts @('bench-host-123', 'bench-host-123.example.com')
        Assert-True ($got -ceq $case[1]) "Identity pass for host 'bench-host-123': [$($case[0])] became [$got], expected [$($case[1])]"
    }
    $shortHost = Hide-Identity 'runner=ab' -Homes @() -User 'someone' -Hosts @('ab')
    Assert-True ($shortHost -ceq 'runner=ab') "A host name under 3 characters is left alone: $shortHost"
    # Hide-Identity's default host list holds this machine's own name.
    Assert-True (@(Get-HostNames) -contains [Environment]::MachineName) "Get-HostNames: $(@(Get-HostNames) -join ', ')"

    # ── #1445 review: the identity pass leaves validated categorical fields alone ──────────────
    # Host and user names that coincide with OS and compiler values: only free-form fields change.
    $sample = [ordered]@{
        schema_version = 1
        generator      = 'tools/bench.ps1'
        preset         = 'Linux-preset'
        git            = [ordered]@{ commit = 'a' * 40; branch = 'clang/Linux'; dirty = $false }
        build          = [ordered]@{ build_type = 'Release'; generator = 'Ninja'; compiler = 'clang'; compiler_id = 'Clang'; compiler_version = '22.1.8'; cxx_flags = '-DHOST=Linux -DBY=clang -DCC=GNU' }
        benchmark      = [ordered]@{ args = [string[]]@('--benchmark_context=os=Windows'); raw_repetitions = $true }
        machine        = [ordered]@{ label = 'x'; cpu_model = 'Linux Box CPU'; logical_cores = 8; os_name = 'Linux'; os_version = '6.1'; arch = 'X64' }
    }
    foreach ($os in @('Linux', 'Windows')) {
        $hidden = Hide-ManifestIdentity $sample -Homes @() -User 'clang' -Hosts @($os, 'GNU')
        Assert-True ($hidden.machine.os_name -ceq 'Linux' -and $hidden.machine.os_version -ceq '6.1' -and $hidden.machine.arch -ceq 'X64') "Validated machine fields changed: $($hidden.machine | ConvertTo-Json -Compress)"
        Assert-True ($hidden.build.compiler_id -ceq 'Clang' -and $hidden.build.compiler_version -ceq '22.1.8' -and $hidden.build.build_type -ceq 'Release' -and $hidden.build.generator -ceq 'Ninja') "Validated build fields changed: $($hidden.build | ConvertTo-Json -Compress)"
        Assert-True ($hidden.git.commit -ceq ('a' * 40) -and $hidden.git.dirty -eq $false -and $hidden.schema_version -eq 1 -and $hidden.benchmark.raw_repetitions -eq $true) 'Schema, commit and boolean fields changed'
        # Free-form fields are still scrubbed.
        Assert-True ($hidden.build.compiler -ceq '<user>' -and $hidden.git.branch -ceq '<user>/' + $(if ($os -eq 'Linux') { '<host>' } else { 'Linux' })) "Free-form fields: compiler=$($hidden.build.compiler) branch=$($hidden.git.branch)"
        Assert-True ($hidden.build.cxx_flags -ceq "-DHOST=$(if ($os -eq 'Linux') { '<host>' } else { 'Linux' }) -DBY=<user> -DCC=<host>") "cxx_flags: $($hidden.build.cxx_flags)"
        Assert-True ((@($hidden.benchmark.args) -join ' ') -ceq "--benchmark_context=os=$(if ($os -eq 'Windows') { '<host>' } else { 'Windows' })") "args: $(@($hidden.benchmark.args) -join ' ')"
        $expectedCpu = if ($os -eq 'Linux') { '<host> Box CPU' } else { 'Linux Box CPU' }
        Assert-True ($hidden.machine.cpu_model -ceq $expectedCpu -and $hidden.machine.label -ceq "$expectedCpu / 8 logical cores / Linux 6.1") "CPU and label: $($hidden.machine.cpu_model) | $($hidden.machine.label)"
    }

    Write-Host 'bench.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
