<#
.SYNOPSIS
    Run clang-tidy on source files (Windows).
.DESCRIPTION
    Configures if needed, then runs clang-tidy with parallel execution.
    Uses .clang-tidy configuration from project root.
.PARAMETER BuildType
    Build type to use: debug, relwithdebinfo (default: debug)
.PARAMETER ShowDetails
    Show verbose output with per-file progress
.PARAMETER Jobs
    Number of parallel jobs (default: number of CPU cores)
.PARAMETER Files
    Specific files to analyze (default: all source files)
.PARAMETER ChangedOnly
    Only analyze C++ files changed on this branch (vs. its merge-base with main, plus untracked).
    Ignored when -Files is given.
.PARAMETER CacheDir
    Skip files that passed before with identical inputs, and record clean results in this
    directory (tools/tidy-cache.py; needs python and clang-scan-deps)
.PARAMETER BuildDir
    Use the compile_commands.json in this directory, any configured tree, instead of
    build\win-<BuildType>. Never configures: the module and PCH flags are stripped in a temporary
    copy, and the tree is left untouched (like clang-tidy.sh --build-dir). Not with a BuildType.
.EXAMPLE
    .\clang-tidy.ps1
.EXAMPLE
    .\clang-tidy.ps1 -ShowDetails -Jobs 8
.EXAMPLE
    .\clang-tidy.ps1 -Files src/Domain/ProcessModel.cpp
.EXAMPLE
    .\clang-tidy.ps1 -ChangedOnly
.EXAMPLE
    .\clang-tidy.ps1 -BuildDir build\win-debug -Files src/Platform/Windows/WindowsSystemProbe.cpp
.NOTES
    CI's Windows clang-tidy runs this script. tools/clang-tidy.sh hands its arguments to it when it
    runs in Git Bash, MSYS2 or Cygwin (#3500), so a local run from either shell uses CI's flags.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo")]
    [string]$BuildType = "debug",

    [switch]$ShowDetails,

    [int]$Jobs = 0,

    [string[]]$Files = @(),

    [switch]$ChangedOnly,

    [string]$CacheDir = "",

    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
# clang-tidy, git and cmake write UTF-8 (paths, diagnostics); decode their output as UTF-8 (#1648).
# Without a console (a detached process) setting it throws; the default is kept then.
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir "common.ps1")
# CMake presets derive compiler paths from LLVM_ROOT; ensure it is set
# before any cmake --preset recovery path below runs.
Initialize-LlvmRoot

# Limit header diagnostics to project headers.
# Note: clang-tidy requires --header-filter to be set when using --exclude-header-filter.
# clang-tidy matches the filters against a header's path as clang spelled it, and on Windows that is
# C:\a\b, C:/a/b or a mix: CMake writes -I flags with "/" and the main file with "\", and a quoted
# include found next to its includer takes the includer's separators (#3500). So every separator in
# the checkout's path matches either one, and the drive letter either case.
$ProjectRootRegex = [regex]::Escape($ProjectRoot) -replace '\\\\', '[\\/]'
if ($ProjectRootRegex -match '^([A-Za-z]):') {
    $ProjectRootRegex = "[$($Matches[1].ToUpperInvariant())$($Matches[1].ToLowerInvariant())]" + $ProjectRootRegex.Substring(1)
}
$HeaderFilterRegex = "^$ProjectRootRegex[\\/](src|tests)[\\/]"

# Exclude generated/build trees and the other platform's folder.
# gladsources is generated under build\<preset>\gladsources.
$ExcludeHeaderFilterRegex = "^$ProjectRootRegex[\\/](build|dist|coverage|\.cache)[\\/]|^$ProjectRootRegex[\\/]src[\\/]Platform[\\/]Linux[\\/]|^$ProjectRootRegex.*[\\/]gladsources[\\/]"

