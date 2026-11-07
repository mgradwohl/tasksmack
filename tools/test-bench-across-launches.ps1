# Tests for tools/bench.ps1 -Baseline (#3448): a flagged benchmark is measured across separate launches
# with tools/bench-across-launches.py and the median across them is checked, as heavy-checks.yml does on
# Linux (#3476). Canned benchmark JSON only: a stub binary copies a prepared file per launch, and no real
# benchmark runs. Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$benchScript = Join-Path $PSScriptRoot 'bench.ps1'
$hostExe = (Get-Process -Id $PID).Path
# CTest passes CMake's interpreter; run directly, the first python3 or python on PATH (not the Store alias).
$python = if ($env:TASKSMACK_TEST_PYTHON -and (Test-Path -LiteralPath $env:TASKSMACK_TEST_PYTHON)) { $env:TASKSMACK_TEST_PYTHON }
else { @(Get-Command python3, python -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1 -ExpandProperty Source) }
if (-not $python) { throw 'No Python interpreter: set TASKSMACK_TEST_PYTHON or put python on PATH.' }

# Under a directory name with a space and non-ASCII characters, as a user profile can have.
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack bench-across-$([char]0x00FC)-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
$savedEnv = @{}
foreach ($name in 'TASKSMACK_STUB_HOST', 'TASKSMACK_STUB_SCRIPT', 'STUB_CANNED_DIR', 'STUB_COUNTER', 'TASKSMACK_BENCH_LOAD_SAMPLE_MS', 'BENCHMARK_REPORT_AGGREGATES_ONLY') {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    # A Google Benchmark-shaped run: each benchmark's repetitions at its time, and their median.
    function New-Run([hashtable]$Times) {
        $rows = foreach ($name in ($Times.Keys | Sort-Object)) {
            foreach ($repetition in 0, 1, 2) {
                [ordered]@{ name = $name; run_name = $name; run_type = 'iteration'; repetition_index = $repetition; iterations = 1000; real_time = $Times[$name]; cpu_time = $Times[$name]; time_unit = 'ns' }
            }
            [ordered]@{ name = "${name}_median"; run_name = $name; run_type = 'aggregate'; aggregate_name = 'median'; iterations = 3; real_time = $Times[$name]; cpu_time = $Times[$name]; time_unit = 'ns' }
        }
        return [ordered]@{ context = [ordered]@{ host_name = [Environment]::MachineName; executable = 'C:\some\dir\TaskSmackBenchmarks.exe' }; benchmarks = @($rows) }
    }
    function Write-Run([string]$Path, [hashtable]$Times) {
        New-Run $Times | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Path -Encoding utf8
    }

    $baseline = Join-Path $root 'baseline.json'
    Write-Run $baseline @{ BM_Steady = 100.0; BM_Noisy = 10.0 }

    # The stub binary: an ASCII batch file running a Python script that copies the canned run for this
    # launch (STUB_CANNED_DIR\launch-<n>.json, n counted in STUB_COUNTER) to the last --benchmark_out, or
    # exits 3 when there is none. It ignores the filter: what bench.ps1 passed is in each launch's manifest.
    $binDir = Join-Path $root 'bin'
    New-Item -ItemType Directory -Path $binDir | Out-Null
    $stubScript = Join-Path $binDir 'stub.py'
    Set-Content -LiteralPath $stubScript -Encoding utf8 -Value @'
import os, shutil, sys
out = [a for a in sys.argv[1:] if a.startswith("--benchmark_out=")][-1][len("--benchmark_out="):]
counter = os.environ["STUB_COUNTER"]
launch = (int(open(counter, encoding="ascii").read()) if os.path.exists(counter) else 0) + 1
with open(counter, "w", encoding="ascii") as stream:
    stream.write(str(launch))
canned = os.path.join(os.environ["STUB_CANNED_DIR"], f"launch-{launch}.json")
if not os.path.exists(canned):
    sys.exit(3)
shutil.copyfile(canned, out)
'@
    $stub = Join-Path $binDir 'TaskSmackBenchmarks.cmd'
    # The interpreter and script paths can hold non-ASCII characters, so they come from the environment.
    Set-Content -LiteralPath $stub -Encoding ascii -Value "@`"%TASKSMACK_STUB_HOST%`" `"%TASKSMACK_STUB_SCRIPT%`" %*`r`n@exit /b %ERRORLEVEL%"
    [Environment]::SetEnvironmentVariable('TASKSMACK_STUB_HOST', $python)
    [Environment]::SetEnvironmentVariable('TASKSMACK_STUB_SCRIPT', $stubScript)
    [Environment]::SetEnvironmentVariable('TASKSMACK_BENCH_LOAD_SAMPLE_MS', '20')
    [Environment]::SetEnvironmentVariable('BENCHMARK_REPORT_AGGREGATES_ONLY', [NullString]::Value)

    # One bench.ps1 -Baseline run in a process of its own (the script exits with the check's code), over
    # the canned launches given as BM_Noisy's time in each: BM_Steady is 100 ns throughout.
    function Invoke-Scenario {
        param([string]$Name, [double[]]$NoisyTimes, [string[]]$More = @(), [string]$BaselinePath = $baseline)
        $directory = Join-Path $root $Name
        $canned = Join-Path $directory 'canned'
        New-Item -ItemType Directory -Path $canned | Out-Null
        for ($index = 0; $index -lt $NoisyTimes.Count; $index++) {
            # Launch 1 is the full run; the further ones hold only the flagged benchmark, as a filtered run does.
            $times = if ($index -eq 0) { @{ BM_Steady = 100.0; BM_Noisy = $NoisyTimes[$index] } } else { @{ BM_Noisy = $NoisyTimes[$index] } }
            Write-Run (Join-Path $canned "launch-$($index + 1).json") $times
        }
        $counter = Join-Path $directory 'launches.txt'
        [Environment]::SetEnvironmentVariable('STUB_CANNED_DIR', $canned)
        [Environment]::SetEnvironmentVariable('STUB_COUNTER', $counter)
        $out = Join-Path $directory 'out'
        $output = @(& $hostExe -NoProfile -File $benchScript fake-preset -BenchmarkBinary $stub -OutputDirectory $out -Core any -Baseline $BaselinePath -Python $python @More '--benchmark_filter=BM_' 2>&1)
        $code = $LASTEXITCODE
        $text = ($output | ForEach-Object { "$_" }) -join "`n"
        $launched = if (Test-Path -LiteralPath $counter) { [int](Get-Content -LiteralPath $counter -Raw) } else { 0 }
        $result = @(Get-ChildItem -LiteralPath $out -Filter '*.json' -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.manifest.json' })
        $work = if ($result.Count -eq 1) { [IO.Path]::ChangeExtension($result[0].FullName, '.across-launches') } else { $null }
        return [pscustomobject]@{ Name = $Name; Exit = $code; Output = $text; Launched = $launched; Result = $result; Work = $work }
    }
    function Get-LaunchManifests([string]$Work) {
        @(Get-ChildItem -LiteralPath $Work -Directory -Filter 'launch-*' | Sort-Object Name | ForEach-Object {
                $manifest = @(Get-ChildItem -LiteralPath $_.FullName -Filter '*.manifest.json' -File)
                Assert-True ($manifest.Count -eq 1) "One manifest in $($_.FullName)"
                Get-Content -LiteralPath $manifest[0].FullName -Raw | ConvertFrom-Json
            })
    }

    # Noise: BM_Noisy reads 2x slow in the full run only. It is flagged, re-measured in 4 more launches,
    # and the median across the 5 (10.2 ns) passes. BM_Steady is never re-run.
    $noise = Invoke-Scenario 'noise' @(20.0, 10.0, 10.5, 9.8, 10.2)
    Assert-True ($noise.Exit -eq 0) "noise: exit $($noise.Exit)`n$($noise.Output)"
    Assert-True ($noise.Launched -eq 5) "noise: $($noise.Launched) launches, not 5"
    Assert-True ($noise.Output -match 'Median across 5 separate launches') "noise: no per-launch report`n$($noise.Output)"
    Assert-True ($noise.Output -match "RESULT=.*$([regex]::Escape($noise.Result[0].Name))\s*$") "noise: RESULT= is not the last line`n$($noise.Output)"
    $manifests = Get-LaunchManifests $noise.Work
    Assert-True ($manifests.Count -eq 4) "noise: $($manifests.Count) launch directories, not 4"
    foreach ($manifest in $manifests) {
        # The plan's anchored filter comes last, after the caller's own, so Google Benchmark uses it.
        $filters = @($manifest.benchmark.args | Where-Object { $_ -like '--benchmark_filter=*' })
        Assert-True ($filters.Count -eq 2 -and $filters[-1] -ceq '--benchmark_filter=^(BM_Noisy)$') "noise: launch filters $($filters -join ', ')"
        Assert-True ($manifest.placement.core -ceq 'any') "noise: a launch did not keep -Core any"
        Assert-True ($manifest.exit_code -eq 0) 'noise: a launch manifest has no exit code 0'
    }
    $combined = Get-Content -LiteralPath (Join-Path $noise.Work 'combined.json') -Raw | ConvertFrom-Json
    $noisyRow = @($combined.benchmarks | Where-Object { $_.run_name -ceq 'BM_Noisy' })
    Assert-True ($noisyRow.Count -eq 1 -and $noisyRow[0].real_time -eq 10.2) "noise: combined BM_Noisy $($noisyRow | ConvertTo-Json -Compress)"
    Assert-True (@($noisyRow[0].tasksmack_launch_real_times).Count -eq 5) 'noise: five launch times'
    Assert-True (@($combined.benchmarks | Where-Object { $_.run_name -ceq 'BM_Steady' }).Count -eq 4) 'noise: BM_Steady kept as the full run had it'
    # A launch's output is redacted like any run's.
    $launchResult = @(Get-ChildItem -LiteralPath (Join-Path $noise.Work 'launch-2') -Filter '*.json' -File | Where-Object { $_.Name -notlike '*.manifest.json' })
    Assert-True ((Get-Content -LiteralPath $launchResult[0].FullName -Raw | ConvertFrom-Json).context.host_name -ceq 'redacted') 'noise: launch result not redacted'

    # A real regression: slow in 4 of the 5 launches. One fast launch doesn't pass it (not best-of-N).
    $real = Invoke-Scenario 'real' @(20.0, 20.0, 21.0, 10.0, 19.0)
    Assert-True ($real.Exit -eq 1) "real: exit $($real.Exit)`n$($real.Output)"
    Assert-True ($real.Launched -eq 5) "real: $($real.Launched) launches, not 5"
    Assert-True ($real.Output -match 'BM_Noisy') "real: the regression is not named`n$($real.Output)"

    # Nothing flagged: one launch, no re-measurement.
    $clean = Invoke-Scenario 'clean' @(10.5)
    Assert-True ($clean.Exit -eq 0 -and $clean.Launched -eq 1) "clean: exit $($clean.Exit), $($clean.Launched) launches`n$($clean.Output)"
    Assert-True (@(Get-ChildItem -LiteralPath $clean.Work -Directory -Filter 'launch-*').Count -eq 0) 'clean: a launch directory'

    # Over budget: plan refuses, the flag stands as a regression, and nothing is re-run.
    $budget = Invoke-Scenario 'budget' @(20.0) -More @('-BudgetSeconds', '0')
    Assert-True ($budget.Exit -eq 1 -and $budget.Launched -eq 1) "budget: exit $($budget.Exit), $($budget.Launched) launches`n$($budget.Output)"
    Assert-True ($budget.Output -match 'Over budget') "budget: no over-budget message`n$($budget.Output)"

    # -Launches and -CheckArgs: 2 more launches, and a 200% threshold the 2x full run is under.
    $loose = Invoke-Scenario 'loose' @(20.0) -More @('-CheckArgs', '--threshold=200')
    Assert-True ($loose.Exit -eq 0 -and $loose.Launched -eq 1) "loose: exit $($loose.Exit), $($loose.Launched) launches`n$($loose.Output)"
    $two = Invoke-Scenario 'two' @(20.0, 10.0, 10.0) -More @('-Launches', '2')
    Assert-True ($two.Exit -eq 0 -and $two.Launched -eq 3) "two: exit $($two.Exit), $($two.Launched) launches`n$($two.Output)"

    # A launch that fails (no canned run for launch 3: the stub exits 3) ends the script with its exit code.
    $failed = Invoke-Scenario 'failed' @(20.0, 10.0)
    Assert-True ($failed.Exit -eq 3 -and $failed.Launched -eq 3) "failed: exit $($failed.Exit), $($failed.Launched) launches`n$($failed.Output)"

    # Usage errors are exit 2, before anything runs.
    $missing = Invoke-Scenario 'missing' @(20.0) -BaselinePath (Join-Path $root 'no-such-baseline.json')
    Assert-True ($missing.Exit -eq 2 -and $missing.Launched -eq 0) "missing baseline: exit $($missing.Exit), $($missing.Launched) launches"
    $owned = Invoke-Scenario 'owned' @(20.0) -More @('-CheckArgs', '--current=x.json')
    Assert-True ($owned.Exit -eq 2 -and $owned.Launched -eq 0) "-CheckArgs --current: exit $($owned.Exit), $($owned.Launched) launches"

    # In process: Invoke-AcrossLaunches with a launcher that writes canned runs, for two flagged names.
    # The launcher gets the plan's filter as it is, every name anchored and escaped.
    $benchAst = [System.Management.Automation.Language.Parser]::ParseFile($benchScript, [ref]$null, [ref]$null)
    $function = $benchAst.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-AcrossLaunches' }, $false)
    . ([scriptblock]::Create($function.Extent.Text))
    $direct = Join-Path $root 'direct'
    New-Item -ItemType Directory -Path $direct | Out-Null
    $first = Join-Path $direct 'first.json'
    $names = @('BM_A/size:8', 'BM_B<int>/threads:2')
    Write-Run $first @{ 'BM_A/size:8' = 30.0; 'BM_B<int>/threads:2' = 30.0; BM_Steady = 100.0 }
    $directBaseline = Join-Path $direct 'baseline.json'
    Write-Run $directBaseline @{ 'BM_A/size:8' = 10.0; 'BM_B<int>/threads:2' = 10.0; BM_Steady = 100.0 }
    $seen = [Collections.Generic.List[string]]::new()
    $fake = {
        param([string]$Filter, [string]$Directory)
        $seen.Add($Filter)
        $path = Join-Path $Directory 'run.json'
        Write-Run $path @{ 'BM_A/size:8' = 10.0; 'BM_B<int>/threads:2' = 11.0 }
        [pscustomobject]@{ ExitCode = 0; Result = $path }
    }
    $code = Invoke-AcrossLaunches -Python $python -ToolsDirectory $PSScriptRoot -Baseline $directBaseline -First $first `
        -FirstManifest (Join-Path $direct 'first.manifest.json') -WorkDirectory (Join-Path $direct 'work') -Launches 4 `
        -BudgetSeconds 600 -CheckArgs @() -Launch $fake 6>$null
    Assert-True ($code -eq 0) "direct: exit $code"
    Assert-True ($seen.Count -eq 4 -and @($seen | Select-Object -Unique).Count -eq 1) "direct: filters $($seen -join ' ; ')"
    Assert-True ($seen[0] -ceq '^(BM_A/size:8|BM_B<int>/threads:2)$') "direct: filter $($seen[0])"
    Assert-True (@($names | Where-Object { $_ -notmatch $seen[0] }).Count -eq 0 -and 'BM_Steady' -notmatch $seen[0]) 'direct: the filter matches the flagged names only'

    Write-Host 'bench.ps1 -Baseline (across launches) tests passed'
}
finally {
    foreach ($name in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $(if ($null -eq $savedEnv[$name]) { [NullString]::Value } else { $savedEnv[$name] }))
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
