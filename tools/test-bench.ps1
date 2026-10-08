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
    # The fields the writer exempts are skipped exactly as it skips them ($script:IdentityExempt,
    # build.build_type when it is one of $script:StandardBuildTypes, both loaded from bench.ps1
    # below); machine.label is checked against the machine fields it is built from instead.
    param($Value, [string[]]$Tokens, [string[]]$Paths)
    $leaks = [System.Collections.Generic.List[string]]::new()
    $separated = '\s/\\"''=:,;'
    $visit = {
        param($Node, [string]$Path)
        if ($null -eq $Node) { return }
        if ($Path -eq 'machine.label') { return }
        if ($script:IdentityExempt -contains $Path) { return }
        if ($Path -eq 'build.build_type' -and $script:StandardBuildTypes -ccontains $Node) { return }
        if ($Node -is [string]) {
            foreach ($token in $Tokens) {
                if ($token -and $token.Length -ge 3 -and [regex]::IsMatch($Node, "(?<![^$separated])" + [regex]::Escape($token) + "(?![^$separated])", 'IgnoreCase')) {
                    $leaks.Add("'$token' in $Path [$Node]")
                }
            }
            foreach ($path in $Paths) {
                if ($path -and $Node.IndexOf($path, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $leaks.Add("'$path' in $Path [$Node]") }
            }
            return
        }
        $child = { param([string]$Key) if ($Path) { "$Path.$Key" } else { $Key } }
        if ($Node -is [System.Management.Automation.PSCustomObject]) {
            foreach ($property in $Node.PSObject.Properties) { & $visit $property.Value (& $child $property.Name) }
            if ($Node.PSObject.Properties['machine'] -and $Node.machine -and $Node.machine.PSObject.Properties['label']) {
                $expected = Get-MachineLabel $Node.machine
                if ($Node.machine.label -cne $expected) { $leaks.Add("machine.label [$($Node.machine.label)] is not built from the machine fields [$expected]") }
            }
            return
        }
        if ($Node -is [System.Collections.IDictionary]) {
            foreach ($key in $Node.Keys) { & $visit $Node[$key] (& $child $key) }
            return
        }
        if ($Node -is [System.Collections.IEnumerable]) {
            foreach ($item in $Node) { & $visit $item $Path }
        }
    }
    & $visit $Value ''
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
    # Compiler flags holding user-home, profile and checkout paths, quotes and non-ASCII text: the
    # manifest records only their SHA-256 (#1445), so none of this text may appear in it.
    $rawFlags = "-fms-compatibility -I`"C:/Users/$U/My Includes/inc`" -isystem/home/$U/sdk/include -fdebug-prefix-map=$H\src=/src -DAPP_NAME=\`"TaskSmack\`" -DAUTHOR=Jos$([char]0x00E9)"
    $rawConfigFlags = "-O3 -DNDEBUG -fprofile-instr-use=`"$($repoRootForward)/profiles/tasksmack.profdata`" -fprofile-use=$H\x.profdata"
    $flagProbes = @('-fms-compatibility', 'My Includes', 'sdk/include', 'prefix-map', 'APP_NAME', 'tasksmack.profdata', 'x.profdata', '-fprofile', "Jos$([char]0x00E9)")
    function Get-ExpectedSha256([string]$Text) {
        # APIs PowerShell 7.0 (.NET Core 3.1) has; not .NET 5's SHA256.HashData/Convert.ToHexString.
        $sha = [Security.Cryptography.SHA256]::Create()
        try { -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) | ForEach-Object { $_.ToString('x2') }) }
        finally { $sha.Dispose() }
    }
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_GENERATOR:INTERNAL=Ninja'
        "CMAKE_CXX_COMPILER:FILEPATH=C:\Users\$U\llvm\bin\clang++.exe"
        "CMAKE_CXX_FLAGS:STRING=$rawFlags"
        "CMAKE_CXX_FLAGS_RELEASE:STRING=$rawConfigFlags"
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
if os.environ.get("STUB_MUTATE"):
    with open(os.environ["STUB_MUTATE"], "a", encoding="ascii") as stream:
        stream.write("@rem rebuilt during the run\r\n")
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
if ($env:STUB_MUTATE) { Add-Content -LiteralPath $env:STUB_MUTATE -Value '@rem rebuilt during the run' -Encoding ascii }
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
        # BENCHMARK_REPORT_AGGREGATES_ONLY is cleared: a caller's own setting would change what the
        # runs report (#1445 review). The saved values are restored afterwards.
        $all = @{ TASKSMACK_STUB_HOST = $stubHost; TASKSMACK_STUB_HOST_ARGS = $stubHostArgs; TASKSMACK_STUB_SCRIPT = $stubScript; STUB_SLEEP_MS = $null; BENCHMARK_REPORT_AGGREGATES_ONLY = $null }
        foreach ($name in $Variables.Keys) { $all[$name] = $Variables[$name] }
        # $null removes a variable: PowerShell passes a plain $null to a .NET string parameter as '',
        # which newer .NET keeps as an empty variable (and Google Benchmark reads an empty
        # BENCHMARK_REPORT_AGGREGATES_ONLY as true), so [NullString]::Value is passed instead.
        $set = { param([string]$Name, $Value) [Environment]::SetEnvironmentVariable($Name, $(if ($null -eq $Value) { [NullString]::Value } else { [string]$Value })) }
        $saved = @{}
        foreach ($name in $all.Keys) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name)
            & $set $name $all[$name]
        }
        try { & $Body }
        finally {
            foreach ($name in $saved.Keys) { & $set $name $saved[$name] }
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
    # A copy of the stub wrapper in a build tree of its own, rewritten by the 'mutate' scenario.
    $mutateDir = Join-Path $root 'build\mutate-preset\bin'
    New-Item -ItemType Directory -Path $mutateDir | Out-Null
    $mutatingStub = Join-Path $mutateDir 'TaskSmackBenchmarks.cmd'
    Copy-Item -LiteralPath $stub -Destination $mutatingStub
    $launchedHash = (Get-FileHash -LiteralPath $mutatingStub -Algorithm SHA256).Hash.ToLowerInvariant()
    # A decoy with the stub's name, put first on PATH by the 'bare-name' scenario: it leaves a marker
    # and fails.
    $decoyDir = Join-Path $root 'decoy'
    $decoyMarker = Join-Path $root 'decoy-ran'
    New-Item -ItemType Directory -Path $decoyDir | Out-Null
    Set-Content -LiteralPath (Join-Path $decoyDir 'TaskSmackBenchmarks.cmd') -Encoding ascii -Value @('@echo off', "type nul > `"$decoyMarker`"", 'exit /b 7')
    $scenarios = @(
        # #1445 review: a crash mid-run, from a session with native-command error promotion on.
        @{ Name = 'crashed'; StubExit = '5'; StubOutput = 'partial'; Promote = $true
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'crashed')) '--benchmark_filter=BM_X'" }
        # #1424: a successful run, with this machine's own host name in the args (#1445 review).
        @{ Name = 'ok'; StubExit = '0'; StubOutput = 'full'
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'ok')) '--benchmark_filter=BM_X' $(& $quote "--benchmark_context=tsk_ctx_machine=$([Environment]::MachineName)") $(& $quote "--benchmark_context=tsk_ctx_data=$H\bench data\input.bin")" }
        # #1445 review: an extra --benchmark_out / --benchmark_out_format is refused before launch.
        # A relative path, run from $root, so a file written anyway would be found.
        @{ Name = 'override-out'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-out')) '--benchmark_filter=BM_X' '--benchmark_out=elsewhere.json'" }
        @{ Name = 'override-format'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'override-format')) '--benchmark_filter=BM_X' '--benchmark_out_format=csv'" }
        # #1445 review: bench.ps1 copied into a checkout under a Unicode path, outside git.
        @{ Name = 'uni'; StubExit = '0'; StubOutput = 'full'
            Command = "& $(& $quote (Join-Path $checkout 'tools\bench.ps1')) fake-preset -BenchmarkBinary $(& $quote (Join-Path $checkout 'build\uni\bin\TaskSmackBenchmarks.cmd')) -OutputDirectory $(& $quote (Join-Path $root 'uni')) '--benchmark_filter=BM_X' $(& $quote "--benchmark_context=tsk_ctx_profile=$checkout\profiles\tasksmack.profdata")" }
        # #1445 review: the binary is rebuilt during the run; the manifest keeps the launched hash.
        @{ Name = 'mutate'; StubExit = '0'; StubOutput = 'full'; StubMutate = $mutatingStub
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $mutatingStub) -OutputDirectory $(& $quote (Join-Path $root 'mutate')) '--benchmark_filter=BM_X'" }
        # #1445 review: a relative -BenchmarkBinary after Set-Location (the process directory stays
        # where the host started) still finds its build tree.
        @{ Name = 'relative'; StubExit = '0'; StubOutput = 'full'; Cwd = $root
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary 'build\fake-preset\bin\TaskSmackBenchmarks.cmd' -OutputDirectory $(& $quote (Join-Path $root 'relative')) '--benchmark_filter=BM_X'" }
        # #1445 review: a bare -BenchmarkBinary name is the file in the PowerShell location, the one
        # checked and hashed -- not a same-named decoy earlier on PATH.
        @{ Name = 'bare-name'; StubExit = '0'; StubOutput = 'full'; Cwd = (Split-Path -Parent $stub); PathFirst = $decoyDir
            Command = "& $(& $quote $benchScript) fake-preset -BenchmarkBinary 'TaskSmackBenchmarks.cmd' -OutputDirectory $(& $quote (Join-Path $root 'bare-name')) '--benchmark_filter=BM_X'" }
        # #1445 review: a preset named after the user or the machine reaches no file name and no
        # manifest string (scenarios added below when the name has 3+ characters).
        # Self-review: `& bench.ps1 -- --benchmark_filter=...` with no preset, as documented; the
        # stub exits 0 with output that cannot be redacted.
        @{ Name = 'separator'; StubExit = '0'; StubOutput = 'partial'
            Command = "& $(& $quote $benchScript) -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root 'separator')) -- '--benchmark_filter=BM_X'" }
    )
    $presetIdentities = [ordered]@{ user = [Environment]::UserName; host = [Environment]::MachineName.Split('.')[0] }
    foreach ($kind in @($presetIdentities.Keys)) {
        $name = $presetIdentities[$kind]
        if ($name.Length -lt 3 -or $name -in @('user', 'host')) { $presetIdentities.Remove($kind); continue }
        $scenarios += @{ Name = "preset-$kind"; StubExit = '0'; StubOutput = 'full'
            Command = "& $(& $quote $benchScript) $(& $quote $name) -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote (Join-Path $root "preset-$kind")) '--benchmark_filter=BM_X'" }
    }
    New-Item -ItemType Directory -Path (Join-Path $checkout 'tools'), (Join-Path $checkout 'build\uni\bin') | Out-Null
    Copy-Item -LiteralPath $benchScript -Destination (Join-Path $checkout 'tools')
    Copy-Item -LiteralPath $stub -Destination (Join-Path $checkout 'build\uni\bin')
    $checkoutForward = $checkout.Replace('\', '/')
    $uniConfigFlags = "-O3 -fprofile-instr-use=`"$checkoutForward/profiles/tasksmack.profdata`" -DAUTHOR=Jos$([char]0x00E9)"
    Set-Content -LiteralPath (Join-Path $checkout 'build\uni\CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        "CMAKE_CXX_FLAGS_RELEASE:STRING=$uniConfigFlags"
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
    $env:STUB_MUTATE = if ($scenario.PSObject.Properties['StubMutate']) { $scenario.StubMutate } else { $null }
    $PSNativeCommandUseErrorActionPreference = [bool]($scenario.PSObject.Properties['Promote'] -and $scenario.Promote)
    $here = Get-Location
    $path = $env:PATH
    if ($scenario.PSObject.Properties['Cwd']) { Set-Location -LiteralPath $scenario.Cwd }
    if ($scenario.PSObject.Properties['PathFirst']) { $env:PATH = $scenario.PathFirst + [IO.Path]::PathSeparator + $path }
    $threw = $false
    try { $log = & ([scriptblock]::Create($scenario.Command)) *>&1 | Out-String }
    catch { $threw = $true; $log = "$($_ | Out-String)" }
    finally { Set-Location $here; $env:PATH = $path }
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
    Assert-True ($failed.Manifest.Count -eq 1) "A failed run must still write its manifest:`n$($failed.Log)"
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
            build     = @('build_type', 'generator', 'compiler', 'compiler_id', 'compiler_version', 'cxx_flags_sha256', 'cxx_flags_config_sha256', 'cxx_flags_source', 'ipo', 'ipo_source')
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
        $manifest.build.compiler_version -eq '22.1.8' -and $manifest.build.ipo -eq 'ON' -and $manifest.build.ipo_source -eq 'TASKSMACK_ENABLE_IPO') "Build provenance: $($manifest.build | ConvertTo-Json -Compress)"
    Assert-True ($manifest.benchmark.raw_repetitions -eq $true -and $manifest.benchmark.report_aggregates_only -eq $false) 'Raw repetitions must be kept'
    $recordedArgs = @($manifest.benchmark.args)
    Assert-True ($recordedArgs -notcontains '--benchmark_report_aggregates_only=true') 'Aggregates-only reporting must not be forced on'
    Assert-True ($recordedArgs -contains '--benchmark_filter=BM_X' -and $recordedArgs -contains '--benchmark_repetitions=10') "Benchmark args: $($recordedArgs -join ' ')"
    Assert-True ($recordedArgs -contains "--benchmark_out=$($ok.Result[0].Name)") 'The output path must be reduced to its file name'
    if ([Environment]::MachineName.Length -ge 3) {
        Assert-True ($recordedArgs -contains "--benchmark_context=sha256:$(Get-ExpectedSha256 "tsk_ctx_machine=$([Environment]::MachineName)")") "Host name in args: $($recordedArgs -join ' ')"
    }
    Assert-True ($manifest.machine.logical_cores -eq [Environment]::ProcessorCount -and $manifest.machine.os_name) 'Machine class'

    # The compiler flags are recorded only as SHA-256 of their CMakeCache.txt values (#1445):
    # none of that text, and none of its paths, appears anywhere in the manifest.
    Assert-True ($manifest.build.cxx_flags_sha256 -ceq (Get-ExpectedSha256 $rawFlags) -and $manifest.build.cxx_flags_config_sha256 -ceq (Get-ExpectedSha256 $rawConfigFlags)) "Flag hashes: $($manifest.build | ConvertTo-Json -Compress)"
    foreach ($probe in $flagProbes) { Assert-True ($manifestText.IndexOf($probe, [StringComparison]::OrdinalIgnoreCase) -lt 0) "Flag text '$probe' is in the manifest" }
    # The benchmark arguments: allowlisted options as written, a --benchmark_context value hashed.
    foreach ($expected in @('--benchmark_repetitions=10', '--benchmark_min_time=0.5s', '--benchmark_display_aggregates_only=true', '--benchmark_out_format=json', '--benchmark_filter=BM_X', "--benchmark_out=$($ok.Result[0].Name)", "--benchmark_context=sha256:$(Get-ExpectedSha256 "tsk_ctx_data=$H\bench data\input.bin")")) {
        Assert-True ($recordedArgs -ccontains $expected) "Recorded args lack [$expected]: $($recordedArgs -join ' ')"
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
    # Its UTF-8 flags hash as UTF-8, a --benchmark_context value naming the checkout is recorded
    # only as its hash, and the missing git repository leaves the git fields unknown, not an error.
    $uni = $outcomes['uni']
    Assert-True ($uni.ExitCode -eq 0 -and $uni.Manifest.Count -eq 1) "Unicode-checkout run failed:`n$($uni.Log)"
    $uniManifest = Get-Content -LiteralPath $uni.Manifest[0].FullName -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-True ($uniManifest.build.cxx_flags_config_sha256 -ceq (Get-ExpectedSha256 $uniConfigFlags)) "Unicode checkout flag hash: $($uniManifest.build.cxx_flags_config_sha256)"
    Assert-True (@($uniManifest.benchmark.args) -ccontains "--benchmark_context=sha256:$(Get-ExpectedSha256 "tsk_ctx_profile=$checkout\profiles\tasksmack.profdata")") "Unicode checkout args: $(@($uniManifest.benchmark.args) -join ' ')"
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

    # ── #1445 review: provenance is captured before the benchmark starts ─────────────────────
    $mutate = $outcomes['mutate']
    Assert-True ($mutate.ExitCode -eq 0 -and $mutate.Manifest.Count -eq 1) "Rebuilt-binary run failed:`n$($mutate.Log)"
    Assert-True ((Get-FileHash -LiteralPath $mutatingStub -Algorithm SHA256).Hash.ToLowerInvariant() -ne $launchedHash) 'The stub did not change during the run'
    $mutateManifest = Get-Content -LiteralPath $mutate.Manifest[0].FullName -Raw | ConvertFrom-Json
    Assert-True ($mutateManifest.binary.sha256 -eq $launchedHash -and $mutateManifest.exit_code -eq 0) "The manifest must keep the launched binary's hash: $($mutateManifest.binary.sha256) vs $launchedHash"

    # ── #1445 review: a relative -BenchmarkBinary resolves against the PowerShell location ────
    $relative = $outcomes['relative']
    Assert-True ($relative.ExitCode -eq 0 -and $relative.Manifest.Count -eq 1) "Relative-binary run failed:`n$($relative.Log)"
    $relativeBuild = (Get-Content -LiteralPath $relative.Manifest[0].FullName -Raw | ConvertFrom-Json).build
    Assert-True ($relativeBuild.build_type -eq 'Release' -and $relativeBuild.compiler_version -eq '22.1.8') "A relative binary must find its build tree: $($relativeBuild | ConvertTo-Json -Compress)"

    # ── #1445 review: a preset named after the user or the machine reaches no name ─────────────
    # The file names are checked for the names as plain substrings; the manifest with the
    # field-aware Find-IdentityLeaks, which skips the fields the writer keeps on purpose (#1445
    # review): a user named Release keeps the Release build type, a host named Windows the OS name.
    $identities = @([Environment]::UserName, [Environment]::MachineName, [Environment]::MachineName.Split('.')[0]) | Where-Object { $_.Length -ge 3 }
    function Get-JsonStrings($Node) {
        if ($Node -is [string]) { $Node }
        elseif ($Node -is [System.Management.Automation.PSCustomObject]) { foreach ($property in $Node.PSObject.Properties) { Get-JsonStrings $property.Value } }
        elseif ($Node -is [System.Collections.IEnumerable]) { foreach ($item in $Node) { Get-JsonStrings $item } }
    }
    foreach ($kind in @($presetIdentities.Keys)) {
        $run = $outcomes["preset-$kind"]
        Assert-True ($run.ExitCode -eq 0 -and $run.Result.Count -eq 1 -and $run.Manifest.Count -eq 1) "Preset-$kind run failed:`n$($run.Log)"
        $presetManifest = Get-Content -LiteralPath $run.Manifest[0].FullName -Raw | ConvertFrom-Json
        Assert-True ($presetManifest.preset -ceq "<$kind>" -and $presetManifest.result_file -ceq $run.Result[0].Name -and $run.Result[0].Name.StartsWith("$kind-", [StringComparison]::Ordinal)) "Preset named after the ${kind}: preset=$($presetManifest.preset) result_file=$($presetManifest.result_file) file=$($run.Result[0].Name)"
        foreach ($name in @($run.Result[0].Name, $run.Manifest[0].Name)) {
            foreach ($identity in $identities) {
                Assert-True ($name.IndexOf($identity, [StringComparison]::OrdinalIgnoreCase) -lt 0) "'$identity' in the file name '$name' (preset named after the $kind)"
            }
        }
        $presetLeaks = Find-IdentityLeaks $presetManifest -Tokens $identities -Paths @()
        Assert-True ($presetLeaks.Count -eq 0) "Preset named after the ${kind}: $($presetLeaks -join '; ')"
        # A user named Release, a host named Windows: the field-aware check passes on the fields
        # the writer keeps on purpose, which a plain scan of every string would flag.
        $kept = Find-IdentityLeaks $presetManifest -Tokens @('Release', 'Windows') -Paths @()
        Assert-True ($kept.Count -eq 0) "Kept categorical fields reported as leaks: $($kept -join '; ')"
        Assert-True (@(Get-JsonStrings $presetManifest) -ccontains 'Release') 'The fixture keeps a Release build type, so the case above means something'
    }

    # ── #1445 review: a bare -BenchmarkBinary is the file in the location, not one on PATH ─────
    $bare = $outcomes['bare-name']
    Assert-True (-not (Test-Path -LiteralPath $decoyMarker)) "The decoy on PATH was launched:`n$($bare.Log)"
    Assert-True ($bare.ExitCode -eq 0 -and $bare.Result.Count -eq 1 -and $bare.Manifest.Count -eq 1) "Bare-name run failed:`n$($bare.Log)"
    $bareManifest = Get-Content -LiteralPath $bare.Manifest[0].FullName -Raw | ConvertFrom-Json
    $stubHash = (Get-FileHash -LiteralPath $stub -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-True ($bareManifest.binary.sha256 -eq $stubHash -and $bareManifest.exit_code -eq 0) "The manifest must hash the binary that ran: $($bareManifest.binary.sha256) vs $stubHash"

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
        $multiBuild.cxx_flags_config_sha256 -ceq (Get-ExpectedSha256 '-O2 -g -DNDEBUG') -and $multiBuild.compiler_version -eq '22.1.8') "Multi-config build provenance: $($multiBuild | ConvertTo-Json -Compress)"

    # ── #1445: equal flags hash equally across build trees; a changed flag changes the hash ────
    $benchBin = $stub
    $original = Get-BuildProvenance
    foreach ($case in @(@{ Name = 'same-flags'; Flags = $rawFlags; Same = $true }, @{ Name = 'changed-flag'; Flags = $rawFlags.Replace('-fms-compatibility', '-fms-compatibility -fno-rtti'); Same = $false })) {
        $tree = Join-Path $root "build\$($case.Name)"
        New-Item -ItemType Directory -Path (Join-Path $tree 'bin') | Out-Null
        Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value @('CMAKE_BUILD_TYPE:STRING=Release', "CMAKE_CXX_FLAGS:STRING=$($case.Flags)", "CMAKE_CXX_FLAGS_RELEASE:STRING=$rawConfigFlags")
        $benchBin = Join-Path $tree 'bin\TaskSmackBenchmarks.cmd'
        $other = Get-BuildProvenance
        Assert-True (($other.cxx_flags_sha256 -ceq $original.cxx_flags_sha256) -eq $case.Same -and $other.cxx_flags_config_sha256 -ceq $original.cxx_flags_config_sha256) "$($case.Name): $($other.cxx_flags_sha256) vs $($original.cxx_flags_sha256)"
    }

    # ── Self-review: on a non-ASCII branch, the branch name arrives intact ─────────────────────
    # git writes UTF-8 ref names; tests/tools/test_bench_sh.py checks the same for bench.sh.
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $branchName = "f$([char]0x00EB)ature"
        & git -C $checkout init -q -b $branchName 2>&1 | Out-Null
        & git -C $checkout -c user.name=bench-test -c user.email=bench-test@example.invalid commit -q --allow-empty -m init 2>&1 | Out-Null
        $repoRoot = $checkout
        $branchGit = Get-GitProvenance
        Assert-True ($branchGit.branch -ceq $branchName -and $branchGit.commit -match '^[0-9a-f]{40}$' -and $branchGit.dirty -eq $false) "Unicode branch: $($branchGit | ConvertTo-Json -Compress)"
        # #1445 review: a user or host name in the branch is hidden between the branch's own '-',
        # '.', '_' and '/' too. The same cases as test_bench_sh.py.
        foreach ($case in @(
                , @('benchuser-fix', '<user>-fix')
                , @('feature/benchuser', 'feature/<user>')
                , @('BenchUser_wip', '<user>_wip')
                , @('bench-host-123.example.com-test', '<host>-test')
                , @('ci/bench-host-123/nightly', 'ci/<host>/nightly')
                , @('benchusers-x', 'benchusers-x')
            )) {
            & git -C $checkout checkout -q -b $case[0] 2>&1 | Out-Null
            $caseGit = Get-GitProvenance -User 'benchuser' -Hosts @('bench-host-123', 'bench-host-123.example.com')
            Assert-True ($caseGit.branch -ceq $case[1]) "Branch [$($case[0])] recorded as [$($caseGit.branch)], expected [$($case[1])]"
        }
    }
    $repoRoot = $repoRootPath

    # ── #1445 review: no provenance inherited from an enclosing repository ───────────────────
    # A source archive unpacked inside another checkout: git would find that checkout.
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $outer = Join-Path $root 'outer'
        New-Item -ItemType Directory -Path (Join-Path $outer 'inner') | Out-Null
        & git -C $outer init -q 2>&1 | Out-Null
        & git -C $outer -c user.name=bench-test -c user.email=bench-test@example.invalid commit -q --allow-empty -m outer 2>&1 | Out-Null
        $repoRoot = Join-Path $outer 'inner'
        $nestedGit = Get-GitProvenance
        Assert-True ($null -eq $nestedGit.commit -and $null -eq $nestedGit.branch -and $null -eq $nestedGit.dirty) "Inherited provenance: $($nestedGit | ConvertTo-Json -Compress)"
        $repoRoot = $outer
        Assert-True ((Get-GitProvenance).commit -match '^[0-9a-f]{40}$') 'The repository itself must still be read'
        $repoRoot = $repoRootPath
    }

    # ── #1445 review: only CMake's standard configurations are exempt as build types ─────────
    foreach ($case in @(
            @{ BuildType = 'benchuser'; User = 'benchuser'; Hosts = @(); Expected = '<user>' }
            @{ BuildType = 'benchhost'; User = 'someone'; Hosts = @('benchhost'); Expected = '<host>' }
            @{ BuildType = 'Release'; User = 'Release'; Hosts = @('Release'); Expected = 'Release' }
            @{ BuildType = 'RelWithDebInfo'; User = 'RelWithDebInfo'; Hosts = @(); Expected = 'RelWithDebInfo' }
            # A compound custom build type: its own '-', '.' and '_' bound the name too.
            @{ BuildType = 'ASan-benchuser'; User = 'benchuser'; Hosts = @(); Expected = 'ASan-<user>' }
            @{ BuildType = 'Release_benchhost'; User = 'someone'; Hosts = @('benchhost'); Expected = 'Release_<host>' }
            @{ BuildType = 'ci.BenchUser'; User = 'benchuser'; Hosts = @(); Expected = 'ci.<user>' }
            @{ BuildType = 'ASan-benchusers'; User = 'benchuser'; Hosts = @(); Expected = 'ASan-benchusers' }
            @{ BuildType = 'ASan-UBSan'; User = 'asan'; Hosts = @(); Expected = '<user>-UBSan' }
        )) {
        $hiddenBuild = (Hide-ManifestIdentity ([ordered]@{ build = [ordered]@{ build_type = $case.BuildType } }) -Homes @() -User $case.User -Hosts $case.Hosts).build
        Assert-True ($hiddenBuild.build_type -ceq $case.Expected) "Build type '$($case.BuildType)': $($hiddenBuild.build_type), expected $($case.Expected)"
    }

    # ── #1445 review: benchmark arguments -- allowlisted options as written, the rest hashed ────
    $verbatim = @(
        '--benchmark_repetitions=10', '--benchmark_min_time=0.5s', '--benchmark_min_time=100x', '--benchmark_min_time=2',
        '--benchmark_min_warmup_time=0.25', '--benchmark_min_warmup_time=1.5e-1s',
        '--benchmark_display_aggregates_only=true', '--benchmark_report_aggregates_only=FALSE', '--benchmark_report_aggregates_only',
        '--benchmark_enable_random_interleaving=yes', '--benchmark_counters_tabular=1', '--benchmark_dry_run', '--benchmark_list_tests=t',
        '--benchmark_time_unit=ms', '--benchmark_format=csv', '--benchmark_out_format=json', '--benchmark_color=auto', '--v=2',
        '--benchmark_filter=BM_(A|B)$', '--benchmark_filter='
    )
    foreach ($argument in $verbatim) {
        Assert-True ((Get-RecordedArgument $argument) -ceq $argument) "Allowlisted [$argument] became [$(Get-RecordedArgument $argument)]"
    }
    $hashed = @(
        # A malformed allowlisted value.
        , @('--benchmark_repetitions=10;rm', '--benchmark_repetitions', '10;rm')
        , @('--benchmark_min_time=/home/u/x', '--benchmark_min_time', '/home/u/x')
        , @('--benchmark_time_unit=hours', '--benchmark_time_unit', 'hours')
        , @('--Benchmark_Repetitions=10', '--Benchmark_Repetitions', '10')
        # --benchmark_context values with quoted, embedded and '=' paths (#1445 review).
        , @('--benchmark_context=src="/srv/private-checkout/tasksmack/profiles/input.bin"', '--benchmark_context', 'src="/srv/private-checkout/tasksmack/profiles/input.bin"')
        , @('--benchmark_context=note=loaded /srv/private-checkout/tasksmack/profiles/input.bin', '--benchmark_context', 'note=loaded /srv/private-checkout/tasksmack/profiles/input.bin')
        , @('--benchmark_context=note=/srv/private=run/host/data.bin', '--benchmark_context', 'note=/srv/private=run/host/data.bin')
        , @('--benchmark_perf_counters=CYCLES', '--benchmark_perf_counters', 'CYCLES')
        , @('--some_unknown_flag=/home/u/x', '--some_unknown_flag', '/home/u/x')
    )
    foreach ($case in $hashed) {
        $got = Get-RecordedArgument $case[0]
        Assert-True ($got -ceq "$($case[1])=sha256:$(Get-ExpectedSha256 $case[2])") "[$($case[0])] became [$got]"
        Assert-True ((Get-RecordedArgument $case[0]) -ceq $got) "[$($case[0])] must hash the same way every time"
    }
    foreach ($whole in @('--some_unknown_switch', '/home/u/positional', '-x', '--benchmark_context')) {
        Assert-True ((Get-RecordedArgument $whole) -ceq "sha256:$(Get-ExpectedSha256 $whole)") "[$whole] became [$(Get-RecordedArgument $whole)]"
    }
    Assert-True ((Get-RecordedArgument "--benchmark_out=$root\out\fake-1.json") -ceq '--benchmark_out=fake-1.json') 'The script''s own output file is recorded by name'
    Assert-True ((Get-RecordedArgument '--benchmark_context=a=1') -cne (Get-RecordedArgument '--benchmark_context=a=2')) 'Different values must hash differently'

    # ── #1445 review: cache entries are read as CMake reads them ─────────────────────────────────
    # CMake writes KEY:TYPE=VALUE with any character but ':' in an unquoted key (a custom build
    # type's CMAKE_CXX_FLAGS_ASAN-UBSAN), quotes a key holding ':', and puts a value with trailing
    # whitespace in single quotes (cmState::ParseCacheEntry). The same cases as test_bench_sh.py.
    $cacheFile = Join-Path $root 'parse-CMakeCache.txt'
    [IO.File]::WriteAllText($cacheFile, (@(
                '# This is the CMakeCache file.'
                '//Help text: not=an entry'
                'CMAKE_CXX_FLAGS_ASAN-UBSAN:STRING=-O2 -fsanitize=address'
                'CMAKE_CXX_FLAGS_REL.WITH+INFO:STRING=-O1'
                '"KEY:WITH=SPECIALS":STRING=colon'
                '"QUOTED_UNTYPED"=q'
                'UNTYPED=u'
                "  INDENTED:BOOL=ON`r"
                "TRAILING:STRING='-O3 '"
                "PADDED:STRING=-O2 `t`r"
                'LEADING:STRING= -O3'
                'EMPTY:STRING='
                'lower_case:STRING=lower'
                'not an entry'
            ) -join "`n") + "`n")
    $parsed = Read-CMakeCache -Path $cacheFile
    $expectedCache = [ordered]@{
        'CMAKE_CXX_FLAGS_ASAN-UBSAN' = '-O2 -fsanitize=address'; 'CMAKE_CXX_FLAGS_REL.WITH+INFO' = '-O1'; 'KEY:WITH=SPECIALS' = 'colon'
        'QUOTED_UNTYPED' = 'q'; 'UNTYPED' = 'u'; 'INDENTED' = 'ON'; 'TRAILING' = '-O3 '; 'PADDED' = '-O2'; 'LEADING' = ' -O3'; 'EMPTY' = ''; 'lower_case' = 'lower'
    }
    Assert-True ($parsed.Count -eq $expectedCache.Count) "Parsed $($parsed.Count) entries, expected $($expectedCache.Count): $(@($parsed.Keys) -join ', ')"
    foreach ($key in $expectedCache.Keys) {
        Assert-True ($parsed.ContainsKey($key) -and $parsed[$key] -ceq $expectedCache[$key]) "[$key] read as [$($parsed[$key])], expected [$($expectedCache[$key])]"
    }
    Assert-True (-not $parsed.ContainsKey('LOWER_CASE')) 'Cache keys are case-sensitive'
    Assert-True ((ConvertTo-CMakeUpper 'asan-ubsan.rel+info') -ceq 'ASAN-UBSAN.REL+INFO') 'The <CONFIG> suffix is upper-cased as CMake does'
    # The configuration's flags are found and hashed for a hyphenated, dotted or plus-signed custom
    # build type.
    foreach ($case in @(@{ Name = 'ASan-UBSan'; Flags = '-O2 -fsanitize=address' }, @{ Name = 'Rel.With+Info'; Flags = '-O1' })) {
        $tree = Join-Path $root "build\type-$($case.Name)"
        New-Item -ItemType Directory -Path (Join-Path $tree 'bin') | Out-Null
        Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value @("CMAKE_BUILD_TYPE:STRING=$($case.Name)", "CMAKE_CXX_FLAGS_$($case.Name.ToUpperInvariant()):STRING=$($case.Flags)")
        $benchBin = Join-Path $tree 'bin\TaskSmackBenchmarks.cmd'
        $build = Get-BuildProvenance
        Assert-True ($build.build_type -ceq $case.Name -and $build.cxx_flags_config_sha256 -ceq (Get-ExpectedSha256 $case.Flags)) "$($case.Name): $($build | ConvertTo-Json -Compress)"
    }

    # ── #1445 review: an absent cache entry hashes as null, an empty one as the empty string ───
    Assert-True ($null -eq (Get-TextSha256 $null)) 'Get-TextSha256 $null must be $null'
    foreach ($case in @(@{ Name = 'flags-absent'; Lines = @('CMAKE_BUILD_TYPE:STRING=Release'); Expected = $null }, @{ Name = 'flags-empty'; Lines = @('CMAKE_BUILD_TYPE:STRING=Release', 'CMAKE_CXX_FLAGS:STRING=', 'CMAKE_CXX_FLAGS_RELEASE:STRING='); Expected = (Get-ExpectedSha256 '') })) {
        $tree = Join-Path $root "build\$($case.Name)"
        New-Item -ItemType Directory -Path (Join-Path $tree 'bin') | Out-Null
        Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value $case.Lines
        $benchBin = Join-Path $tree 'bin\TaskSmackBenchmarks.cmd'
        $build = Get-BuildProvenance
        Assert-True ($build.cxx_flags_sha256 -ceq $case.Expected -and $build.cxx_flags_config_sha256 -ceq $case.Expected) "$($case.Name): $($build | ConvertTo-Json -Compress)"
        if ($null -eq $case.Expected) { Assert-True ($null -eq $build.cxx_flags_sha256 -and $null -eq $build.cxx_flags_config_sha256) "$($case.Name) must be null, not a hash" }
    }

    # ── #1445 review: the flag hashes are what the binary was linked with ─────────────────────────
    # Reconfiguring CMAKE_CXX_FLAGS_<CONFIG> without a rebuild changes the cache, not the binary;
    # the build information beside the binary carries the hashes it was built with and is
    # preferred. Older trees, or unusable build information, fall back to the cache. The same cases
    # as test_bench_sh.py.
    $linked = Get-ExpectedSha256 '-O2 linked'
    $linkedConfig = Get-ExpectedSha256 '-O3 linked'
    $fromCache = @((Get-ExpectedSha256 '-O2 reconfigured'), (Get-ExpectedSha256 '-O3 reconfigured'))
    foreach ($case in @(
            @{ Name = 'flags-linked'; BuildInfo = (@{ ipo = 'ON'; cxx_flags_sha256 = $linked; cxx_flags_config_sha256 = $linkedConfig } | ConvertTo-Json); Expected = @($linked, $linkedConfig); Source = 'buildinfo' }
            @{ Name = 'flags-linked-absent-entry'; BuildInfo = '{"cxx_flags_sha256": null, "cxx_flags_config_sha256": "' + $linkedConfig + '"}'; Expected = @($null, $linkedConfig); Source = 'buildinfo' }
            @{ Name = 'flags-no-buildinfo'; BuildInfo = $null; Expected = $fromCache; Source = 'cache' }
            @{ Name = 'flags-ipo-only-buildinfo'; BuildInfo = '{"ipo": "ON"}'; Expected = $fromCache; Source = 'cache' }
            @{ Name = 'flags-malformed-hash'; BuildInfo = '{"cxx_flags_sha256": "ABC", "cxx_flags_config_sha256": "' + $linkedConfig + '"}'; Expected = $fromCache; Source = 'cache' }
        )) {
        $tree = Join-Path $root "build\$($case.Name)"
        New-Item -ItemType Directory -Path (Join-Path $tree 'bin') | Out-Null
        Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value @('CMAKE_BUILD_TYPE:STRING=Release', 'CMAKE_CXX_FLAGS:STRING=-O2 reconfigured', 'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 reconfigured')
        if ($null -ne $case.BuildInfo) { [IO.File]::WriteAllText((Join-Path $tree 'bin\TaskSmackBenchmarks.buildinfo.json'), $case.BuildInfo) }
        $benchBin = Join-Path $tree 'bin\TaskSmackBenchmarks.cmd'
        $build = Get-BuildProvenance
        Assert-True ($build.cxx_flags_sha256 -ceq $case.Expected[0] -and $build.cxx_flags_config_sha256 -ceq $case.Expected[1] -and $build.cxx_flags_source -ceq $case.Source) "$($case.Name): $($build | ConvertTo-Json -Compress)"
        if ($null -eq $case.Expected[0]) { Assert-True ($null -eq $build.cxx_flags_sha256) "$($case.Name): an absent entry must stay null" }
    }

    # ── #1445 review: the build type is the configuration the binary was linked as ──────────────
    # A single-config tree reconfigured from Release to Debug without a rebuild holds the Release
    # binary and its build information while the cache says Debug. The same cases as
    # test_bench_sh.py.
    foreach ($case in @(
            @{ Name = 'type-reconfigured'; Cache = @('CMAKE_BUILD_TYPE:STRING=Debug'); Bin = 'bin'; BuildInfo = '{"config": "Release", "ipo": "ON"}'; Expected = 'Release' }
            @{ Name = 'type-no-cache'; Cache = $null; Bin = 'bin'; BuildInfo = '{"config": "Release"}'; Expected = 'Release' }
            @{ Name = 'type-multi-config'; Cache = @(); Bin = 'bin\RelWithDebInfo'; BuildInfo = '{"config": "RelWithDebInfo"}'; Expected = 'RelWithDebInfo' }
            @{ Name = 'type-no-buildinfo'; Cache = @('CMAKE_BUILD_TYPE:STRING=Debug'); Bin = 'bin'; BuildInfo = $null; Expected = 'Debug' }
            @{ Name = 'type-no-buildinfo-multi-config'; Cache = @(); Bin = 'bin\Release'; BuildInfo = $null; Expected = 'Release' }
            @{ Name = 'type-empty-config'; Cache = @('CMAKE_BUILD_TYPE:STRING=Debug'); Bin = 'bin'; BuildInfo = '{"config": "", "ipo": "ON"}'; Expected = 'Debug' }
            @{ Name = 'type-non-string-config'; Cache = @('CMAKE_BUILD_TYPE:STRING=Debug'); Bin = 'bin'; BuildInfo = '{"config": 7}'; Expected = 'Debug' }
        )) {
        $tree = Join-Path $root "build\$($case.Name)"
        $binaryDir = Join-Path $tree $case.Bin
        New-Item -ItemType Directory -Path $binaryDir | Out-Null
        if ($null -ne $case.Cache) { Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value $case.Cache }
        if ($null -ne $case.BuildInfo) { [IO.File]::WriteAllText((Join-Path $binaryDir 'TaskSmackBenchmarks.buildinfo.json'), $case.BuildInfo) }
        $benchBin = Join-Path $binaryDir 'TaskSmackBenchmarks.cmd'
        $build = Get-BuildProvenance
        Assert-True ($build.build_type -ceq $case.Expected) "$($case.Name): build_type=$($build.build_type), expected $($case.Expected)"
    }

    # ── #1445 review: IPO is what the benchmark target is built with, not the cached variable ──
    # CompilerOptions.cmake turns IPO on through a normal variable, so the cached
    # CMAKE_INTERPROCEDURAL_OPTIMIZATION can say OFF, and a multi-config generator can set IPO per
    # configuration. benchmarks/CMakeLists.txt writes each configuration's effective IPO to
    # TaskSmackBenchmarks.buildinfo.json next to its binary; it wins over the cache. Trees without it
    # keep the old fallbacks. The same cases as tests/tools/test_bench_sh.py.
    $conflicting = @('CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF', 'TASKSMACK_ENABLE_IPO:BOOL=ON')
    $oldIpo = 'CMAKE_INTERPROCEDURAL_OPTIMIZATION'
    foreach ($case in @(
            @{ Name = 'ipo-single'; Cache = $conflicting; Bin = 'bin'; BuildInfo = '{"config": "Release", "ipo": "ON"}'; BuildType = 'Release'; Ipo = 'ON'; Source = 'buildinfo' }
            # Multi-config: no CMAKE_BUILD_TYPE; each bin/<Config>/ has its own build information,
            # each disagreeing with the generic cache value.
            @{ Name = 'ipo-multi-on'; Cache = @('CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=ON'); Bin = 'bin\Debug'; BuildInfo = '{"config": "Debug", "ipo": "OFF"}'; BuildType = 'Debug'; Ipo = 'OFF'; Source = 'buildinfo' }
            @{ Name = 'ipo-multi-off'; Cache = @('CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF'); Bin = 'bin\Release'; BuildInfo = '{"config": "Release", "ipo": "ON"}'; BuildType = 'Release'; Ipo = 'ON'; Source = 'buildinfo' }
            @{ Name = 'ipo-no-cache'; Cache = $null; Bin = 'bin'; BuildInfo = '{"config": "Release", "ipo": "ON"}'; BuildType = 'Release'; Ipo = 'ON'; Source = 'buildinfo' }
            # Unusable build information falls back to the cache.
            @{ Name = 'ipo-bad-value'; Cache = $conflicting; Bin = 'bin'; BuildInfo = '{"ipo": "on"}'; BuildType = 'Release'; Ipo = 'OFF'; Source = $oldIpo }
            @{ Name = 'ipo-bad-json'; Cache = $conflicting; Bin = 'bin'; BuildInfo = '{"ipo": '; BuildType = 'Release'; Ipo = 'OFF'; Source = $oldIpo }
            @{ Name = 'ipo-old-cache'; Cache = $conflicting; Bin = 'bin'; BuildInfo = $null; BuildType = 'Release'; Ipo = 'OFF'; Source = $oldIpo }
            @{ Name = 'ipo-option-only'; Cache = @('TASKSMACK_ENABLE_IPO:BOOL=ON'); Bin = 'bin'; BuildInfo = $null; BuildType = 'Release'; Ipo = 'ON'; Source = 'TASKSMACK_ENABLE_IPO' }
            @{ Name = 'ipo-unknown'; Cache = @(); Bin = 'bin'; BuildInfo = $null; BuildType = 'Release'; Ipo = $null; Source = $null }
        )) {
        $tree = Join-Path $root "build\$($case.Name)"
        $binaryDir = Join-Path $tree $case.Bin
        New-Item -ItemType Directory -Path $binaryDir | Out-Null
        if ($null -ne $case.Cache) {
            $single = if ($case.Bin -eq 'bin') { @('CMAKE_BUILD_TYPE:STRING=Release') } else { @() }
            Set-Content -LiteralPath (Join-Path $tree 'CMakeCache.txt') -Encoding utf8 -Value (@($single) + @($case.Cache))
        }
        if ($null -ne $case.BuildInfo) { [IO.File]::WriteAllText((Join-Path $binaryDir 'TaskSmackBenchmarks.buildinfo.json'), $case.BuildInfo) }
        $benchBin = Join-Path $binaryDir 'TaskSmackBenchmarks.cmd'
        $build = Get-BuildProvenance
        Assert-True ($build.build_type -ceq $case.BuildType -and $build.ipo -ceq $case.Ipo -and $build.ipo_source -ceq $case.Source) "$($case.Name): build_type=$($build.build_type) ipo=$($build.ipo) source=$($build.ipo_source)"
    }

    # ── #1445 review: the leak check itself, with a controlled user and home ────────────────────
    # A correctly scrubbed manifest must pass for a user whose name is also a flag word or a JSON
    # key (root: --sysroot=; build: the "build" section), and a real leak must still be found.
    $clean = '{"build": {"build_type": "Release", "compiler": "clang++"}, "benchmark": {"args": ["--sysroot=<abs>/sysroot", "-DBUILD=1", "-DCMAKE_BUILD=on", "--benchmark_filter=BM_Build"]}}' | ConvertFrom-Json
    foreach ($user in @('root', 'build')) {
        $found = Find-IdentityLeaks $clean -Tokens @($user) -Paths @("/home/$user", "C:\Users\$user")
        Assert-True ($found.Count -eq 0) "A clean manifest was reported as leaking for user '${user}': $($found -join '; ')"
        foreach ($leaky in @("-DBUILT_BY=$user", "E:/Users/$user/x", "/home/$user/src")) {
            $dirty = [pscustomobject]@{ benchmark = [pscustomobject]@{ args = @("-O2 $leaky") } }
            Assert-True ((Find-IdentityLeaks $dirty -Tokens @($user) -Paths @("/home/$user")).Count -gt 0) "'$leaky' was not reported for user '${user}'"
        }
    }
    # The checker covers an injected host name the same way.
    $hostTokens = @('bench-host-123', 'bench-host-123.example.com')
    $hostClean = [pscustomobject]@{ benchmark = [pscustomobject]@{ args = @('--benchmark_context=tsk_ctx_machine=<host>', '--benchmark_filter=BM_bench-host-123x') } }
    Assert-True ((Find-IdentityLeaks $hostClean -Tokens $hostTokens -Paths @()).Count -eq 0) 'A redacted host name was reported'
    foreach ($leaky in @('--benchmark_context=tsk_ctx_machine=bench-host-123', 'ssh://bench-host-123.example.com/x')) {
        $dirty = [pscustomobject]@{ benchmark = [pscustomobject]@{ args = @($leaky) } }
        Assert-True ((Find-IdentityLeaks $dirty -Tokens $hostTokens -Paths @()).Count -gt 0) "'$leaky' was not reported as a host-name leak"
    }

    # The checker skips exactly the fields the writer exempts, and checks machine.label against
    # the fields it is built from: a host named after the OS or a user named after a standard
    # build type is no leak, while the free-form fields are still checked.
    foreach ($case in @(
            @{ Os = 'Linux'; Version = '6.1'; Token = 'Linux' }
            @{ Os = 'Windows'; Version = '10.0.26100.0'; Token = 'Windows' }
            @{ Os = 'Linux'; Version = '6.1'; Token = 'Release' }
        )) {
        $machine = [pscustomobject]@{ label = $null; cpu_model = 'Some CPU'; logical_cores = 8; os_name = $case.Os; os_version = $case.Version; arch = 'X64' }
        $machine.label = Get-MachineLabel $machine
        $exempt = [pscustomobject]@{ build = [pscustomobject]@{ build_type = 'Release'; generator = 'Ninja' }; machine = $machine }
        $found = Find-IdentityLeaks $exempt -Tokens @($case.Token) -Paths @()
        Assert-True ($found.Count -eq 0) "Exempt fields were reported for '$($case.Token)': $($found -join '; ')"
        $machine.cpu_model = "$($case.Token) Box"
        Assert-True ((Find-IdentityLeaks $exempt -Tokens @($case.Token) -Paths @()).Count -gt 0) "A leak in machine.cpu_model was missed for '$($case.Token)'"
    }
    $tampered = [pscustomobject]@{ machine = [pscustomobject]@{ label = 'benchhost / 8 logical cores / Linux 6.1'; cpu_model = 'Some CPU'; logical_cores = 8; os_name = 'Linux'; os_version = '6.1'; arch = 'X64' } }
    Assert-True ((Find-IdentityLeaks $tampered -Tokens @('benchhost') -Paths @()).Count -gt 0) 'A label not built from the machine fields was not reported'
    $custom = [pscustomobject]@{ build = [pscustomobject]@{ build_type = 'benchuser' } }
    Assert-True ((Find-IdentityLeaks $custom -Tokens @('benchuser') -Paths @()).Count -gt 0) 'A custom build type named after the user was not reported'

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
    # A short home directory is still a home prefix (#1445 review); only a root or a bare drive
    # is never one. The same cases as test_bench_sh.py.
    $shortHome = Hide-Identity @('--benchmark_filter=/ab/data', '-DX=ab', 'C:\ab\x') -Homes @('/ab/', 'C:\ab') -User 'ab' -Hosts @()
    Assert-True (($shortHome -join ' ') -ceq '--benchmark_filter=<home>/data -DX=ab <home>\x') "Short home /ab: $($shortHome -join ' ')"
    $notHomes = Hide-Identity '/usr/bin C:\Windows D:/x' -Homes @('/', '\', '//', 'C:\', 'D:', '', $null) -User 'someone' -Hosts @()
    Assert-True ($notHomes -ceq '/usr/bin C:\Windows D:/x') "A root or bare drive is never a home prefix: $notHomes"

    # ── #1445 review: host names (short and FQDN) are hidden the same way, injected here ──────
    $hostCases = @(
        , @('--benchmark_context=tsk_ctx_machine=bench-host-123', '--benchmark_context=tsk_ctx_machine=<host>')
        , @('--benchmark_context=tsk_ctx_machine=BENCH-HOST-123.example.com', '--benchmark_context=tsk_ctx_machine=<host>')
        , @('ssh://bench-host-123.example.com/x', 'ssh://<host>/x')
        , @('xbench-host-123y -DHOST_bench-host-123', 'xbench-host-123y -DHOST_bench-host-123')
    )
    foreach ($case in $hostCases) {
        $got = Hide-Identity $case[0] -Homes @() -User 'someone' -Hosts @('bench-host-123', 'bench-host-123.example.com')
        Assert-True ($got -ceq $case[1]) "Identity pass for host 'bench-host-123': [$($case[0])] became [$got], expected [$($case[1])]"
    }
    $shortHost = Hide-Identity 'tsk_ctx_machine=ab' -Homes @() -User 'someone' -Hosts @('ab')
    Assert-True ($shortHost -ceq 'tsk_ctx_machine=ab') "A host name under 3 characters is left alone: $shortHost"
    # Hide-Identity's default host list holds this machine's own name.
    Assert-True (@(Get-HostNames) -contains [Environment]::MachineName) "Get-HostNames: $(@(Get-HostNames) -join ', ')"

    # ── #1445 review: the preset's own '-', '.' and '_' bound a user or host name in it too ────
    # The same cases as test_bench_sh.py.
    $presetHosts = @('bench-host-123', 'bench-host-123.example.com')
    foreach ($case in @(
            , @('benchuser', '<user>')
            , @('BENCHUSER', '<user>')
            , @('win-benchuser', 'win-<user>')
            , @('benchuser.release_x', '<user>.release_x')
            , @('bench-host-123', '<host>')
            , @('ci-bench-host-123.example.com-nightly', 'ci-<host>-nightly')
            , @('benchusers', 'benchusers')
            , @('x86_64-RelWithDebInfo', 'x86_64-RelWithDebInfo')
            , @('win-benchmark', 'win-benchmark')
        )) {
        $got = Hide-NameIdentity $case[0] -User 'benchuser' -Hosts $presetHosts
        Assert-True ($got -ceq $case[1]) "Preset [$($case[0])] became [$got], expected [$($case[1])]"
    }
    Assert-True ((Hide-NameIdentity 'ab-release' -User 'ab' -Hosts @()) -ceq 'ab-release') 'A user name under 3 characters is left alone in a preset'

    # ── #1445 review: the identity pass leaves validated categorical fields alone ──────────────
    # Host and user names that coincide with OS and compiler values: only free-form fields change.
    $sample = [ordered]@{
        schema_version = 1
        generator      = 'tools/bench.ps1'
        preset         = 'Linux-preset'
        git            = [ordered]@{ commit = 'a' * 40; branch = 'clang/Linux'; dirty = $false }
        build          = [ordered]@{ build_type = 'Release'; generator = 'Ninja'; compiler = 'clang'; compiler_id = 'Clang'; compiler_version = '22.1.8'; cxx_flags_sha256 = 'a' * 64 }
        benchmark      = [ordered]@{ args = [string[]]@('--benchmark_context=os=Windows', '-DHOST=Linux', '-DBY=clang', '-DCC=GNU'); raw_repetitions = $true }
        machine        = [ordered]@{ label = 'x'; cpu_model = 'Linux Box CPU'; logical_cores = 8; os_name = 'Linux'; os_version = '6.1'; arch = 'X64' }
    }
    foreach ($os in @('Linux', 'Windows')) {
        $hidden = Hide-ManifestIdentity $sample -Homes @() -User 'clang' -Hosts @($os, 'GNU')
        Assert-True ($hidden.machine.os_name -ceq 'Linux' -and $hidden.machine.os_version -ceq '6.1' -and $hidden.machine.arch -ceq 'X64') "Validated machine fields changed: $($hidden.machine | ConvertTo-Json -Compress)"
        Assert-True ($hidden.build.compiler_id -ceq 'Clang' -and $hidden.build.compiler_version -ceq '22.1.8' -and $hidden.build.build_type -ceq 'Release' -and $hidden.build.generator -ceq 'Ninja') "Validated build fields changed: $($hidden.build | ConvertTo-Json -Compress)"
        Assert-True ($hidden.git.commit -ceq ('a' * 40) -and $hidden.git.dirty -eq $false -and $hidden.schema_version -eq 1 -and $hidden.benchmark.raw_repetitions -eq $true) 'Schema, commit and boolean fields changed'
        # Free-form fields are still scrubbed.
        Assert-True ($hidden.build.compiler -ceq '<user>' -and $hidden.git.branch -ceq '<user>/' + $(if ($os -eq 'Linux') { '<host>' } else { 'Linux' })) "Free-form fields: compiler=$($hidden.build.compiler) branch=$($hidden.git.branch)"
        Assert-True ($hidden.build.cxx_flags_sha256 -ceq ('a' * 64)) 'The flag hash changed'
        Assert-True ((@($hidden.benchmark.args) -join ' ') -ceq "--benchmark_context=os=$(if ($os -eq 'Windows') { '<host>' } else { 'Windows' }) -DHOST=$(if ($os -eq 'Linux') { '<host>' } else { 'Linux' }) -DBY=<user> -DCC=<host>") "args: $(@($hidden.benchmark.args) -join ' ')"
        $expectedCpu = if ($os -eq 'Linux') { '<host> Box CPU' } else { 'Linux Box CPU' }
        Assert-True ($hidden.machine.cpu_model -ceq $expectedCpu -and $hidden.machine.label -ceq "$expectedCpu / 8 logical cores / Linux 6.1") "CPU and label: $($hidden.machine.cpu_model) | $($hidden.machine.label)"
    }

    # ── #1445 review: a user or host name in a custom kernel release is hidden ───────────────────
    # A Linux kernel built with CONFIG_LOCALVERSION reports its suffix in the OS version, which is
    # otherwise exempt. The same cases as test_bench_sh.py.
    foreach ($case in @(
            , @('Linux', '6.8.0-benchhost', 'someone', @('benchhost'), '6.8.0-<host>')
            , @('Linux', '6.8.0-45-generic', 'someone', @('benchhost'), '6.8.0-45-generic')
            , @('Linux', '6.8.0-benchuser_rt', 'benchuser', @(), '6.8.0-<user>_rt')
            , @('Linux', '6.8.0-benchhosts', 'someone', @('benchhost'), '6.8.0-benchhosts')
            # Windows reports a build number; the same treatment applies, for parity.
            , @('Windows', '10.0.26300.0', 'someone', @('benchhost'), '10.0.26300.0')
            , @('Windows', '10.0.26300-benchhost', 'someone', @('benchhost'), '10.0.26300-<host>')
        )) {
        $kernel = [ordered]@{ machine = [ordered]@{ label = 'x'; cpu_model = 'Some CPU'; logical_cores = 8; os_name = $case[0]; os_version = $case[1]; arch = 'X64' } }
        $hiddenKernel = (Hide-ManifestIdentity $kernel -Homes @() -User $case[2] -Hosts $case[3]).machine
        Assert-True ($hiddenKernel.os_version -ceq $case[4] -and $hiddenKernel.label -ceq "Some CPU / 8 logical cores / $($case[0]) $($case[4])") "OS version [$($case[1])]: $($hiddenKernel.os_version) | $($hiddenKernel.label)"
    }

    Write-Host 'bench.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
