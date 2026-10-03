# Tests for analyze-etw-common.ps1 (analyze-etw.ps1's validity and overhead checks). Uses fixed
# xperf text and real PE files; no trace or xperf needed. Registered in CTest on Windows with
# PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'analyze-etw-common.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# ── Lost events (#873) ───────────────────────────────────────────────────────────────────────
$stats = @(
    'Number of Processors : 16'
    'Total # Lost Buffers : 0'
    'Total # Lost Events  : 500'
    ''
    'Classic EventGuid                        TotalCount       TotalSize  Name'
    '{2cb15d1d-5fc1-11d2-abe1-00a0c911f518}        42756         8049624  Image'
    '    0x02  0x00 0x0003                           199           34144  Image: Unload'
    '{3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c}        57244          361648  Process'
)
$loss = Get-TraceLossSummary -Lines $stats
Assert-True ($loss.Parsed -and $loss.LostEvents -eq 500 -and $loss.LostBuffers -eq 0) 'Lost counts not parsed'
Assert-True ($loss.RecordedEvents -eq 100000) "Provider totals summed to $($loss.RecordedEvents), expected 100000 (per-event-type rows must not be counted)"
Assert-True ($loss.LostEventsPct -eq 0.498) "Lost share $($loss.LostEventsPct), expected 500/100500"
# xperf's -detail output has no totals, only the lost-events warning.
$detailOnly = Get-TraceLossSummary -Lines @("`t`t336273 Events were lost in this trace.  Data may be unreliable.", '{2cb15d1d-5fc1-11d2-abe1-00a0c911f518}        42756         8049624  Image')
Assert-True ($detailOnly.Parsed -and $detailOnly.LostEvents -eq 336273) 'The lost-events warning line must be read'
Assert-True ($null -eq $detailOnly.LostBuffers) "Without the plain pass the buffer count is unknown, not $($detailOnly.LostBuffers)"
# Detail-only, with a small loss: 5 of 100,000 events is under the limit, but the verdict must say the
# lost-buffer check could not run rather than treat the buffers as zero.
$detailSmallLoss = Get-TraceLossSummary -Lines @("`t`t5 Events were lost in this trace.  Data may be unreliable.", '{2cb15d1d-5fc1-11d2-abe1-00a0c911f518}        99995         1  Image')
Assert-True ($null -eq $detailSmallLoss.LostBuffers -and $detailSmallLoss.LostEventsPct -eq 0.005) "Detail-only small loss: $($detailSmallLoss | Out-String)"
$detailVerdict = Get-TraceValidity -Loss $detailSmallLoss -Identity $null -AppUnresolved $null
$detailText = $detailVerdict.Degraded -join ' '
Assert-True ($detailVerdict.Status -eq 'Degraded' -and $detailText -like '*unknown number of buffers*' -and $detailText -like '*lost-buffer count could not be read*') "Unknown buffer count must be flagged: $detailText"
Assert-True ($detailText -notlike '*0 buffers*') 'An unknown buffer count must not be reported as 0'
# No loss in the -detail pass and no plain pass: still flagged, never silently Valid.
$detailNoLoss = [pscustomobject]@{ Parsed = $true; LostEvents = 0; LostBuffers = $null; RecordedEvents = 1000; LostEventsRatio = 0.0; LostEventsPct = 0 }
Assert-True ((Get-TraceValidity -Loss $detailNoLoss -Identity $null -AppUnresolved $null).Status -eq 'Degraded') 'An unchecked buffer count is Degraded'
# Lost events but no provider totals (the -detail pass failed): the share is unknown, not 100 %.
$noTotals = Get-TraceLossSummary -Lines @('Total # Lost Buffers : 0', 'Total # Lost Events  : 500')
Assert-True ($noTotals.Parsed -and $noTotals.LostEvents -eq 500 -and $null -eq $noTotals.RecordedEvents) 'Lost count without totals'
Assert-True ($null -eq $noTotals.LostEventsRatio -and $null -eq $noTotals.LostEventsPct) "Share must be unknown without totals, got $($noTotals.LostEventsPct)"
$unparsed = Get-TraceLossSummary -Lines @('nothing useful')
Assert-True (-not $unparsed.Parsed) 'Missing statistics must be reported as unparsed'

