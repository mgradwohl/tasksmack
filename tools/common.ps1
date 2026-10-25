<#
.SYNOPSIS
    Common PowerShell functions for TaskSmack build tools.
.DESCRIPTION
    Dot-source this file in other scripts: . "$PSScriptRoot\common.ps1"
    Counterpart of tools/common.sh for Windows scripts.
#>

# Find an LLVM tool by name (without extension).
# Search order: $env:LLVM_ROOT\bin, standard install locations, then PATH.
# Returns the full path, or $null if not found.
function Find-LlvmTool {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $candidates = @()
    if ($env:LLVM_ROOT) {
        $candidates += Join-Path $env:LLVM_ROOT "bin\$Name.exe"
    }
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles "LLVM\bin\$Name.exe"
    }
    $candidates += "C:\Program Files\LLVM\bin\$Name.exe"

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $fromPath = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if ($fromPath) {
        return $fromPath
    }

    return $null
}

# Print the LLVM major version for the given executable path, or $null if undetectable.
function Get-LlvmToolMajorVersion {
    [CmdletBinding()]
    [OutputType([int])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolPath
    )

    $versionOutput = & $ToolPath --version 2>$null | Out-String
    if ($versionOutput -match 'version (\d+)') {
        return [int]$Matches[1]
    }
    return $null
}

# The LLVM major version CI pins: the mirrors-clang-format `rev:` in .pre-commit-config.yaml, which
# moves with the LLVM_VERSION the CI workflows install. $null if it can't be read.
# Counterpart of pinned_llvm_major in common.sh.
function Get-PinnedLlvmMajor {
    [CmdletBinding()]
    [OutputType([int])]
    param()

    $config = Join-Path (Split-Path -Parent $PSScriptRoot) '.pre-commit-config.yaml'
    if (-not (Test-Path -LiteralPath $config)) {
        return $null
    }
    $text = Get-Content -LiteralPath $config -Raw -Encoding utf8
    if ($text -match 'mirrors-clang-format\s*\r?\n\s*rev:\s*v?(\d+)') {
        return [int]$Matches[1]
    }
    return $null
}

# Warn when an LLVM tool's major version differs from the one CI pins. Different majors can format
# the same code differently or report different clang-tidy findings, so a failure may be the
# toolchain rather than the tree (#916). Never fails. Counterpart of warn_llvm_version_skew.
function Write-LlvmVersionSkewWarning {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolPath,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $pinned = Get-PinnedLlvmMajor
    $localVersion = Get-LlvmToolMajorVersion -ToolPath $ToolPath
    if ($pinned -and $localVersion -and $pinned -ne $localVersion) {
        $hint = if ($Name -eq 'clang-format') {
            "'pre-commit run clang-format --all-files' uses the pinned version."
        } else {
            "install LLVM $pinned and point LLVM_ROOT at it for CI-identical results."
        }
        Write-Warning "$ToolPath is $Name $localVersion, but CI pins LLVM $pinned (.pre-commit-config.yaml). Results can differ from CI; $hint"
    }
}

# The directories holding C++ sources that the format scripts check and format (FORMAT_DIRS in
# common.sh), as full paths under -Root. pre-commit's clang-format hook formats every C++ file in
# the repo, and these are the directories that have any.
function Get-FormatSourceDir {
    [CmdletBinding()]
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    return @('src', 'tests', 'benchmarks') | ForEach-Object { Join-Path $Root $_ }
}

# List the files this branch changes, for the -ChangedOnly modes of clang-format.ps1 and
# clang-tidy.ps1: everything that differs from the merge-base with origin/main (or local main) --
# commits already on the branch, staged and unstaged edits -- plus untracked files that aren't
# ignored. Deleted files are left out. Falls back to HEAD when neither main ref exists. Paths are
# relative to the repository root. Counterpart of list_changed_files in common.sh (#1187).
function Get-ChangedFileList {
    [CmdletBinding()]
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $base = 'HEAD'
    foreach ($ref in @('origin/main', 'main')) {
        $null = & git -C $Root rev-parse --verify --quiet "$ref^{commit}" 2>$null
        if ($LASTEXITCODE -eq 0) {
            $mergeBase = & git -C $Root merge-base HEAD $ref 2>$null
            if ($LASTEXITCODE -eq 0 -and $mergeBase) {
                $base = "$mergeBase".Trim()
            }
            break
        }
    }

    # core.quotePath=false: non-ASCII paths come out as UTF-8 rather than quoted octal escapes.
    $changed = @(& git -C $Root -c core.quotePath=false diff --name-only --diff-filter=d $base 2>$null)
    $untracked = @(& git -C $Root -c core.quotePath=false ls-files --others --exclude-standard 2>$null)
    return @($changed + $untracked | Where-Object { $_ } | Sort-Object -Unique)
}

# Ensure $env:LLVM_ROOT is set for CMake presets (windows-base derives the
# compiler paths from it). If unset, derive it from a discovered LLVM tool
# located in a conventional <root>\bin layout.
function Initialize-LlvmRoot {
    [CmdletBinding()]
    param()

    if ($env:LLVM_ROOT) {
        return
    }

    $clang = Find-LlvmTool -Name "clang++"
    if ($clang) {
        $binDir = Split-Path -Parent $clang
        if ((Split-Path -Leaf $binDir) -eq 'bin') {
            $env:LLVM_ROOT = Split-Path -Parent $binDir
            Write-Host "LLVM_ROOT not set; derived '$env:LLVM_ROOT' from $clang"
            return
        }
    }

    Write-Error "LLVM_ROOT is not set and could not be derived. CMake presets require it; set LLVM_ROOT to your LLVM install directory."
    exit 1
}
