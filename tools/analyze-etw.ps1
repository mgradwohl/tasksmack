<#
.SYNOPSIS
    Analyze a TaskSmack ETW CPU trace using xperf.
.DESCRIPTION
    Exports module-level and function-level sampled CPU reports, then prints the top
    TaskSmack-specific modules and functions for a chosen process name / PID.

    Every run also checks whether the trace can be trusted (#873) and says so:
      - lost events and buffers, as a count and as a share of the events recorded;
      - symbol identity: the binary in the symbol directory must be the build the trace captured
        (its PDB signature and age are compared with the ones the trace recorded);
      - unresolved functions: the share of the target module's samples with no function name.
    The verdict (Valid / Degraded / Invalid, with reasons) is printed and written to
    <trace>-analysis.json. An Invalid trace is still fully exported for inspection, but the
    script exits with code 3 unless -AllowInvalid is passed, so it cannot silently pass a check.
.PARAMETER TracePath
    Path to the ETW trace (.etl).
.PARAMETER ProcessName
    Process name to filter in the exported xperf reports. Defaults to TaskSmack.exe.
.PARAMETER ProcessId
    Optional PID to disambiguate multiple processes with the same name.
.PARAMETER SymbolPath
    Optional symbol directory used for function decoding. Defaults to build/win-profile/bin.
    When analyzing traces captured from win-optimized, function decoding may be limited. The
    <ProcessName> binary in this directory is checked against the trace's recorded PDB signature.
.PARAMETER Top
    Number of rows to print in each summary.
.PARAMETER SkipFunctions
    Skip function-level symbol decoding and only export module-level hotspots. The symbol identity
    and unresolved-function checks are skipped too.
.PARAMETER AllowInvalid
    Exit 0 even when the trace is judged Invalid. The verdict is still printed and recorded.
.PARAMETER MaxLostEventsPct
    Lost events above this share of all events make the trace Invalid (default 1). Any lost
    buffer makes it Invalid; fewer lost events make it Degraded.
.PARAMETER MaxUnresolvedPct
    Unresolved functions above this share of the target module's samples make the trace Invalid
    (default 10); any make it Degraded.
.PARAMETER Overhead
    Also report the instrumentation overhead inside the target process (#931): the share of its
    sampled CPU spent in ETW's own logging path (ntoskrnl Etwp*, PerfInfoLogSysCall*,
    KeQueryPerformanceCounter*). Writes <trace>-overhead.json. Needs the kernel's symbols, so it
    downloads only the kernel PDB the trace recorded (one file, matched by signature) from the
    Microsoft symbol server into <trace folder>\KernelSymbols, and reads it from there; the
    server is never put on the symbol path, which would fetch a PDB for every module in the trace.
    If the kernel's functions still do not resolve, the share is reported as unavailable rather
    than as a misleadingly low number. Opt-in because the symbol pass is slower.
.PARAMETER RangeStartUs
    With -RangeEndUs: restrict every report to this interval, in microseconds from the start of
    the trace (xperf -range), e.g. a stall found in WPA. The overhead share is then within it.
.PARAMETER RangeEndUs
    End of the -RangeStartUs interval, in microseconds from the start of the trace.
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl -ProcessId 7412
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\perf-data\etw-app-20260604-104009.etl -SkipFunctions
.EXAMPLE
    pwsh tools/analyze-etw.ps1 -TracePath .\run\trace.etl -SymbolPath .\run\bin -Overhead -RangeStartUs 12000000 -RangeEndUs 13064000
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TracePath,

    [string]$ProcessName = 'TaskSmack.exe',

    [int]$ProcessId,

    [string]$SymbolPath,

    [int]$Top = 20,

    [switch]$SkipFunctions,

    [switch]$AllowInvalid,

    [double]$MaxLostEventsPct = 1.0,

    [double]$MaxUnresolvedPct = 10.0,

    [switch]$Overhead,

    [Nullable[int64]]$RangeStartUs,

    [Nullable[int64]]$RangeEndUs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'analyze-etw-common.ps1')

if ($Overhead -and $SkipFunctions) { throw '-Overhead needs function-level symbols; it cannot be combined with -SkipFunctions.' }
if (($null -eq $RangeStartUs) -ne ($null -eq $RangeEndUs)) { throw 'Pass both -RangeStartUs and -RangeEndUs, or neither.' }
if ($null -ne $RangeStartUs -and $RangeEndUs -le $RangeStartUs) { throw '-RangeEndUs must be after -RangeStartUs.' }

$xperf = Get-Command xperf -ErrorAction SilentlyContinue
if (-not $xperf) {
    throw 'xperf not found on PATH.'
}