# ── Debug IDs and symbol identity (#873) ────────────────────────────────────────────────────
$dbgid = @(
    'CodeView Record'
    '"[RSDS] PdbSig: {525d3b8b-c741-5cce-40c3-f81e3ccaf261}; Age: 1; Pdb: IppCommon.pdb"'
    '"[RSDS] PdbSig: {917B0F96-811D-35C3-4C4C-44205044422E}; Age: 1; Pdb: C:\src\tasksmack\build\win-debug\bin\TaskSmack.pdb"'
)
$ids = @(Get-TraceDebugIds -Lines $dbgid)
Assert-True ($ids.Count -eq 2) 'Debug IDs not parsed'
Assert-True ($ids[1].Guid -eq '917b0f96-811d-35c3-4c4c-44205044422e' -and $ids[1].PdbLeaf -eq 'TaskSmack.pdb') 'Debug ID fields wrong (GUIDs compare lower-case)'

$sameBuild = [pscustomobject]@{ Guid = '917b0f96-811d-35c3-4c4c-44205044422e'; Age = 1; PdbLeaf = 'TaskSmack.pdb' }
Assert-True ((Test-SymbolIdentity -BinaryInfo $sameBuild -TraceIds $ids -ExpectedPdbLeaf 'TaskSmack.pdb').Status -eq 'Match') 'Same build must match'
$otherBuild = [pscustomobject]@{ Guid = '00000000-1111-2222-3333-444444444444'; Age = 1; PdbLeaf = 'TaskSmack.pdb' }
$mismatch = Test-SymbolIdentity -BinaryInfo $otherBuild -TraceIds $ids -ExpectedPdbLeaf 'TaskSmack.pdb'
Assert-True ($mismatch.Status -eq 'Mismatch' -and $mismatch.Detail -like '*different build*') 'A different build must be a mismatch'
$newerAge = [pscustomobject]@{ Guid = '917b0f96-811d-35c3-4c4c-44205044422e'; Age = 2; PdbLeaf = 'TaskSmack.pdb' }
Assert-True ((Test-SymbolIdentity -BinaryInfo $newerAge -TraceIds $ids -ExpectedPdbLeaf 'TaskSmack.pdb').Status -eq 'Mismatch') 'The age is part of the identity'
Assert-True ((Test-SymbolIdentity -BinaryInfo $sameBuild -TraceIds $ids -ExpectedPdbLeaf 'Other.pdb').Status -eq 'NotInTrace') 'An image the trace never loaded'
# Two builds of TaskSmack ran during the system-wide trace: matching one of them cannot say it is
# the analyzed process's.
$twoBuilds = $ids + @([pscustomobject]@{ Guid = '00000000-1111-2222-3333-444444444444'; Age = 1; Pdb = 'D:\other\TaskSmack.pdb'; PdbLeaf = 'TaskSmack.pdb' })
$ambiguous = Test-SymbolIdentity -BinaryInfo $sameBuild -TraceIds $twoBuilds -ExpectedPdbLeaf 'TaskSmack.pdb'
Assert-True ($ambiguous.Status -eq 'Ambiguous' -and $ambiguous.Detail -like '*2 different builds*') "Two builds must be Ambiguous, got $($ambiguous.Status)"
$sameTwice = $ids + @($ids[1])
Assert-True ((Test-SymbolIdentity -BinaryInfo $sameBuild -TraceIds $sameTwice -ExpectedPdbLeaf 'TaskSmack.pdb').Status -eq 'Match') 'The same build loaded twice is still a Match'
Assert-True ((Test-SymbolIdentity -BinaryInfo $null -TraceIds $ids -ExpectedPdbLeaf 'TaskSmack.pdb').Status -eq 'BinaryUnreadable') 'No CodeView record'

