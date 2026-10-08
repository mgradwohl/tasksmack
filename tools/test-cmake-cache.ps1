# Tests for tools/cmake-cache.ps1, the CMakeCache.txt reader shared by bench.ps1 and
# profile-etw-common.ps1 (#1479), through profile-etw-common.ps1's Get-PresetBuildFlags.
# (tools/test-bench.ps1 covers the parse rules through bench.ps1.) Registered in CTest on Windows
# with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'profile-etw-common.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-cmake-cache-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # ── #1479: a custom build type's flags are read whatever characters its name holds ──────────
    # CMake writes CMAKE_CXX_FLAGS_<CONFIG> with the upper-cased build type as is, so '-', '.' and
    # '+' are part of the key; a value with trailing whitespace is written in single quotes.
    foreach ($case in @(
            @{ BuildType = 'ASan-UBSan'; Key = 'CMAKE_CXX_FLAGS_ASAN-UBSAN'; Flags = '-O1 -fsanitize=address,undefined' }
            @{ BuildType = 'Rel.With+Info'; Key = 'CMAKE_CXX_FLAGS_REL.WITH+INFO'; Flags = '-O2 -g' }
            @{ BuildType = 'Release'; Key = 'CMAKE_CXX_FLAGS_RELEASE'; Flags = '-O3 -DNDEBUG' }
        )) {
        $build = Join-Path $root "build-$($case.BuildType)"
        New-Item -ItemType Directory -Path $build | Out-Null
        [IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), (@(
                    '# This is the CMakeCache file.'
                    '//Flags used by the CXX compiler during all build types.'
                    "CMAKE_BUILD_TYPE:STRING=$($case.BuildType)"
                    "CMAKE_CXX_FLAGS:STRING='-fms-compatibility '"
                    "$($case.Key):STRING=$($case.Flags)"
                    '"KEY:WITH=SPECIALS":STRING=colon'
                    'TASKSMACK_ENABLE_IPO:BOOL=ON'
                ) -join "`r`n") + "`r`n")
        $flags = Get-PresetBuildFlags -BuildDirectory $build
        Assert-True ($flags.BuildType -ceq $case.BuildType) "$($case.BuildType): build type read as [$($flags.BuildType)]"
        Assert-True ($flags.CxxConfigFlags -ceq $case.Flags) "$($case.BuildType): configuration flags read as [$($flags.CxxConfigFlags)]"
        Assert-True ($flags.CxxFlags -ceq '-fms-compatibility ') "$($case.BuildType): CMAKE_CXX_FLAGS read as [$($flags.CxxFlags)]"
        Assert-True ($flags.Ipo -like 'ON*TASKSMACK_ENABLE_IPO*') "$($case.BuildType): IPO read as [$($flags.Ipo)]"
        $line = Format-PresetBuildFlags -Preset 'win-custom' -Flags $flags
        Assert-True ($line -like "*CMAKE_BUILD_TYPE=$($case.BuildType)*$($case.Flags)*") "$($case.BuildType): $line"
    }

    # The shared reader itself: the quoted key, case-sensitive keys, and the upper-casing.
    $cache = Read-CMakeCache -Path (Join-Path $root 'build-ASan-UBSan\CMakeCache.txt')
    Assert-True ($cache['KEY:WITH=SPECIALS'] -ceq 'colon' -and -not $cache.ContainsKey('cmake_build_type')) "Quoted or case-folded keys: $(@($cache.Keys) -join ', ')"
    Assert-True ((ConvertTo-CMakeUpper 'asan-ubsan.rel+info') -ceq 'ASAN-UBSAN.REL+INFO') 'ConvertTo-CMakeUpper'

    Write-Host 'cmake-cache.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
