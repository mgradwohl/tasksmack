# Tests that the format scripts and the coverage report find the project's sources when the checkout
# itself sits under a folder named build (#3528). They used to match their build/.git ignore pattern
# against each file's full path, so C:\build\tasksmack checked 0 files and passed:
#   - Get-FormatSourceFile (common.ps1), which check-format.ps1 and clang-format.ps1 list files with,
#     finds the same files through a junction at <temp>\Build\ts as from the checkout;
#   - check-format.ps1 and clang-format.ps1, run from a small copy under <temp>\Build\copy, check and
#     format its src\ files and skip its src\build\ ones, and check-format.ps1 fails on a copy with no
#     sources rather than passing with "All 0 files";
#   - Get-CoverageIgnoreRegex (common.ps1), coverage.ps1's -ignore-filename-regex, leaves out build\,
#     tests\, benchmarks\, _deps\ and .cache\ but not src\ under a root with build and tests in its path.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File. The two
# script runs are skipped (the rest still runs) when clang-format isn't installed.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$scratch = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-format-sources-$([guid]::NewGuid().ToString('N'))"
$junction = Join-Path $scratch 'Build\ts'
try {
    # ── The checkout, reached through a path with a Build folder in it ──────────────────────────────
    New-Item -ItemType Directory -Path (Split-Path -Parent $junction) -Force | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $repoRoot | Out-Null
    $direct = @(Get-FormatSourceFile -Root $repoRoot | ForEach-Object { [IO.Path]::GetRelativePath($repoRoot, $_.FullName) })
    $viaJunction = @(Get-FormatSourceFile -Root $junction | ForEach-Object { [IO.Path]::GetRelativePath($junction, $_.FullName) })
    Assert-True ($direct.Count -gt 0) "Get-FormatSourceFile found no sources in $repoRoot"
    Assert-True ($viaJunction.Count -eq $direct.Count) "Get-FormatSourceFile found $($viaJunction.Count) files through $junction, $($direct.Count) from $repoRoot"
    Assert-True ($null -eq (Compare-Object $direct $viaJunction)) "Get-FormatSourceFile listed different files through $junction"
    # The junction goes before anything is deleted recursively: only the link, never the checkout.
    [IO.Directory]::Delete($junction)

    # ── The scripts themselves, in a copy under a Build folder ──────────────────────────────────────
    $clangFormat = Find-LlvmTool -Name 'clang-format'
    if ($clangFormat) {
        $copy = Join-Path $scratch 'Build\copy'
        New-Item -ItemType Directory -Path (Join-Path $copy 'tools'), (Join-Path $copy 'src\build') -Force | Out-Null
        foreach ($file in 'check-format.ps1', 'clang-format.ps1', 'common.ps1') {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $copy 'tools')
        }
        foreach ($file in '.clang-format', '.pre-commit-config.yaml') {
            Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $copy
        }
        $formatted = "int answer()`n{`n    return 42;`n}`n"
        $unformatted = "int   answer( ){return 42;}`n"
        [IO.File]::WriteAllText((Join-Path $copy 'src\Good.cpp'), $formatted)
        # Inside a build directory of the copy's own: never checked or formatted.
        [IO.File]::WriteAllText((Join-Path $copy 'src\build\Generated.cpp'), $unformatted)

        $output = & pwsh -NoLogo -NoProfile -File (Join-Path $copy 'tools\check-format.ps1') *>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0 -and $output -match 'All 1 files are correctly formatted') "check-format.ps1 under a Build folder (exit $LASTEXITCODE):`n$output"

        [IO.File]::WriteAllText((Join-Path $copy 'src\Bad.cpp'), $unformatted)
        $output = & pwsh -NoLogo -NoProfile -File (Join-Path $copy 'tools\check-format.ps1') *>&1 | Out-String
        Assert-True ($LASTEXITCODE -ne 0 -and $output -match '1 of 2 files need formatting') "check-format.ps1 missed src\Bad.cpp under a Build folder (exit $LASTEXITCODE):`n$output"

        $output = & pwsh -NoLogo -NoProfile -File (Join-Path $copy 'tools\clang-format.ps1') *>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0 -and $output -match 'Formatted 2 files') "clang-format.ps1 under a Build folder (exit $LASTEXITCODE):`n$output"
        Assert-True ([IO.File]::ReadAllText((Join-Path $copy 'src\Bad.cpp')) -eq $formatted) 'clang-format.ps1 did not format src\Bad.cpp'
        Assert-True ([IO.File]::ReadAllText((Join-Path $copy 'src\build\Generated.cpp')) -eq $unformatted) 'clang-format.ps1 formatted src\build\Generated.cpp'

        Remove-Item -LiteralPath (Join-Path $copy 'src') -Recurse -Force
        $output = & pwsh -NoLogo -NoProfile -File (Join-Path $copy 'tools\check-format.ps1') *>&1 | Out-String
        Assert-True ($LASTEXITCODE -ne 0 -and $output -match 'No C\+\+ sources found') "check-format.ps1 passed a copy with no sources (exit $LASTEXITCODE):`n$output"
    } else {
        Write-Host 'clang-format not found; the check-format.ps1 and clang-format.ps1 runs were skipped'
    }

    # ── coverage.ps1's -ignore-filename-regex under a root with build and tests in its path ─────────
    # llvm-cov's regex is POSIX extended; this one uses only what .NET reads the same way.
    $root = 'C:\build\tests\task.smack (x86)'
    $regex = Get-CoverageIgnoreRegex -Root $root
    $kept = @("$root\src\App\Main.cpp", 'c:/BUILD/tests/TASK.SMACK (X86)/src/Domain/Sampler.h', "$root\src\build\Generated.h")
    $ignored = @("$root\tests\Domain\SamplerTests.cpp", "$root\benchmarks\Bench.h", "$root\build\win-coverage\Config.h",
        'c:/build/tests/task.smack (x86)/tests/Mocks/MockProbes.h', 'C:\deps\_deps\imgui-src\imgui.cpp',
        'C:\Users\me\.cache\fetchcontent\fmt-src\format.cc')
    foreach ($path in $kept) {
        Assert-True ($path -cnotmatch $regex) "coverage regex leaves out $path`n  regex: $regex"
    }
    foreach ($path in $ignored) {
        Assert-True ($path -cmatch $regex) "coverage regex keeps $path`n  regex: $regex"
    }

    Write-Host 'format sources tests passed'
}
finally {
    if (Test-Path -LiteralPath $junction) {
        [IO.Directory]::Delete($junction)
    }
    if (-not (Test-Path -LiteralPath $junction)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}
