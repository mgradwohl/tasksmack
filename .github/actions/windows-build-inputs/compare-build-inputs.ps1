<#
.SYNOPSIS
    Compare two Windows build-inputs manifests (#2802): did the CRT change, or the build?
.DESCRIPTION
    Takes two TaskSmack-<version>-<system>-build-inputs.json files written by
    record-build-inputs.ps1 and diffs their `inputs` objects (toolset, CRT/UCRT library hashes,
    SDK, runner image, compiler, commit), ignoring `provenance` (job, run id, package name).

    release.yml's verify-reproducibility-windows runs it beside its digest comparison, so a
    differing TaskSmack.exe reads "the runner image's CRT changed" when the inputs differ and "the
    build itself is not reproducible" when they match. The maintainer's post-release check runs it
    on the dry run's and the published release's manifests the same way:

        pwsh .github/actions/windows-build-inputs/compare-build-inputs.ps1 `
            dry-run/TaskSmack-0.9.1-windows-arm64-build-inputs.json `
            release/TaskSmack-0.9.1-windows-arm64-build-inputs.json

    Exit code 0: the inputs match. 1: they differ (each difference is printed as "- <a>" / "+ <b>"
    lines, with the flattened key). 2: a manifest could not be read.
.PARAMETER A
    The first manifest (the original: build-windows', or the published release's).
.PARAMETER B
    The second manifest (the rebuild's, or the dry run's).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)] [string] $A,
    [Parameter(Mandatory, Position = 1)] [string] $B
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-Manifest([string] $path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "No manifest at '$path'." }
    $manifest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if (-not $manifest.PSObject.Properties['inputs']) { throw "'$path' has no `inputs` object; is it a build-inputs manifest?" }
    return $manifest
}

# "<dotted.key> = <value>" lines, one per leaf, so two manifests diff as line sets whatever their nesting.
function ConvertTo-FlatLines($node, [string] $prefix) {
    $lines = @()
    if ($null -eq $node) {
        $lines += "$prefix = null"
    } elseif ($node -is [System.Management.Automation.PSCustomObject]) {
        foreach ($property in $node.PSObject.Properties) {
            $key = if ($prefix) { "$prefix.$($property.Name)" } else { $property.Name }
            $lines += ConvertTo-FlatLines $property.Value $key
        }
    } elseif ($node -is [System.Collections.IEnumerable] -and $node -isnot [string]) {
        $index = 0
        foreach ($element in $node) {
            $lines += ConvertTo-FlatLines $element "$prefix[$index]"
            $index++
        }
        if ($index -eq 0) { $lines += "$prefix = []" }
    } else {
        $lines += "$prefix = $node"
    }
    return $lines
}

function Describe-Manifest($manifest, [string] $path) {
    $p = $manifest.provenance
    $parts = @()
    foreach ($name in 'job', 'run_id', 'run_attempt', 'package') {
        if ($p.PSObject.Properties[$name] -and $p.$name) { $parts += "$name=$($p.$name)" }
    }
    return "$path ($($parts -join ', '))"
}

try {
    $first = Read-Manifest $A
    $second = Read-Manifest $B
} catch {
    Write-Host "::error::$($_.Exception.Message)"
    exit 2
}

Write-Host "A: $(Describe-Manifest $first $A)"
Write-Host "B: $(Describe-Manifest $second $B)"

$linesA = @(ConvertTo-FlatLines $first.inputs '')
$linesB = @(ConvertTo-FlatLines $second.inputs '')
$setA = [System.Collections.Generic.HashSet[string]]::new([string[]] $linesA)
$setB = [System.Collections.Generic.HashSet[string]]::new([string[]] $linesB)
$onlyA = @($linesA | Where-Object { -not $setB.Contains($_) })
$onlyB = @($linesB | Where-Object { -not $setA.Contains($_) })

if ($onlyA.Count -eq 0 -and $onlyB.Count -eq 0) {
    Write-Host "Build inputs match: same MSVC toolset, CRT/UCRT library hashes, Windows SDK, runner image and compiler."
    exit 0
}

Write-Host "Build inputs differ ($($onlyA.Count + $onlyB.Count) lines):"
foreach ($line in $onlyA) { Write-Host "- $line" }
foreach ($line in $onlyB) { Write-Host "+ $line" }
exit 1
