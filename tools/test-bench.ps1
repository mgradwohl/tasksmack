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

$benchScript = Join-Path $PSScriptRoot 'bench.ps1'
$hostExe = (Get-Process -Id $PID).Path
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-bench-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # A fake build tree, so the manifest's build fields are read from a CMakeCache.txt the way
    # they are for build/<preset>.
    $buildDir = Join-Path $root 'build\fake-preset'
    $binDir = Join-Path $buildDir 'bin'
    New-Item -ItemType Directory -Path $binDir, (Join-Path $buildDir 'CMakeFiles\4.0.0') | Out-Null
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Encoding ascii -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_GENERATOR:INTERNAL=Ninja'
        "CMAKE_CXX_COMPILER:FILEPATH=C:\Users\$([Environment]::UserName)\llvm\bin\clang++.exe"
        'CMAKE_CXX_FLAGS:STRING=-fms-compatibility'
        'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG'
        'TASKSMACK_ENABLE_IPO:BOOL=ON'
    )
    Set-Content -LiteralPath (Join-Path $buildDir 'CMakeFiles\4.0.0\CMakeCXXCompiler.cmake') -Encoding ascii -Value @(
        'set(CMAKE_CXX_COMPILER_ID "Clang")'
        'set(CMAKE_CXX_COMPILER_VERSION "22.1.8")'
    )

    # The stub benchmark binary: a batch file running a script that writes Google Benchmark-shaped
    # JSON (holding this machine's real host name and a full executable path, as the real binary
    # does) to --benchmark_out, then exits with STUB_EXIT. STUB_OUTPUT=partial writes truncated
    # JSON, as a crash mid-run does.
    $stubScript = Join-Path $binDir 'stub.ps1'
    Set-Content -LiteralPath $stubScript -Encoding utf8 -Value @'