# A real PE file: this PowerShell host carries a CodeView record; a text file does not.
$hostExe = (Get-Process -Id $PID).Path
$pe = Get-PeCodeViewInfo -Path $hostExe
Assert-True ($null -ne $pe -and $pe.Guid -match '^[0-9a-f]{8}-' -and $pe.Age -ge 1 -and $pe.PdbLeaf -like '*.pdb') "No CodeView record read from $hostExe"
$notPe = [IO.Path]::GetTempFileName()
try {
    Set-Content -LiteralPath $notPe -Value 'not a PE file'
    Assert-True ($null -eq (Get-PeCodeViewInfo -Path $notPe)) 'A non-PE file must give no record'
}
finally { Remove-Item -LiteralPath $notPe -Force -ErrorAction SilentlyContinue }

# Truncated copies of the host binary give no record (BinaryUnreadable) instead of throwing: cut
# right after the COFF header, inside the optional header, inside the section table, and just
# short of the debug directory's entries.
$hostBytes = [IO.File]::ReadAllBytes($hostExe)
$hostPe = [BitConverter]::ToInt32($hostBytes, 0x3C)
$hostOptional = $hostPe + 24
$hostSections = $hostOptional + [BitConverter]::ToUInt16($hostBytes, $hostPe + 20)
$cuts = [ordered]@{
    'after the COFF header'    = $hostOptional
    'inside the optional header' = $hostOptional + 40
    'inside the section table' = $hostSections + 20
    'before the debug data'    = $hostSections + 40 * [BitConverter]::ToUInt16($hostBytes, $hostPe + 6)
}
foreach ($cut in $cuts.GetEnumerator()) {
    $truncated = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllBytes($truncated, $hostBytes[0..($cut.Value - 1)])
        $result = $null
        try { $result = Get-PeCodeViewInfo -Path $truncated }
        catch { throw "A PE truncated $($cut.Key) must not throw: $_" }
        Assert-True ($null -eq $result) "A PE truncated $($cut.Key) must give no record"
        Assert-True ((Test-SymbolIdentity -BinaryInfo $result -TraceIds $ids -ExpectedPdbLeaf 'TaskSmack.pdb').Status -eq 'BinaryUnreadable') "Truncated $($cut.Key) is BinaryUnreadable"
    }
    finally { Remove-Item -LiteralPath $truncated -Force -ErrorAction SilentlyContinue }
}

# ── Rows, unresolved share and the verdict (#873) ───────────────────────────────────────────
$functionLines = @(
    'TaskSmack.exe (4000),     80000,       0.03,         TaskSmack.exe!Domain::SystemModel::refresh'
    'TaskSmack.exe (4000),     20000,       0.01,         TaskSmack.exe!"Unknown"'
    'TaskSmack.exe (4000),      5000,       0.00,         ntoskrnl.exe!EtwpLogKernelEvent'
    'TaskSmack.exe (4000),      2000,       0.00,         ntoskrnl.exe!KeQueryPerformanceCounter'
    'TaskSmack.exe (4000),     13000,       0.00,         ntoskrnl.exe!KiPageFault'
    'Other.exe (77),          99999,       0.10,         Other.exe!main'
)
$rows = @(Parse-XperfRows -Lines $functionLines -TargetProcessName 'TaskSmack.exe')
Assert-True ($rows.Count -eq 5) "Expected the 5 TaskSmack rows, got $($rows.Count)"
$byPid = @(Parse-XperfRows -Lines $functionLines -TargetProcessName 'TaskSmack.exe' -TargetProcessId 77)
Assert-True ($byPid.Count -eq 1 -and $byPid[0].Weight -eq 99999) 'Filtering by PID'

$unresolved = Get-UnresolvedShare -FunctionRows $rows -Module 'TaskSmack.exe'
Assert-True ($unresolved.WeightUs -eq 100000 -and $unresolved.UnresolvedPct -eq 20) "Unresolved share $($unresolved.UnresolvedPct), expected 20"