# Build directory: build\win-<BuildType>, which this script configures when needed and whose stripped
# database goes to its clang-tidy-compdb folder; or a -BuildDir, which is only read, and whose
# stripped database goes to a temporary folder removed on exit.
$ExternalCompdb = [bool]$BuildDir
if ($ExternalCompdb) {
    if ($PSBoundParameters.ContainsKey('BuildType')) {
        Write-Error "Give a BuildType ($BuildType) or -BuildDir, not both." -ErrorAction Continue
        exit 1
    }
    if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
        Write-Error "-BuildDir ${BuildDir}: no such directory." -ErrorAction Continue
        exit 1
    }
    $BuildDir = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
    $CompileCommandsJson = Join-Path $BuildDir "compile_commands.json"
    if (-not (Test-Path -LiteralPath $CompileCommandsJson)) {
        Write-Error "-BuildDir $BuildDir has no compile_commands.json. Configure that tree first: cmake --preset <its preset>." -ErrorAction Continue
        exit 1
    }
    $TidyCompdbDir = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-clang-tidy-compdb-$([guid]::NewGuid().ToString('N'))"
} else {
    $BuildDir = Join-Path $ProjectRoot "build\win-$BuildType"
    $CompileCommandsJson = Join-Path $BuildDir "compile_commands.json"
    $TidyCompdbDir = Join-Path $BuildDir "clang-tidy-compdb"
}
$CompileCommandsTidy = Join-Path $TidyCompdbDir "compile_commands.json"

# The first Python that runs: the project venv's, then python, python3 and py -3 on PATH. Each is
# started once, so the Microsoft Store's python/python3 stub (a WindowsApps alias that only prints
# "Python was not found" and fails) is skipped instead of chosen (#3500). Returns the interpreter's
# own path (sys.executable), or $null.
function Find-WorkingPython {
    $candidates = @(
        @{ Exe = (Join-Path $ProjectRoot ".venv\Scripts\python.exe"); Args = @() }
        @{ Exe = "python"; Args = @() }
        @{ Exe = "python3"; Args = @() }
        @{ Exe = "py"; Args = @("-3") }
    )
    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate.Exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $command) { continue }
        try {
            $exe = & $command.Source @($candidate.Args) -I -c "import sys; print(sys.executable)" 2>$null | Select-Object -First 1
        } catch {
            continue
        }
        if ($LASTEXITCODE -eq 0 -and $exe -and (Test-Path -LiteralPath $exe)) {
            return $exe
        }
    }
    return $null
}

# Find clang-tidy
$ClangTidy = Find-LlvmTool -Name "clang-tidy"
if (-not $ClangTidy) {
    Write-Error "clang-tidy not found. Please install LLVM or set LLVM_ROOT." -ErrorAction Continue
    exit 1
}

Write-LlvmVersionSkewWarning -ToolPath $ClangTidy -Name 'clang-tidy'

if ($ShowDetails) {
    Write-Host "Using clang-tidy: $ClangTidy"
}

