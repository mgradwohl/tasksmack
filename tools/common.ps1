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