$clean = [pscustomobject]@{ Parsed = $true; LostEvents = 0; LostBuffers = 0; RecordedEvents = 1000; LostEventsRatio = 0.0; LostEventsPct = 0 }
$match = [pscustomobject]@{ Status = 'Match'; Detail = 'ok' }
$allResolved = [pscustomobject]@{ Module = 'TaskSmack.exe'; UnresolvedUs = 0; UnresolvedRatio = 0.0; UnresolvedPct = 0 }
Assert-True ((Get-TraceValidity -Loss $clean -Identity $match -AppUnresolved $allResolved).Status -eq 'Valid') 'A clean trace is Valid'

$fewLost = [pscustomobject]@{ Parsed = $true; LostEvents = 5; LostBuffers = 0; RecordedEvents = 1000; LostEventsRatio = 0.005; LostEventsPct = 0.5 }
# Just over the limit: 100,004 of 10,000,000 is 1.00004%, which rounds to 1.000 for display but must
# still be Invalid -- limits compare the exact ratio.
$justOver = Get-TraceLossSummary -Lines @('Total # Lost Buffers : 0', 'Total # Lost Events  : 100004', '{2cb15d1d-5fc1-11d2-abe1-00a0c911f518}        9899996         1  Image')
Assert-True ($justOver.LostEventsPct -eq 1.0) "Display rounding: $($justOver.LostEventsPct)"
Assert-True ((Get-TraceValidity -Loss $justOver -Identity $match -AppUnresolved $allResolved).Status -eq 'Invalid') 'Loss just over the limit must be Invalid despite rounding'
Assert-True ((Get-TraceValidity -Loss $fewLost -Identity $match -AppUnresolved $allResolved).Status -eq 'Degraded') 'A little loss is Degraded'
$manyLost = [pscustomobject]@{ Parsed = $true; LostEvents = 336273; LostBuffers = 0; RecordedEvents = 3000000; LostEventsRatio = 0.1008; LostEventsPct = 10.08 }
$verdict = Get-TraceValidity -Loss $manyLost -Identity $match -AppUnresolved $allResolved
Assert-True ($verdict.Status -eq 'Invalid' -and ($verdict.Invalid -join ' ') -like '*336273 events*') 'Heavy loss is Invalid, with the count'
$lostBuffer = [pscustomobject]@{ Parsed = $true; LostEvents = 0; LostBuffers = 1; RecordedEvents = 1000; LostEventsRatio = 0.0; LostEventsPct = 0 }
Assert-True ((Get-TraceValidity -Loss $lostBuffer -Identity $match -AppUnresolved $allResolved).Status -eq 'Invalid') 'Any lost buffer is Invalid'
Assert-True ((Get-TraceValidity -Loss $clean -Identity $mismatch -AppUnresolved $allResolved).Status -eq 'Degraded') 'A mismatch whose functions resolved anyway (via the image''s PDB path) is flagged, not Invalid'
Assert-True ((Get-TraceValidity -Loss $clean -Identity $mismatch -AppUnresolved $unresolved).Status -eq 'Invalid') 'A mismatch that left functions unresolved is Invalid through the unresolved share'
Assert-True ((Get-TraceValidity -Loss $clean -Identity ([pscustomobject]@{ Status = 'NotInTrace'; Detail = 'x' }) -AppUnresolved $allResolved).Status -eq 'Degraded') 'Unverified identity is Degraded'
Assert-True ((Get-TraceValidity -Loss $clean -Identity $match -AppUnresolved $unresolved).Status -eq 'Invalid') '20% unresolved is above the 10% limit'
Assert-True ((Get-TraceValidity -Loss $clean -Identity $match -AppUnresolved $unresolved -MaxUnresolvedPct 25).Status -eq 'Degraded') 'The limit is configurable'
# 1,000,040 of 10,000,000 us unresolved is 10.0004%: rounds to 10 but is over the 10% limit.
$justOverUnresolved = Get-UnresolvedShare -Module 'TaskSmack.exe' -FunctionRows @(
    [pscustomobject]@{ Weight = 8999960; Symbol = 'TaskSmack.exe!main' }
    [pscustomobject]@{ Weight = 1000040; Symbol = 'TaskSmack.exe!"Unknown"' })