# Configure if needed (using CMake presets); never a -BuildDir
$NinjaFile = Join-Path $BuildDir "build.ninja"
if (-not $ExternalCompdb -and -not (Test-Path $NinjaFile)) {
    Write-Host "Build not configured. Running cmake --preset win-$BuildType..."
    & cmake --preset "win-$BuildType"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Ensure compile_commands.json exists (emitted by CMake at generate time)
if (-not $ExternalCompdb -and -not (Test-Path $CompileCommandsJson)) {
    Write-Host "compile_commands.json missing. Re-running cmake --preset win-$BuildType..."
    & cmake --preset "win-$BuildType"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Exit with $Code. (exit in a function ends the script; the finally below still removes a -BuildDir's
# temporary database.) Every result past this point exits through here, and
# $global:TaskSmackClangTidyExit records it, so clang-tidy.sh's hand-off counts a run as clean only
# when it got here with 0 (#3500 review).
function Exit-Tidy([int]$Code) {
    $global:TaskSmackClangTidyExit = $Code
    exit $Code
}

# Split a compile database "command" string the way clang does on Windows (CommandLineToArgvW's rules):
# whitespace separates arguments, "..." groups, 2n backslashes before a quote are n backslashes and the
# quote toggles, 2n+1 are n and a literal quote, and backslashes before anything else are literal.
function Split-WindowsCommandLine([string]$CommandLine) {
    if (-not $CommandLine.Contains('\"')) {
        # No backslash before a quote, so backslashes are all literal: an argument is a run of unquoted
        # non-space text and "..." groups (fast; CMake's commands are this form).
        return , @([regex]::Matches($CommandLine, '(?:[^\s"]+|"[^"]*"?)+') | ForEach-Object { $_.Value.Replace('"', '') })
    }
    $arguments = [Collections.Generic.List[string]]::new()
    $current = [Text.StringBuilder]::new()
    $inQuotes = $false
    $hasArgument = $false
    $i = 0
    while ($i -lt $CommandLine.Length) {
        $c = $CommandLine[$i]
        if ($c -eq '\') {
            $run = 0
            while ($i -lt $CommandLine.Length -and $CommandLine[$i] -eq '\') { $run++; $i++ }
            if ($i -lt $CommandLine.Length -and $CommandLine[$i] -eq '"') {
                $null = $current.Append('\', [Math]::Floor($run / 2))
                if ($run % 2 -eq 1) { $null = $current.Append('"'); $i++ }
            } else {
                $null = $current.Append('\', $run)
            }
            $hasArgument = $true
            continue
        }
        if ($c -eq '"') {
            $inQuotes = -not $inQuotes
            $hasArgument = $true
        } elseif (-not $inQuotes -and [char]::IsWhiteSpace($c)) {
            if ($hasArgument) { $arguments.Add($current.ToString()); $null = $current.Clear(); $hasArgument = $false }
        } else {
            $null = $current.Append($c)
            $hasArgument = $true
        }
        $i++
    }
    if ($hasArgument) { $arguments.Add($current.ToString()) }
    return , $arguments.ToArray()
}

# Drop the flags clang-tidy can't use from one entry's argument list: C++20 module @*.modmap files and
# -fmodule-output=..., and CMake's precompiled-header flags (the -Xclang -include-pch -Xclang <pch>
# group, the -Xclang -include -Xclang <...cmake_pch...> force-include, and -Xclang -fno-pch-timestamp).
function Remove-UnusableCompileFlags([string[]]$Arguments) {
    $kept = [Collections.Generic.List[string]]::new()
    $i = 0
    while ($i -lt $Arguments.Count) {
        $a = $Arguments[$i]
        $next = if ($i + 1 -lt $Arguments.Count) { $Arguments[$i + 1] } else { $null }
        $value = if ($i + 3 -lt $Arguments.Count -and $Arguments[$i + 2] -eq '-Xclang') { $Arguments[$i + 3] } else { $null }
        if ($a -like '@*.modmap' -or $a -like '-fmodule-output=*') { $i++; continue }
        if ($a -eq '-Xclang' -and $next -eq '-fno-pch-timestamp') { $i += 2; continue }
        if ($a -eq '-Xclang' -and $next -eq '-include-pch' -and $null -ne $value) { $i += 4; continue }
        if ($a -eq '-Xclang' -and $next -eq '-include' -and $null -ne $value -and $value -like '*cmake_pch*') { $i += 4; continue }
        $kept.Add($a)
        $i++
    }
    return , $kept.ToArray()
}

# Write compile database $Source to $Destination with Remove-UnusableCompileFlags applied to every entry.
# The entries are parsed, not edited as text (#3500 review): an "arguments" entry is filtered element by
# element, and a "command" entry is split with Windows rules and written back as "arguments".
function Write-SanitizedCompileCommands([string]$Source, [string]$Destination) {
    $entries = @(Get-Content -LiteralPath $Source -Raw -Encoding utf8 | ConvertFrom-Json)
    $sanitized = foreach ($entry in $entries) {
        $arguments = if ($entry.PSObject.Properties['arguments']) { [string[]]$entry.arguments }
        else { Split-WindowsCommandLine $entry.command }
        $out = [ordered]@{ directory = $entry.directory }
        $out.arguments = Remove-UnusableCompileFlags $arguments
        $out.file = $entry.file
        if ($entry.PSObject.Properties['output']) { $out.output = $entry.output }
        [PSCustomObject]$out
    }
    $json = ConvertTo-Json -InputObject @($sanitized) -Depth 4
    [IO.File]::WriteAllText($Destination, $json, [Text.UTF8Encoding]::new($false))
}

# Everything from here on can create the temporary database, so it is removed in finally however the
# script ends: through Exit-Tidy, or on a terminating error such as a malformed database (#3517).
try {
    # Generate a sanitized compile_commands.json for clang-tidy without touching the live file.
    # CMake emits two PCH-related flag groups per TU:
    #   1. -Xclang -include-pch -Xclang /path/cmake_pch.hxx.pch  (binary PCH)
    #   2. -Xclang -include    -Xclang /path/cmake_pch.hxx        (PCH source include)
    #   3. -Xclang -fno-pch-timestamp                            (PCH reproducibility flag, defensive)
    # clang-tidy cannot use any of these without a prior full build, so strip all.
    if ($ShowDetails) {
        Write-Host "Generating sanitized clang-tidy compilation database..."
    }
    Remove-Item $CompileCommandsTidy -Force -ErrorAction SilentlyContinue
    if ($ExternalCompdb) {
        if ($ShowDetails) {
            Write-Host "Using compile database: $CompileCommandsJson"
        }
    } else {
        & cmake --build $BuildDir --target generate-clang-tidy-compile-commands 2>$null
        if ($LASTEXITCODE -ne 0 -and $ShowDetails) {
            Write-Host "Falling back to local compile_commands sanitization..."
        }
    }
    if (-not (Test-Path $CompileCommandsTidy)) {
        $null = New-Item -ItemType Directory -Path $TidyCompdbDir -Force
        Write-SanitizedCompileCommands -Source $CompileCommandsJson -Destination $CompileCommandsTidy
    }

    # Several targets compile the same src/ files (the TaskSmackApp object library, the tests, ...), and
    # clang-tidy analyzes a file once per database entry. Keep one entry per file, the app's (#1626).
    # Not fatal: without it every file is still analyzed, just once per target that compiles it.
    $Python = Find-WorkingPython
    if (-not $Python) {
        Write-Warning "python not found (tried .venv, python, python3, py -3); compile database not de-duplicated (files may be analyzed more than once)."
    } else {
        if ($ShowDetails) {
            Write-Host "Using python: $Python"
        }
        $dedupeOutput = & $Python -I -X utf8 (Join-Path $ScriptDir "dedupe-compile-commands.py") $CompileCommandsTidy 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "compile database not de-duplicated (files may be analyzed more than once): $dedupeOutput"
        } elseif ($ShowDetails) {
            Write-Host $dedupeOutput
        }
    }

    # Determine files to analyze. Explicit -Files win over -ChangedOnly, as in clang-tidy.sh.
    if ($ChangedOnly -and $Files.Count -eq 0) {
        # Files this branch changes (committed, staged, unstaged, untracked); see Get-ChangedFileList. Not
        # tests/tools/fixtures/: test fixtures with deliberate findings, analyzed only when named in -Files.
        if ($ShowDetails) {
            Write-Host "Getting changed files from git..."
        }
        $SourceFiles = @(Get-ChangedFileList -Root $ProjectRoot |
            Where-Object { $_ -match '\.(cpp|h)$' } |
            Where-Object { $_ -notmatch 'Platform[/\\]Linux[/\\]' } |
            Where-Object { $_ -notmatch '^tests[/\\]tools[/\\]fixtures[/\\]' } |
            ForEach-Object { Join-Path $ProjectRoot $_ })
        if ($SourceFiles.Count -eq 0) {
            Write-Host "No changed C++ files found."
            Exit-Tidy 0
        }
        if ($ShowDetails) {
            Write-Host "Analyzing $($SourceFiles.Count) changed files..."
        }
    } else {
        $ChangedOnly = $false
        if ($Files.Count -eq 0) {
            # Get all source files from project, excluding other-platform files
            $SourceFiles = @()
            $SourceDirs = @("src")
            foreach ($dir in $SourceDirs) {
                $fullDir = Join-Path $ProjectRoot $dir
                if (Test-Path $fullDir) {
                    $SourceFiles += Get-ChildItem -Path $fullDir -Recurse -Include "*.cpp" |
                        Where-Object { $_.FullName -notmatch '\\Platform\\Linux\\' } |
                        Select-Object -ExpandProperty FullName
                }
            }
        } else {
            $SourceFiles = $Files | ForEach-Object {
                if ([System.IO.Path]::IsPathRooted($_)) { $_ }
                else { Join-Path $ProjectRoot $_ }
            }
        }
    }

    if ($SourceFiles.Count -eq 0) {
        Write-Host "Error: No source files found to analyze." -ForegroundColor Red
        Exit-Tidy 1
    }

    # Determine number of jobs
    if ($Jobs -le 0) {
        $Jobs = [Environment]::ProcessorCount
    }

    $ConfigFile = Join-Path $ProjectRoot ".clang-tidy"

    # Windows-only check exclusions, appended to .clang-tidy's Checks (which Linux keeps in full).
    # - clang-analyzer-optin.core.EnumCastOutOfRange: MSVC STL false positive. std::filesystem::read_symlink
    #   ORs two __std_fs_file_flags bits (_BITMASK_OPS in <xfilesystem_abi.h>), and the analyzer reports
    #   the combined value as outside the enum's range. The report is located in that system header, so
    #   a NOLINT (or [[clang::suppress]]) in our code can't silence it. Reached via App/UserConfig.cpp.
    $WindowsCheckExclusions = "-clang-analyzer-optin.core.EnumCastOutOfRange"

    # Each task carries the file's result-cache key ("-" when its result isn't cached).
    $Tasks = @($SourceFiles | ForEach-Object { [PSCustomObject]@{ Key = '-'; File = [string]$_ } })
    if ($CacheDir) {
        $CacheDir = [System.IO.Path]::GetFullPath($CacheDir)
        $fileList = New-TemporaryFile
        $selected = $null
        $cacheOk = $false
        try {
            Set-Content -LiteralPath $fileList -Value $SourceFiles -Encoding utf8
            if ($Python) {
                # A whole-tree run prunes markers no current file has, so a saved cache holds only live keys.
                # The flags below must match the clang-tidy call's: they are part of every cache key.
                $cacheArgs = @(
                    '-I', '-X', 'utf8', (Join-Path $ScriptDir "tidy-cache.py"), 'select',
                    '--cache-dir', $CacheDir, '--compdb', $CompileCommandsTidy, '--clang-tidy', $ClangTidy,
                    '--repo-root', $ProjectRoot, '--salt-file', $ConfigFile, '--salt-file', $PSCommandPath,
                    # --salt=VALUE, not --salt VALUE: a value starting with "-" would read as an option.
                    "--salt=$HeaderFilterRegex", "--salt=$ExcludeHeaderFilterRegex", "--salt=$WindowsCheckExclusions",
                    '--extra-arg=-std=c++23', '--extra-arg=-Wno-unknown-warning-option', '--files-from', $fileList)
                if ($Files.Count -eq 0 -and -not $ChangedOnly) { $cacheArgs += '--prune' }
                $selected = & $Python @cacheArgs
                $cacheOk = ($LASTEXITCODE -eq 0)
            }
        } finally {
            Remove-Item -LiteralPath $fileList -Force -ErrorAction SilentlyContinue
        }
        if (-not $cacheOk) {
            Write-Warning "clang-tidy result cache unavailable; analyzing every file."
            $CacheDir = ""
        } else {
            $Tasks = @($selected | Where-Object { $_ } | ForEach-Object {
                $key, $file = $_ -split "`t", 2
                [PSCustomObject]@{ Key = $key; File = $file }
            })
            if ($Tasks.Count -eq 0) {
                Write-Host "clang-tidy completed successfully (every file unchanged since a clean run)." -ForegroundColor Green
                Exit-Tidy 0
            }
        }
    }

    if ($ShowDetails) {
        Write-Host "Running clang-tidy on $($Tasks.Count) files with $Jobs parallel jobs..."
        Write-Host ""
    }

    # Process files in batches for parallel execution
    $results = $Tasks | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $file = $_.File
        $key = $_.Key
        $clangTidy = $using:ClangTidy
        $configFile = $using:ConfigFile
        $buildDir = $using:TidyCompdbDir
        $projectRoot = $using:ProjectRoot
        $showDetails = $using:ShowDetails

        # Get relative path for display
        $relativePath = $file
        if ($file.StartsWith($projectRoot)) {
            $relativePath = $file.Substring($projectRoot.Length + 1)
        }

        if ($showDetails) {
            Write-Host "  Analyzing: $relativePath"
        }

        $output = & $clangTidy `
            --config-file="$configFile" `
            --checks="$using:WindowsCheckExclusions" `
            --header-filter="$using:HeaderFilterRegex" `
            --exclude-header-filter="$using:ExcludeHeaderFilterRegex" `
            -p "$buildDir" `
            --extra-arg=-std=c++23 `
            --extra-arg=-Wno-unknown-warning-option `
            "$file" 2>&1

        $exitCode = $LASTEXITCODE

        [PSCustomObject]@{
            File = $relativePath
            Key = $key
            ExitCode = $exitCode
            Output = $output
        }
    }

    # Collect results
    $hasErrors = $false
    foreach ($result in $results) {
        # Record only a clean result (#1785); a file with findings is analyzed again next time.
        if ($CacheDir -and $result.Key -ne '-' -and $result.ExitCode -eq 0 -and -not ($result.Output -match "warning:|error:")) {
            Set-Content -LiteralPath (Join-Path $CacheDir $result.Key) -Value $result.File -Encoding utf8 -ErrorAction SilentlyContinue
        }
        if ($result.ExitCode -ne 0 -or ($result.Output -match "warning:|error:")) {
            if ($result.Output) {
                Write-Host $result.Output
            }
            if ($result.ExitCode -ne 0) {
                $hasErrors = $true
            }
        }
    }

    Write-Host ""
    if ($hasErrors) {
        Write-Host "clang-tidy found issues." -ForegroundColor Yellow
        Exit-Tidy 1
    } else {
        Write-Host "clang-tidy completed successfully." -ForegroundColor Green
        Exit-Tidy 0
    }
} finally {
    if ($ExternalCompdb) {
        Remove-Item -LiteralPath $TidyCompdbDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
