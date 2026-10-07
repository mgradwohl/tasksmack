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

    # The stub benchmark binary: a batch file running a script that writes Google Benchmark-shaped
    # JSON (holding this machine's real host name and a full executable path, as the real binary
    # does) to --benchmark_out, then exits with STUB_EXIT. STUB_OUTPUT=partial writes truncated
    # JSON, as a crash mid-run does.
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
if ($env:STUB_OUTPUT -eq 'partial') { $body = $body.Substring(0, 60) }
if ($env:STUB_OUTPUT -ne 'none') { Set-Content -LiteralPath $out -Value $body -Encoding utf8 }
exit [int]$env:STUB_EXIT
'@
    $stub = Join-Path $binDir 'TaskSmackBenchmarks.cmd'
    # The wrapper is ASCII (cmd.exe reads it in the console code page), so the PowerShell and stub
    # script paths, which can hold Unicode characters, come from inherited environment variables
    # that Invoke-Bench sets (and restores) around each run.
    Set-Content -LiteralPath $stub -Encoding ascii -Value "@`"%TASKSMACK_STUB_PWSH%`" -NoProfile -File `"%TASKSMACK_STUB_SCRIPT%`" %*`r`n@exit /b %ERRORLEVEL%"

    function Invoke-Bench {
        param([int]$StubExit, [string]$StubOutput = 'full', [string]$Name, [switch]$NativeErrorPromotion,
            [string[]]$Extra = @('--benchmark_filter=BM_X'), [string]$Binary = $stub, [string]$Script = $benchScript,
            [switch]$InProcessSeparator)
        $outDir = Join-Path $root $Name
        $variables = @{
            STUB_EXIT             = "$StubExit"
            STUB_OUTPUT           = $StubOutput
            TASKSMACK_STUB_PWSH   = $hostExe
            TASKSMACK_STUB_SCRIPT = $stubScript
        }
        $saved = @{}
        foreach ($name in $variables.Keys) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $variables[$name])
        }
        try {
            if ($NativeErrorPromotion) {
                # As from a session or profile that turns native exit codes into errors.
                $quote = { param([string]$s) "'" + $s.Replace("'", "''") + "'" }
                $command = '$ErrorActionPreference = ''Stop''; $PSNativeCommandUseErrorActionPreference = $true; ' +
                "& $(& $quote $benchScript) fake-preset -BenchmarkBinary $(& $quote $stub) -OutputDirectory $(& $quote $outDir) '--benchmark_filter=BM_X'"
                $log = & $hostExe -NoProfile -Command $command 2>&1 | Out-String
            }
            elseif ($InProcessSeparator) {
                # Called from PowerShell with no preset and a "--" before the benchmark flags.
                $quote = { param([string]$s) "'" + $s.Replace("'", "''") + "'" }
                $command = "& $(& $quote $Script) -BenchmarkBinary $(& $quote $Binary) -OutputDirectory $(& $quote $outDir) -- " + (@($Extra | ForEach-Object { & $quote $_ }) -join ' ')
                $log = & $hostExe -NoProfile -Command $command 2>&1 | Out-String
            }
            else {
                $log = & $hostExe -NoProfile -File $Script fake-preset -BenchmarkBinary $Binary -OutputDirectory $outDir @Extra 2>&1 | Out-String
            }
            $code = $LASTEXITCODE
        }
        finally {
            foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
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

    # With native-command error promotion on in the caller's session, the failure must still go
    # through the same path (manifest written, partial output redacted), not throw before it.
    $promoted = Invoke-Bench -StubExit 3 -Name 'promoted' -NativeErrorPromotion
    Assert-True ($promoted.ExitCode -ne 0) "bench.ps1 reported success with native error promotion on:`n$($promoted.Log)"
    Assert-True ($promoted.Log -like '*exited with code 3*') "Native error promotion bypassed the exit-code handling:`n$($promoted.Log)"
    Assert-True ($promoted.Manifest.Count -eq 1) "The manifest must be written with native error promotion on:`n$($promoted.Log)"
    Assert-True ($promoted.Result.Count -eq 1 -and (Get-Content -LiteralPath $promoted.Result[0].FullName -Raw | ConvertFrom-Json).context.host_name -eq 'redacted') 'The partial output must be redacted with native error promotion on'

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
        $manifest.build.compiler_version -eq '22.1.8' -and $manifest.build.ipo -eq 'ON') "Build provenance: $($manifest.build | ConvertTo-Json -Compress)"
    Assert-True ($manifest.benchmark.raw_repetitions -eq $true -and $manifest.benchmark.report_aggregates_only -eq $false) 'Raw repetitions must be kept'
    $recordedArgs = @($manifest.benchmark.args)
    Assert-True ($recordedArgs -notcontains '--benchmark_report_aggregates_only=true') 'Aggregates-only reporting must not be forced on'
    Assert-True ($recordedArgs -contains '--benchmark_filter=BM_X' -and $recordedArgs -contains '--benchmark_repetitions=10') "Benchmark args: $($recordedArgs -join ' ')"
    Assert-True ($recordedArgs -contains "--benchmark_out=$($ok.Result[0].Name)") 'The output path must be reduced to its file name'
    Assert-True ($manifest.machine.logical_cores -eq [Environment]::ProcessorCount -and $manifest.machine.os_name) 'Machine class'

    # No host name, user name or user-profile path anywhere in the manifest.
    # Absolute paths in flags: the checkout's become <source>/..., others <abs>/<file name>.
    Assert-True ($manifest.build.cxx_flags_config -ceq '-O3 -DNDEBUG -fprofile-instr-use="<source>/profiles/tasksmack.profdata" -fprofile-use=<abs>/x.profdata') "cxx_flags_config: $($manifest.build.cxx_flags_config)"
    $expectedFlags = @($flagForms | ForEach-Object { $_[1] }) -join ' '
    if ($manifest.build.cxx_flags -cne $expectedFlags) {
        $got = $manifest.build.cxx_flags
        $diff = @($flagForms | Where-Object { -not $got.Contains($_[1]) } | ForEach-Object { "$($_[0]) -> expected $($_[1])" })
        throw "cxx_flags: $got`nForms not scrubbed as expected:`n$($diff -join "`n")"
    }

    # The user and host names as tokens, the profile, temp and checkout paths as substrings.
    $identityPaths = @($env:USERPROFILE, [IO.Path]::GetTempPath().TrimEnd('\'), $repoRootPath, $repoRootForward, 'C:/Users', 'C:\Users')
    if ($env:USERPROFILE) { $identityPaths += $env:USERPROFILE.Replace('\', '/') }
    $leaks = Find-IdentityLeaks $manifest -Tokens @([Environment]::MachineName, [Environment]::UserName, $env:COMPUTERNAME, $env:USERNAME) -Paths $identityPaths
    Assert-True ($leaks.Count -eq 0) "The manifest leaks: $($leaks -join '; ')"
    Assert-True ($manifestText -notmatch 'host_?name|user_?name') 'The manifest must not have host or user name fields'

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

    # End to end: this machine's own host name passed in the benchmark args does not survive.
    if ([Environment]::MachineName.Length -ge 3) {
        $hostRun = Invoke-Bench -StubExit 0 -Name 'host' -Extra @('--benchmark_filter=BM_X', "--benchmark_context=runner=$([Environment]::MachineName)")
        Assert-True ($hostRun.ExitCode -eq 0 -and $hostRun.Manifest.Count -eq 1) "Host-name run failed:`n$($hostRun.Log)"
        $hostManifest = Get-Content -LiteralPath $hostRun.Manifest[0].FullName -Raw | ConvertFrom-Json
        Assert-True (@($hostManifest.benchmark.args) -contains '--benchmark_context=runner=<host>') "Host name in args: $(@($hostManifest.benchmark.args) -join ' ')"
        $hostLeaks = Find-IdentityLeaks $hostManifest -Tokens @([Environment]::MachineName, $env:COMPUTERNAME) -Paths @()
        Assert-True ($hostLeaks.Count -eq 0) "The manifest leaks the host name: $($hostLeaks -join '; ')"
    }

    # ── #1445 review: an extra --benchmark_out/--benchmark_out_format is refused before launch ──
    # Google Benchmark takes the last value, so the run would write somewhere the redaction and the
    # manifest never look (with the real host name in it).
    # A relative path, run from $root: pwsh -File splits an argument such as
    # --benchmark_out=C:\x at the drive colon, which would hide where the file went.
    $elsewhere = Join-Path $root 'elsewhere.json'
    foreach ($override in @('--benchmark_out=elsewhere.json', '--benchmark_out_format=csv')) {
        Push-Location -LiteralPath $root
        try { $refused = Invoke-Bench -StubExit 0 -Name "override-$([guid]::NewGuid().ToString('N'))" -Extra @('--benchmark_filter=BM_X', $override) }
        finally { Pop-Location }
        Assert-True ($refused.ExitCode -ne 0) "bench.ps1 accepted '$override':`n$($refused.Log)"
        Assert-True ($refused.Log -like '*-OutputDirectory*') "The refusal must point to -OutputDirectory:`n$($refused.Log)"
        Assert-True (-not (Test-Path -LiteralPath $elsewhere)) "The benchmark ran and wrote '$elsewhere' for '$override'"
        Assert-True ($refused.Result.Count -eq 0 -and $refused.Manifest.Count -eq 0) "Nothing may be written for '$override'"
    }

    # ── #1445 review: the manifest records the effective --benchmark_report_aggregates_only ─────
    # The last occurrence wins, parsed as Google Benchmark does (bare flag = true; f/n/0, false, no
    # and off = false, case-insensitively).
    foreach ($case in @(
            @{ Args = @('--benchmark_report_aggregates_only=true', '--benchmark_report_aggregates_only=FALSE'); Expected = $false }
            @{ Args = @('--benchmark_report_aggregates_only=no', '--benchmark_report_aggregates_only'); Expected = $true }
            @{ Args = @('--benchmark_report_aggregates_only=Yes', '--benchmark_report_aggregates_only=0'); Expected = $false }
            @{ Args = @('--benchmark_report_aggregates_only=off', '--benchmark_report_aggregates_only=t'); Expected = $true }
        )) {
        $run = Invoke-Bench -StubExit 0 -Name "aggregates-$([guid]::NewGuid().ToString('N'))" -Extra $case.Args
        Assert-True ($run.ExitCode -eq 0 -and $run.Manifest.Count -eq 1) "Run with $($case.Args -join ' ') failed:`n$($run.Log)"
        $benchmark = (Get-Content -LiteralPath $run.Manifest[0].FullName -Raw | ConvertFrom-Json).benchmark
        Assert-True ($benchmark.report_aggregates_only -eq $case.Expected -and $benchmark.raw_repetitions -eq (-not $case.Expected)) "$($case.Args -join ' '): report_aggregates_only=$($benchmark.report_aggregates_only), raw_repetitions=$($benchmark.raw_repetitions); expected $($case.Expected)"
    }

    # ── #1445 review: a compiler directory that does not match the cache's CMake is not guessed ─
    $staleDir = Join-Path $root 'build\stale-preset'
    New-Item -ItemType Directory -Path (Join-Path $staleDir 'bin'), (Join-Path $staleDir 'CMakeFiles\4.0.0') | Out-Null
    Copy-Item -LiteralPath $stub -Destination (Join-Path $staleDir 'bin')
    Set-Content -LiteralPath (Join-Path $staleDir 'CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4'
        'CMAKE_CACHE_MINOR_VERSION:INTERNAL=2'
        'CMAKE_CACHE_PATCH_VERSION:INTERNAL=0'
    )
    Set-Content -LiteralPath (Join-Path $staleDir 'CMakeFiles\4.0.0\CMakeCXXCompiler.cmake') -Encoding utf8 -Value 'set(CMAKE_CXX_COMPILER_ID "Clang")', 'set(CMAKE_CXX_COMPILER_VERSION "21.1.0")'
    $stale = Invoke-Bench -StubExit 0 -Name 'stale' -Binary (Join-Path $staleDir 'bin\TaskSmackBenchmarks.cmd')
    Assert-True ($stale.ExitCode -eq 0 -and $stale.Manifest.Count -eq 1) "Stale-tree run failed:`n$($stale.Log)"
    $staleBuild = (Get-Content -LiteralPath $stale.Manifest[0].FullName -Raw | ConvertFrom-Json).build
    Assert-True ($null -eq $staleBuild.compiler_id -and $null -eq $staleBuild.compiler_version) "A stale compiler directory must not be used: $($staleBuild | ConvertTo-Json -Compress)"

    # ── #1445 review: the identity pass leaves a user name that is also a flag word alone ──────
    # Hide-Identity is taken from bench.ps1 itself and given a user named "build".
    $benchAst = [System.Management.Automation.Language.Parser]::ParseFile($benchScript, [ref]$null, [ref]$null)
    $hideIdentity = $benchAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Hide-Identity' }, $true) | Select-Object -First 1
    . ([scriptblock]::Create($hideIdentity.Extent.Text))
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
    $getHostNames = $benchAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-HostNames' }, $true) | Select-Object -First 1
    . ([scriptblock]::Create($getHostNames.Extent.Text))
    Assert-True (@(Get-HostNames) -contains [Environment]::MachineName) "Get-HostNames: $(@(Get-HostNames) -join ', ')"

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
    $multi = Invoke-Bench -StubExit 0 -Name 'multi' -Binary (Join-Path $multiDir 'bin\RelWithDebInfo\TaskSmackBenchmarks.cmd')
    Assert-True ($multi.ExitCode -eq 0 -and $multi.Manifest.Count -eq 1) "Multi-config run failed:`n$($multi.Log)"
    $multiBuild = (Get-Content -LiteralPath $multi.Manifest[0].FullName -Raw | ConvertFrom-Json).build
    Assert-True ($multiBuild.build_type -eq 'RelWithDebInfo' -and $multiBuild.generator -eq 'Ninja Multi-Config' -and
        $multiBuild.cxx_flags_config -eq '-O2 -g -DNDEBUG' -and $multiBuild.compiler_version -eq '22.1.8') "Multi-config build provenance: $($multiBuild | ConvertTo-Json -Compress)"

    # ── #1445 review: a checkout under a Unicode path, outside git ─────────────────────────────
    # bench.ps1 copied into <root>/ch<e-acute>ckout/tools: its own checkout maps to <source> through
    # the UTF-8 cache, and the missing git repository leaves the git fields unknown, not an error.
    $checkout = Join-Path $root "ch$([char]0x00E9)ckout"
    New-Item -ItemType Directory -Path (Join-Path $checkout 'tools'), (Join-Path $checkout 'build\uni\bin') | Out-Null
    Copy-Item -LiteralPath $benchScript -Destination (Join-Path $checkout 'tools')
    Copy-Item -LiteralPath $stub -Destination (Join-Path $checkout 'build\uni\bin')
    $checkoutForward = $checkout.Replace('\', '/')
    Set-Content -LiteralPath (Join-Path $checkout 'build\uni\CMakeCache.txt') -Encoding utf8 -Value @(
        'CMAKE_BUILD_TYPE:STRING=Release'
        "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -fprofile-instr-use=`"$checkoutForward/profiles/tasksmack.profdata`" -DAUTHOR=Jos$([char]0x00E9)"
    )
    $uni = Invoke-Bench -StubExit 0 -Name 'uni' -Script (Join-Path $checkout 'tools\bench.ps1') -Binary (Join-Path $checkout 'build\uni\bin\TaskSmackBenchmarks.cmd')
    Assert-True ($uni.ExitCode -eq 0 -and $uni.Manifest.Count -eq 1) "Unicode-checkout run failed:`n$($uni.Log)"
    $uniManifest = Get-Content -LiteralPath $uni.Manifest[0].FullName -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-True ($uniManifest.build.cxx_flags_config -ceq "-O3 -fprofile-instr-use=`"<source>/profiles/tasksmack.profdata`" -DAUTHOR=Jos$([char]0x00E9)") "Unicode checkout flags: $($uniManifest.build.cxx_flags_config)"
    Assert-True ($null -eq $uniManifest.git.commit -and $null -eq $uniManifest.git.dirty) "Outside git, the git fields must be unknown: $($uniManifest.git | ConvertTo-Json -Compress)"
    # Self-review: in a repository on a non-ASCII branch, the branch name arrives intact (git
    # writes UTF-8; tests/tools/test_bench_sh.py checks the same for bench.sh).
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $branchName = "f$([char]0x00EB)ature"
        & git -C $checkout init -q -b $branchName 2>&1 | Out-Null
        & git -C $checkout -c user.name=bench-test -c user.email=bench-test@example.invalid commit -q --allow-empty -m init 2>&1 | Out-Null
        $branchRun = Invoke-Bench -StubExit 0 -Name 'uni-branch' -Script (Join-Path $checkout 'tools\bench.ps1') -Binary (Join-Path $checkout 'build\uni\bin\TaskSmackBenchmarks.cmd')
        Assert-True ($branchRun.ExitCode -eq 0 -and $branchRun.Manifest.Count -eq 1) "Unicode-branch run failed:`n$($branchRun.Log)"
        $branchGit = (Get-Content -LiteralPath $branchRun.Manifest[0].FullName -Raw -Encoding utf8 | ConvertFrom-Json).git
        Assert-True ($branchGit.branch -ceq $branchName -and $branchGit.commit -match '^[0-9a-f]{40}$' -and $branchGit.dirty -eq $false) "Unicode branch: $($branchGit | ConvertTo-Json -Compress)"
    }

    # ── Self-review: output that cannot be redacted after exit 0 is deleted, not left behind ───
    $garbled = Invoke-Bench -StubExit 0 -StubOutput 'partial' -Name 'garbled'
    Assert-True ($garbled.ExitCode -ne 0) "Unredactable output was reported as a success:`n$($garbled.Log)"
    Assert-True ($garbled.Log -like '*could not be redacted*') "The failure must say why:`n$($garbled.Log)"
    Assert-True ($garbled.Result.Count -eq 0) 'Unredactable output must be deleted'

    # ── Self-review: a run in the same second as an earlier one does not overwrite it ──────────
    # Results already sit under every name this run could pick in the next 30 seconds.
    $collideDir = Join-Path $root 'collide'
    New-Item -ItemType Directory -Path $collideDir | Out-Null
    $start = Get-Date
    $placeholders = foreach ($offset in 0..30) {
        $path = Join-Path $collideDir "fake-preset-$($start.AddSeconds($offset).ToString('yyyyMMdd-HHmmss')).json"
        Set-Content -LiteralPath $path -Value 'placeholder' -Encoding utf8
        $path
    }
    $collide = Invoke-Bench -StubExit 0 -Name 'collide'
    Assert-True ($collide.ExitCode -eq 0) "Same-second run failed:`n$($collide.Log)"
    foreach ($path in $placeholders) { Assert-True ((Get-Content -LiteralPath $path -Raw).Trim() -eq 'placeholder') "An earlier result was overwritten: $path" }
    $suffixed = @($collide.Result | Where-Object { $_.Name -match '^fake-preset-\d{8}-\d{6}-2\.json$' })
    Assert-True ($suffixed.Count -eq 1 -and (Test-Path -LiteralPath ($suffixed[0].FullName -replace '\.json$', '.manifest.json'))) "Expected a -2 result and manifest: $(@($collide.Result.Name) -join ', ')"

    # ── Self-review: `& bench.ps1 -- --benchmark_filter=...` from PowerShell, as documented ─────
    # PowerShell ends its own parameters at "--", so the first benchmark flag binds to -Preset.
    $separator = Invoke-Bench -StubExit 0 -Name 'separator' -InProcessSeparator
    Assert-True ($separator.ExitCode -eq 0 -and $separator.Manifest.Count -eq 1) "In-process -- run failed:`n$($separator.Log)"
    Assert-True ($separator.Result[0].Name -like 'win-benchmark-*.json') "The default preset must be used: $($separator.Result[0].Name)"
    $separatorArgs = @((Get-Content -LiteralPath $separator.Manifest[0].FullName -Raw | ConvertFrom-Json).benchmark.args)
    Assert-True ($separatorArgs -contains '--benchmark_filter=BM_X') "The flag must reach the benchmark: $($separatorArgs -join ' ')"

    Write-Host 'bench.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