Assert-True ($justOverUnresolved.UnresolvedPct -eq 10) "Display rounding: $($justOverUnresolved.UnresolvedPct)"
Assert-True ((Get-TraceValidity -Loss $clean -Identity $match -AppUnresolved $justOverUnresolved).Status -eq 'Invalid') 'Unresolved just over the limit must be Invalid despite rounding'
# A sliver of unresolved weight that rounds to 0.00% is still flagged.
$sliver = Get-UnresolvedShare -Module 'TaskSmack.exe' -FunctionRows @(
    [pscustomobject]@{ Weight = 100000000; Symbol = 'TaskSmack.exe!main' }
    [pscustomobject]@{ Weight = 1; Symbol = 'TaskSmack.exe!"Unknown"' })
Assert-True ($sliver.UnresolvedPct -eq 0 -and (Get-TraceValidity -Loss $clean -Identity $match -AppUnresolved $sliver).Status -eq 'Degraded') 'A rounded-to-zero unresolved share is still Degraded'
Assert-True ((Get-TraceValidity -Loss $clean -Identity $ambiguous -AppUnresolved $allResolved).Status -eq 'Degraded') 'Ambiguous identity is Degraded'
Assert-True ((Get-TraceValidity -Loss $unparsed -Identity $null -AppUnresolved $null).Status -eq 'Degraded') 'Unknown loss is not silently Valid'

# ── Kernel symbols only (#931) ──────────────────────────────────────────────────────────────
$kernelId = [pscustomobject]@{ Guid = '0b343933-8f40-ddcd-568a-30e465a9576b'; Age = 1; Pdb = 'ntkrnlmp.pdb'; PdbLeaf = 'ntkrnlmp.pdb' }
Assert-True ((Get-SymbolStoreKey $kernelId) -ceq '0B3439338F40DDCD568A30E465A9576B1') 'Symbol store key format'
Assert-True ((Get-SymbolStoreKey ([pscustomobject]@{ Guid = $kernelId.Guid; Age = 26 })) -ceq '0B3439338F40DDCD568A30E465A9576B1A') 'The age is hex'
$store = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-symstore-$([guid]::NewGuid().ToString('N'))"
try {
    $cached = Join-Path $store 'ntkrnlmp.pdb\0B3439338F40DDCD568A30E465A9576B1\ntkrnlmp.pdb'
    New-Item -ItemType Directory -Path (Split-Path -Parent $cached) -Force | Out-Null
    Set-Content -LiteralPath $cached -Value 'pdb'
    # Only the kernel's PDB is considered; an already-cached one is not fetched again.
    $saved = @(Save-KernelSymbols -TraceIds @($ids[0], $kernelId) -StoreDirectory $store -ServerUrl 'https://invalid.example')
    Assert-True ($saved.Count -eq 1 -and $saved[0].Pdb -eq 'ntkrnlmp.pdb' -and $saved[0].Status -eq 'cached') "Kernel-only fetch: $($saved | Out-String)"
}
finally { Remove-Item -LiteralPath $store -Recurse -Force -ErrorAction SilentlyContinue }

# ── Instrumentation overhead (#931) ─────────────────────────────────────────────────────────
$overhead = Get-EtwOverhead -FunctionRows $rows
Assert-True ($overhead.Status -eq 'Measured') "Overhead status $($overhead.Status)"
Assert-True ($overhead.ProcessWeightUs -eq 120000 -and $overhead.EtwPathWeightUs -eq 7000) 'ETW-path weight'
Assert-True ($overhead.EtwPathSharePct -eq 5.83) "ETW-path share $($overhead.EtwPathSharePct), expected 7000/120000"
Assert-True (@($overhead.TopEtwPathFunctions).Count -eq 2 -and $overhead.TopEtwPathFunctions[0].Function -eq 'ntoskrnl.exe!EtwpLogKernelEvent') 'Top contributors'