$resolvedTrace = Resolve-Path $TracePath
$traceFile = $resolvedTrace.Path
$baseName = [IO.Path]::GetFileNameWithoutExtension($traceFile)
$perfDir = Split-Path -Parent $traceFile
$moduleReport = Join-Path $perfDir "$baseName-profile-modules.txt"
$functionReport = Join-Path $perfDir "$baseName-profile-functions.txt"
$summaryPath = Join-Path $perfDir "$baseName-summary.txt"
$analysisPath = Join-Path $perfDir "$baseName-analysis.json"
$overheadPath = Join-Path $perfDir "$baseName-overhead.json"
$rangeArgs = @(if ($null -ne $RangeStartUs) { '-range'; "$RangeStartUs"; "$RangeEndUs" })

$prevSymCachePath = $env:_NT_SYMCACHE_PATH
$prevSymbolPath   = $env:_NT_SYMBOL_PATH
$exitCode = 0
try {
$env:_NT_SYMCACHE_PATH = Join-Path $perfDir 'SymCache'
$resolvedSymbolDir = $null
if (-not $SkipFunctions) {
    $symbolDir = if ($SymbolPath) { $SymbolPath } else { Join-Path $repoRoot 'build\win-profile\bin' }
    $resolvedSymbolDir = (Resolve-Path $symbolDir -ErrorAction Stop).Path
    $env:_NT_SYMBOL_PATH = $resolvedSymbolDir
}
New-Item -ItemType Directory -Path $env:_NT_SYMCACHE_PATH -Force | Out-Null

# ── Trace validity (#873) ────────────────────────────────────────────────────────────────────
# Two passes: the plain one carries the lost-event and lost-buffer totals, -detail the
# per-provider event counts that are their denominator (it drops the totals).
$statsLines = @(& xperf -i $traceFile -tle -a tracestats 2>&1 | ForEach-Object { "$_" }) +
    @(& xperf -i $traceFile -tle -a tracestats -detail 2>&1 | ForEach-Object { "$_" })
$loss = Get-TraceLossSummary -Lines $statsLines

$identity = $null
if (-not $SkipFunctions) {
    $binaryPath = Join-Path $resolvedSymbolDir $ProcessName
    $binaryInfo = if (Test-Path -LiteralPath $binaryPath) { Get-PeCodeViewInfo -Path $binaryPath } else { $null }
    $expectedPdbLeaf = if ($binaryInfo) { $binaryInfo.PdbLeaf } else { [IO.Path]::ChangeExtension($ProcessName, '.pdb') }
    $traceIds = @(Get-TraceDebugIds -Lines @(& xperf -i $traceFile -tle -a symcache -dbgid 2>$null | ForEach-Object { "$_" }))
    $identity = Test-SymbolIdentity -BinaryInfo $binaryInfo -TraceIds $traceIds -ExpectedPdbLeaf $expectedPdbLeaf
    if (-not (Test-Path -LiteralPath $binaryPath)) {
        $identity = [pscustomobject]@{ Status = 'BinaryNotFound'; Detail = "$ProcessName is not in the symbol directory $resolvedSymbolDir."; TraceCandidates = $identity.TraceCandidates }
    }
}

$kernelSymbols = @()
if ($Overhead) {
    # Only the kernel's PDB, matched to the signature the trace recorded, into a local store that
    # xperf reads with no server behind it (#931). Cached under the trace's folder.
    $kernelStore = Join-Path $perfDir 'KernelSymbols'
    $kernelSymbols = @(Save-KernelSymbols -TraceIds $traceIds -StoreDirectory $kernelStore)
    $env:_NT_SYMBOL_PATH = "$resolvedSymbolDir;srv*$kernelStore"
}

# ── Sampled CPU reports ──────────────────────────────────────────────────────────────────────
& xperf -i $traceFile -quiet -tle -a profile -detail @rangeArgs 2>&1 | Set-Content -Path $moduleReport
if ($LASTEXITCODE -ne 0) {
    throw "xperf module export failed with exit code $LASTEXITCODE"
}

if (-not $SkipFunctions) {
    & xperf -i $traceFile -quiet -tle -symbols -a profile -detail @rangeArgs 2>&1 | Set-Content -Path $functionReport
    if ($LASTEXITCODE -ne 0) {
        throw "xperf function export failed with exit code $LASTEXITCODE"
    }
}

$parseParams = @{ Lines = @(Get-Content $moduleReport); TargetProcessName = $ProcessName }
if ($PSBoundParameters.ContainsKey('ProcessId')) {
    $parseParams.TargetProcessId = $ProcessId
}

$moduleRows = @(Parse-XperfRows @parseParams)
if ($moduleRows.Count -eq 0) {
    throw "No rows found for $ProcessName in $moduleReport"
}

if (-not $PSBoundParameters.ContainsKey('ProcessId')) {
    $selected = $moduleRows | Group-Object ProcessId | ForEach-Object {
        [pscustomobject]@{
            ProcessId = [int]$_.Name
            Weight = ($_.Group | Measure-Object -Property Weight -Sum).Sum
        }
    } | Sort-Object Weight -Descending | Select-Object -First 1
    $ProcessId = $selected.ProcessId
    $moduleRows = @($moduleRows | Where-Object { $_.ProcessId -eq $ProcessId })
}

$functionRows = @()
if ((-not $SkipFunctions) -and (Test-Path $functionReport)) {
    $functionRows = @(Parse-XperfRows -Lines @(Get-Content $functionReport) -TargetProcessName $ProcessName -TargetProcessId $ProcessId)
}

# Weights are sampled CPU in microseconds. ProcessSharePct is a share of this process's sampled
# CPU; AppCodeSharePct is a share of the <ProcessName>!* samples only -- the app's own code,
# excluding the OS, the C++ runtime and drivers -- so the two columns use different denominators.
$moduleTotal = Get-WeightSum $moduleRows
$topModules = $moduleRows | Sort-Object Weight -Descending | Select-Object -First $Top @{n='ProcessSharePct';e={[math]::Round(($_.Weight / $moduleTotal) * 100, 2)}}, @{n='CpuMs';e={[math]::Round($_.Weight / 1000.0, 1)}}, Usage, @{n='Module';e={$_.Symbol}}

$appUnresolved = $null
if ($functionRows.Count -gt 0) {
    $appUnresolved = Get-UnresolvedShare -FunctionRows $functionRows -Module $ProcessName
}
$validity = Get-TraceValidity -Loss $loss -Identity $identity -AppUnresolved $appUnresolved -MaxLostEventsPct $MaxLostEventsPct -MaxUnresolvedPct $MaxUnresolvedPct

$summaryLines = @()
$summaryLines += "Trace: $traceFile"
$summaryLines += "Process: $ProcessName ($ProcessId), sampled CPU $([math]::Round($moduleTotal / 1000.0, 1)) ms"
if ($rangeArgs.Count -gt 0) { $summaryLines += "Range: $RangeStartUs-$RangeEndUs us from trace start" }
$summaryLines += "Module report: $moduleReport"
if (-not $SkipFunctions) {
    $summaryLines += "Function report: $functionReport"
}
$summaryLines += ''
$summaryLines += "Trace validity: $($validity.Status.ToUpperInvariant())"
$buffersText = if ($null -ne $loss.LostBuffers) { "$($loss.LostBuffers) buffers" } else { 'buffers unknown (lost-buffer count not readable)' }
$lossText = if ($loss.Parsed) { "$($loss.LostEvents) events, $buffersText" + $(if ($null -ne $loss.LostEventsPct) { " ($($loss.LostEventsPct)% of $($loss.RecordedEvents + $loss.LostEvents) events)" } else { '' }) } else { 'unknown (trace statistics not readable)' }
$summaryLines += "  Lost: $lossText"
if ($identity) { $summaryLines += "  Symbol identity: $($identity.Status) - $($identity.Detail)" } else { $summaryLines += '  Symbol identity: not checked (-SkipFunctions)' }
if ($appUnresolved -and $null -ne $appUnresolved.UnresolvedPct) { $summaryLines += "  Unresolved $ProcessName functions: $($appUnresolved.UnresolvedPct)% of its samples" }
foreach ($reason in $validity.Invalid) { $summaryLines += "  INVALID: $reason" }
foreach ($reason in $validity.Degraded) { $summaryLines += "  degraded: $reason" }
$summaryLines += ''
$summaryLines += 'Top Modules (ProcessSharePct = share of this process''s sampled CPU)'
$summaryLines += ($topModules | Format-Table -AutoSize | Out-String -Width 240).TrimEnd()

$topFunctions = @()
if ($functionRows.Count -gt 0) {
    $appFunctions = @($functionRows | Where-Object { $_.Symbol -like "$ProcessName!*" })
    $functionTotal = Get-WeightSum $appFunctions
    if ($appFunctions.Count -gt 0 -and $functionTotal -gt 0) {
        $topFunctions = @($appFunctions | Sort-Object Weight -Descending | Select-Object -First $Top @{n='AppCodeSharePct';e={[math]::Round(($_.Weight / $functionTotal) * 100, 2)}}, @{n='ProcessSharePct';e={[math]::Round(($_.Weight / $moduleTotal) * 100, 2)}}, @{n='CpuMs';e={[math]::Round($_.Weight / 1000.0, 1)}}, @{n='Function';e={$_.Symbol}})
        $summaryLines += ''
        $summaryLines += "Top Functions (AppCodeSharePct = share of $ProcessName!* samples only; ProcessSharePct = share of all of this process's sampled CPU)"
        $summaryLines += ($topFunctions | Format-Table -AutoSize | Out-String -Width 240).TrimEnd()
    }
    else {
        $summaryLines += ''
        $summaryLines += "Top Functions: none found for $ProcessName (symbols may not have resolved; try -SymbolPath or use win-profile build)"
    }
}

$overheadResult = $null
if ($Overhead) {
    $overheadResult = Get-EtwOverhead -FunctionRows $functionRows
    $overheadReport = [ordered]@{
        Trace         = $traceFile
        Process       = "$ProcessName ($ProcessId)"
        RangeStartUs  = $RangeStartUs
        RangeEndUs    = $RangeEndUs
    }
    foreach ($property in $overheadResult.PSObject.Properties) { $overheadReport[$property.Name] = $property.Value }
    $overheadReport['KernelSymbols'] = $kernelSymbols
    $overheadReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $overheadPath -Encoding utf8
    $summaryLines += ''
    if ($overheadResult.Status -eq 'Measured') {
        $summaryLines += "Instrumentation overhead: $($overheadResult.EtwPathSharePct)% of $ProcessName's sampled CPU is ETW's logging path ($([math]::Round($overheadResult.EtwPathWeightUs / 1000.0, 1)) ms of $([math]::Round($overheadResult.ProcessWeightUs / 1000.0, 1)) ms; kernel symbols $($overheadResult.KernelResolvedPct)% resolved)"
    }
    elseif ($overheadResult.Status -eq 'Bounded') {
        $summaryLines += "Instrumentation overhead: between $($overheadResult.EtwPathShareMinPct)% and $($overheadResult.EtwPathShareMaxPct)% of $ProcessName's sampled CPU is ETW's logging path - $($overheadResult.Note)"
    }
    else {
        $summaryLines += "Instrumentation overhead: $($overheadResult.Status) - $($overheadResult.Note)"
    }
}

$analysis = [ordered]@{
    Trace             = $traceFile
    Process           = $ProcessName
    ProcessId         = $ProcessId
    RangeStartUs      = $RangeStartUs
    RangeEndUs        = $RangeEndUs
    SymbolDirectory   = $resolvedSymbolDir
    Validity          = $validity
    Loss              = $loss
    SymbolIdentity    = $identity
    AppUnresolved     = $appUnresolved
    ProcessCpuUs      = $moduleTotal
    TopModules        = @($topModules)
    TopFunctions      = $topFunctions
    Overhead          = $overheadResult
    ShareDefinitions  = [ordered]@{
        ProcessSharePct = 'share of this process''s sampled CPU'
        AppCodeSharePct = "share of $ProcessName!* samples only (the app's own code)"
        CpuMs           = 'sampled CPU in milliseconds'
    }
}
$analysis | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $analysisPath -Encoding utf8

$summaryText = $summaryLines -join [Environment]::NewLine
$summaryText | Set-Content -Path $summaryPath
Write-Host $summaryText
Write-Host ''
Write-Host "SUMMARY=$summaryPath"
Write-Host "ANALYSIS=$analysisPath"
if ($Overhead) { Write-Host "OVERHEAD=$overheadPath" }
Write-Host "VALIDITY=$($validity.Status)"

if ($validity.Status -eq 'Invalid') {
    if ($AllowInvalid) {
        Write-Warning 'Trace is INVALID (see above); exiting 0 because -AllowInvalid was passed.'
    }
    else {
        Write-Warning 'Trace is INVALID (see above). The reports are written for inspection, but do not draw conclusions from them; pass -AllowInvalid to exit 0 anyway.'
        $exitCode = 3
    }
}
}
finally {
    if ($null -eq $prevSymCachePath) { Remove-Item Env:_NT_SYMCACHE_PATH -ErrorAction SilentlyContinue }
    else { $env:_NT_SYMCACHE_PATH = $prevSymCachePath }
    if ($null -eq $prevSymbolPath) { Remove-Item Env:_NT_SYMBOL_PATH -ErrorAction SilentlyContinue }
    else { $env:_NT_SYMBOL_PATH = $prevSymbolPath }
}
exit $exitCode