$out = [Environment]::GetCommandLineArgs() | Where-Object { $_ -like '--benchmark_out=*' } | Select-Object -First 1
$out = $out.Substring('--benchmark_out='.Length)
$body = [ordered]@{
    context    = [ordered]@{ host_name = [Environment]::MachineName; executable = 'C:\some\dir\TaskSmackBenchmarks.exe' }
    benchmarks = @(
        [ordered]@{ name = 'BM_X'; run_name = 'BM_X'; run_type = 'iteration'; repetition_index = 0; real_time = 10.0; time_unit = 'ns' }
        [ordered]@{ name = 'BM_X'; run_name = 'BM_X'; run_type = 'iteration'; repetition_index = 1; real_time = 12.0; time_unit = 'ns' }
        [ordered]@{ name = 'BM_X_median'; run_name = 'BM_X'; run_type = 'aggregate'; aggregate_name = 'median'; real_time = 11.0; time_unit = 'ns' }
    )
} | ConvertTo-Json -Depth 5
if ($env:STUB_OUTPUT -eq 'partial') { $body = $body.Substring(0, 60) }
if ($env:STUB_OUTPUT -ne 'none') { Set-Content -LiteralPath $out -Value $body -Encoding utf8 }
exit [int]$env:STUB_EXIT
'@
    $stub = Join-Path $binDir 'TaskSmackBenchmarks.cmd'
    Set-Content -LiteralPath $stub -Encoding ascii -Value "@`"$hostExe`" -NoProfile -File `"$stubScript`" %*`r`n@exit /b %ERRORLEVEL%"

    function Invoke-Bench {
        param([int]$StubExit, [string]$StubOutput = 'full', [string]$Name)
        $outDir = Join-Path $root $Name
        $env:STUB_EXIT = "$StubExit"
        $env:STUB_OUTPUT = $StubOutput
        try {
            $log = & $hostExe -NoProfile -File $benchScript fake-preset -BenchmarkBinary $stub -OutputDirectory $outDir --benchmark_filter=BM_X 2>&1 | Out-String
            $code = $LASTEXITCODE
        }
        finally {
            Remove-Item Env:STUB_EXIT, Env:STUB_OUTPUT -ErrorAction SilentlyContinue
        }
        $result = @(Get-ChildItem -LiteralPath $outDir -Filter '*.json' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.manifest.json' })
        $manifest = @(Get-ChildItem -LiteralPath $outDir -Filter '*.manifest.json' -ErrorAction SilentlyContinue)
        return [pscustomobject]@{ ExitCode = $code; Log = $log; Result = $result; Manifest = $manifest }
    }

    # ── #1423: a benchmark binary that fails makes the script fail ──────────────────────────
    $failed = Invoke-Bench -StubExit 3 -Name 'failed'
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
    # the host name in it, and the script still fails with the benchmark's exit code.
    $crashed = Invoke-Bench -StubExit 5 -StubOutput 'partial' -Name 'crashed'
    Assert-True ($crashed.ExitCode -ne 0) "bench.ps1 reported success for a crashed benchmark:`n$($crashed.Log)"
    Assert-True ($crashed.Log -like '*exited with code 5*') "The crash must name the benchmark's exit code:`n$($crashed.Log)"
    Assert-True ($crashed.Result.Count -eq 0) 'Unparseable partial output must be deleted'

    # ── #1424: a successful run writes a provenance manifest beside the result ───────────────
    $ok = Invoke-Bench -StubExit 0 -Name 'ok'
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
    Assert-True ($manifest.git.commit -match '^[0-9a-f]{40}$' -and $manifest.git.dirty -is [bool]) "Git provenance: $($manifest.git | ConvertTo-Json -Compress)"
    Assert-True ($manifest.binary.name -eq 'TaskSmackBenchmarks.cmd') 'Binary name must be the leaf only'
    Assert-True ($manifest.binary.sha256 -eq (Get-FileHash -LiteralPath $stub -Algorithm SHA256).Hash.ToLowerInvariant()) 'Binary SHA-256'
    Assert-True ($manifest.build.build_type -eq 'Release' -and $manifest.build.compiler -eq 'clang++.exe' -and $manifest.build.compiler_id -eq 'Clang' -and
        $manifest.build.compiler_version -eq '22.1.8' -and $manifest.build.cxx_flags_config -eq '-O3 -DNDEBUG' -and $manifest.build.ipo -eq 'ON') "Build provenance: $($manifest.build | ConvertTo-Json -Compress)"
    Assert-True ($manifest.benchmark.raw_repetitions -eq $true -and $manifest.benchmark.report_aggregates_only -eq $false) 'Raw repetitions must be kept'
    $recordedArgs = @($manifest.benchmark.args)
    Assert-True ($recordedArgs -notcontains '--benchmark_report_aggregates_only=true') 'Aggregates-only reporting must not be forced on'
    Assert-True ($recordedArgs -contains '--benchmark_filter=BM_X' -and $recordedArgs -contains '--benchmark_repetitions=10') "Benchmark args: $($recordedArgs -join ' ')"
    Assert-True ($recordedArgs -contains "--benchmark_out=$($ok.Result[0].Name)") 'The output path must be reduced to its file name'
    Assert-True ($manifest.machine.logical_cores -eq [Environment]::ProcessorCount -and $manifest.machine.os_name) 'Machine class'

    # No host name, user name or user-profile path anywhere in the manifest.
    foreach ($identity in @([Environment]::MachineName, [Environment]::UserName, $env:COMPUTERNAME, $env:USERNAME, $env:USERPROFILE, [IO.Path]::GetTempPath().TrimEnd('\'))) {
        if ($identity -and $identity.Length -ge 3) {
            Assert-True ($manifestText.IndexOf($identity, [StringComparison]::OrdinalIgnoreCase) -lt 0) "The manifest contains '$identity'"
        }
    }
    Assert-True ($manifestText -notmatch 'host_?name|user_?name') 'The manifest must not have host or user name fields'

    Write-Host 'bench.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