# Unresolved kernel frames cannot match Etwp*, so the share must be reported as unavailable.
$noKernelSymbols = @(Parse-XperfRows -Lines @(
    'TaskSmack.exe (4000),     80000,       0.03,         TaskSmack.exe!main'
    'TaskSmack.exe (4000),     20000,       0.01,         ntoskrnl.exe!"Unknown"'
) -TargetProcessName 'TaskSmack.exe')
$unavailable = Get-EtwOverhead -FunctionRows $noKernelSymbols
Assert-True ($unavailable.Status -eq 'SymbolsUnavailable' -and $null -eq $unavailable.EtwPathSharePct) 'Missing kernel symbols must not report a share'
Assert-True ((Get-EtwOverhead -FunctionRows @()).Status -eq 'NoSamples') 'No samples'
Assert-True ((Get-TraceValidity -Loss $noTotals -Identity $match -AppUnresolved $allResolved).Invalid -join ' ' -like '*could not be computed*') 'Unknown share is reported as uncomputable'

# TopEtwPathFunctions is always a JSON list: zero, one, several and unavailable.
$oneEtw = @(Parse-XperfRows -Lines @(
    'TaskSmack.exe (4000),     80000,       0.03,         TaskSmack.exe!main'
    'TaskSmack.exe (4000),      2000,       0.00,         ntoskrnl.exe!EtwpLogKernelEvent'
) -TargetProcessName 'TaskSmack.exe')
$noEtw = @(Parse-XperfRows -Lines @('TaskSmack.exe (4000),     80000,       0.03,         ntoskrnl.exe!KiPageFault') -TargetProcessName 'TaskSmack.exe')
foreach ($case in @(@{ Name = 'one'; Rows = $oneEtw; Count = 1 }, @{ Name = 'zero'; Rows = $noEtw; Count = 0 }, @{ Name = 'several'; Rows = $rows; Count = 2 }, @{ Name = 'unavailable'; Rows = $noKernelSymbols; Count = 0 })) {
    $json = Get-EtwOverhead -FunctionRows $case.Rows | ConvertTo-Json -Depth 5
    Assert-True ($json -match '"TopEtwPathFunctions":\s*\[') "TopEtwPathFunctions must serialize as a list ($($case.Name))"
    Assert-True (@((Get-EtwOverhead -FunctionRows $case.Rows).TopEtwPathFunctions).Count -eq $case.Count) "TopEtwPathFunctions count ($($case.Name))"
}

# Partly resolved kernel: 18,000 us of named non-ETW kernel samples, 2,000 us unknown. Any of the
# unknown could be ETW logging, so no single share -- only bounds (0% to 2,000/100,000).
$partial = @(Parse-XperfRows -Lines @(
    'TaskSmack.exe (4000),     80000,       0.03,         TaskSmack.exe!main'
    'TaskSmack.exe (4000),     18000,       0.01,         ntoskrnl.exe!KiPageFault'
    'TaskSmack.exe (4000),      2000,       0.00,         ntoskrnl.exe!"Unknown"'
) -TargetProcessName 'TaskSmack.exe')
$bounded = Get-EtwOverhead -FunctionRows $partial
Assert-True ($bounded.Status -eq 'Bounded' -and $null -eq $bounded.EtwPathSharePct) "Partial kernel resolution must not report a single share: $($bounded.Status) $($bounded.EtwPathSharePct)"
Assert-True ($bounded.EtwPathShareMinPct -eq 0 -and $bounded.EtwPathShareMaxPct -eq 2) "Bounds $($bounded.EtwPathShareMinPct)-$($bounded.EtwPathShareMaxPct)"
# Even a sliver of unresolved kernel weight that rounds the resolved share to 100% is Bounded.
$tiny = @(Parse-XperfRows -Lines @(
    'TaskSmack.exe (4000),  10000000,       0.03,         ntoskrnl.exe!KiPageFault'
    'TaskSmack.exe (4000),         1,       0.00,         ntoskrnl.exe!"Unknown"'
) -TargetProcessName 'TaskSmack.exe')
Assert-True ((Get-EtwOverhead -FunctionRows $tiny).Status -eq 'Bounded') 'Any unresolved kernel weight is Bounded'

Write-Host 'analyze-etw tests passed'
