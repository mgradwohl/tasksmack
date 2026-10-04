# Parsing and validity helpers for analyze-etw.ps1, dot-sourced by it and by test-analyze-etw.ps1.
# Pure functions over xperf's text output and PE files, so they can be tested without a trace.

function Get-WeightSum {
    # Sum of the rows' Weight; 0 for no rows. (Measure-Object has no .Sum for an empty input under
    # Set-StrictMode, which an empty range or an unresolved module produces.)
    param([AllowEmptyCollection()][object[]]$Rows)
    [int64]$sum = 0
    foreach ($row in @($Rows)) { if ($null -ne $row) { $sum += [int64]$row.Weight } }
    return $sum
}

function Parse-XperfRows {
    # Rows of `xperf -a profile -detail`: "<process> (<pid>), <weight>, <usage>, <module[!function]>".
    # Weight is sampled CPU in microseconds; Usage is a percentage of all CPUs over the trace.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][string]$TargetProcessName,
        [int]$TargetProcessId
    )

    # The name is always required, the PID too when one is given: the symbol-identity and
    # unresolved-share checks are keyed to TargetProcessName, so a PID that belongs to another
    # executable must select nothing rather than another process's samples.
    $pidPattern = if ($TargetProcessId -ne 0) { [regex]::Escape([string]$TargetProcessId) } else { '[0-9]+' }
    $pattern = '^\s*' + [regex]::Escape($TargetProcessName) + ' \((' + $pidPattern + ')\),\s*([0-9]+),\s*([0-9.]+),\s*(.+)$'

    foreach ($line in $Lines) {
        if ($line -match $pattern) {
            [pscustomobject]@{
                ProcessName = $TargetProcessName
                ProcessId   = [int]$Matches[1]
                Weight      = [int64]$Matches[2]
                Usage       = [double]$Matches[3]
                Symbol      = $Matches[4].Trim()
            }
        }
    }
}

function Get-TraceLossSummary {
    # From `xperf -i <etl> -tle -a tracestats -detail`: lost events and buffers, and the number
    # of events recorded (the sum of every provider's TotalCount row) as their denominator (#873).
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines)

    $lostEvents = $null
    $lostBuffers = $null
    [int64]$recorded = 0
    $providerRows = 0
    foreach ($line in $Lines) {
        if ($null -eq $lostEvents -and $line -match '^\s*Total # Lost Events\s*:\s*([0-9]+)') { $lostEvents = [int64]$Matches[1] }
        elseif ($null -eq $lostBuffers -and $line -match '^\s*Total # Lost Buffers\s*:\s*([0-9]+)') { $lostBuffers = [int64]$Matches[1] }
        # The -detail output has no totals, only this warning.
        elseif ($null -eq $lostEvents -and $line -match '^\s*([0-9]+) Events were lost in this trace') { $lostEvents = [int64]$Matches[1] }
        elseif ($line -match '^\{[0-9a-fA-F-]+\}\s+([0-9]+)\s+[0-9]+\s+\S') {
            $recorded += [int64]$Matches[1]
            $providerRows++
        }
    }
    # The exact ratio is what limits are compared against; LostEventsPct is rounded for display,
    # and rounding must not carry a value just over a limit back under it.
    # Only with the per-provider totals as the denominator: without them (e.g. the -detail pass
    # failed) the share is unknown, not 100 %, and the verdict treats it as uncomputable.
    $lostRatio = $null
    if ($null -ne $lostEvents -and $providerRows -gt 0 -and ($recorded + $lostEvents) -gt 0) {
        $lostRatio = [double]$lostEvents / ($recorded + $lostEvents)
    }
    # A trace with no loss prints no warning, so a parsed header with no lost-event line means 0.
    if ($null -eq $lostEvents -and $null -ne $lostBuffers) { $lostEvents = 0 }
    return [pscustomobject]@{
        Parsed         = ($null -ne $lostEvents)
        LostEvents     = $lostEvents
        # $null when only the -detail pass ran: it has no buffer count, and an unknown count must
        # not read as zero buffers lost.
        LostBuffers    = $lostBuffers
        RecordedEvents = if ($providerRows -gt 0) { $recorded } else { $null }
        LostEventsRatio = $lostRatio
        LostEventsPct  = if ($null -ne $lostRatio) { [math]::Round(100.0 * $lostRatio, 3) } else { $null }
    }
}

function Get-TraceDebugIds {
    # CodeView (RSDS) records from `xperf -i <etl> -tle -a symcache -dbgid`: the PDB signature
    # and age of every image loaded during the trace.
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines)
    foreach ($line in $Lines) {
        if ($line -match '\[RSDS\] PdbSig: \{([0-9a-fA-F-]+)\}; Age: ([0-9]+); Pdb: ([^"]+)') {
            [pscustomobject]@{
                Guid    = $Matches[1].ToLowerInvariant()
                Age     = [int]$Matches[2]
                Pdb     = $Matches[3].Trim()
                PdbLeaf = (Split-Path -Leaf $Matches[3].Trim())
            }
        }
    }
}

function Get-PeCodeViewInfo {
    # The CodeView (RSDS) debug record of a PE file: the PDB signature, age and path the linker
    # wrote, which is what the trace records for the image it loaded. $null if there is none.
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    # Every structure is bounds-checked before it is read: a truncated or damaged binary gives
    # $null (BinaryUnreadable), not an exception that aborts the analysis.
    $fits = { param([int64]$Offset, [int64]$Length) $Offset -ge 0 -and $Offset + $Length -le $bytes.Length }
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null } # 'MZ'
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) { return $null } # 'PE\0\0'
    $fileHeader = $peOffset + 4
    $sectionCount = [BitConverter]::ToUInt16($bytes, $fileHeader + 2)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $fileHeader + 16)
    $optional = $fileHeader + 20
    if (-not (& $fits $optional 2)) { return $null }
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $dataDirectories = switch ($magic) { 0x10B { $optional + 96 } 0x20B { $optional + 112 } default { return $null } }
    # The debug directory (index 6, 8 bytes) must lie inside both the optional header and the file.
    if ($dataDirectories + 7 * 8 -gt $optional + $optionalSize -or -not (& $fits $dataDirectories (7 * 8))) { return $null }
    $debugRva = [BitConverter]::ToUInt32($bytes, $dataDirectories + 6 * 8) # IMAGE_DIRECTORY_ENTRY_DEBUG
    $debugSize = [BitConverter]::ToUInt32($bytes, $dataDirectories + 6 * 8 + 4)
    if ($debugRva -eq 0 -or $debugSize -eq 0) { return $null }

    # RVA -> file offset through the section table.
    $sections = $optional + $optionalSize
    $debugOffset = $null
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $s = $sections + 40 * $i
        if (-not (& $fits $s 40)) { return $null }
        $virtualSize = [BitConverter]::ToUInt32($bytes, $s + 8)
        $virtualAddress = [BitConverter]::ToUInt32($bytes, $s + 12)
        $rawSize = [BitConverter]::ToUInt32($bytes, $s + 16)
        $rawPointer = [BitConverter]::ToUInt32($bytes, $s + 20)
        $span = [Math]::Max($virtualSize, $rawSize)
        if ($debugRva -ge $virtualAddress -and $debugRva -lt $virtualAddress + $span) {
            $debugOffset = [int64]$rawPointer + ($debugRva - $virtualAddress)
            break
        }
    }
    if ($null -eq $debugOffset) { return $null }

    for ($entry = $debugOffset; $entry + 28 -le $debugOffset + $debugSize; $entry += 28) {
        if (-not (& $fits $entry 28)) { return $null }
        $type = [BitConverter]::ToUInt32($bytes, $entry + 12)
        $dataSize = [BitConverter]::ToUInt32($bytes, $entry + 16)
        $dataPointer = [BitConverter]::ToUInt32($bytes, $entry + 24)
        # IMAGE_DEBUG_TYPE_CODEVIEW. The whole declared payload must be in the file: RSDS, GUID, age
        # and a NUL-terminated, non-empty PDB path. A record cut short anywhere is unreadable.
        if ($type -eq 2 -and $dataSize -gt 24 -and (& $fits $dataPointer $dataSize)) {
            if ([Text.Encoding]::ASCII.GetString($bytes, $dataPointer, 4) -ne 'RSDS') { continue }
            $guidBytes = New-Object byte[] 16
            [Array]::Copy($bytes, $dataPointer + 4, $guidBytes, 0, 16)
            $age = [BitConverter]::ToUInt32($bytes, $dataPointer + 20)
            $pathStart = [int64]$dataPointer + 24
            $pathEnd = $pathStart
            while ($pathEnd -lt $dataPointer + $dataSize -and $bytes[$pathEnd] -ne 0) { $pathEnd++ }
            if ($pathEnd -ge $dataPointer + $dataSize -or $pathEnd -eq $pathStart) { return $null }
            $pdb = [Text.Encoding]::UTF8.GetString($bytes, [int]$pathStart, [int]($pathEnd - $pathStart))
            $pdbLeaf = Split-Path -Leaf $pdb
            if ([string]::IsNullOrEmpty($pdbLeaf)) { return $null }
            return [pscustomobject]@{
                Guid    = ([guid]::new($guidBytes)).ToString().ToLowerInvariant()
                Age     = [int]$age
                Pdb     = $pdb
                PdbLeaf = $pdbLeaf
            }
        }
    }
    return $null
}

function Test-SymbolIdentity {
    # Whether the binary in the symbol directory is the build the trace captured (#873): its RSDS
    # signature and age must be among those the trace recorded for a PDB of the same file name.
    # Without this, a stale build/<preset>/bin silently resolves nothing, or the wrong code.
    param(
        $BinaryInfo,
        [AllowEmptyCollection()][object[]]$TraceIds,
        [Parameter(Mandatory = $true)][string]$ExpectedPdbLeaf
    )
    $candidates = @($TraceIds | Where-Object { $_.PdbLeaf -ieq $ExpectedPdbLeaf })
    if ($null -eq $BinaryInfo) {
        return [pscustomobject]@{ Status = 'BinaryUnreadable'; Detail = 'The symbol directory has no binary with a CodeView record to compare.'; TraceCandidates = $candidates.Count }
    }
    if ($candidates.Count -eq 0) {
        return [pscustomobject]@{ Status = 'NotInTrace'; Detail = "The trace recorded no image with $ExpectedPdbLeaf."; TraceCandidates = 0 }
    }
    $match = $candidates | Where-Object { $_.Guid -eq $BinaryInfo.Guid -and $_.Age -eq $BinaryInfo.Age } | Select-Object -First 1
    # The dbgid listing covers every image in the system-wide trace, not just the analyzed process,
    # so if more than one build with this PDB name ran, a match cannot say it is the analyzed one.
    $distinctBuilds = @($candidates | ForEach-Object { "$($_.Guid)/$($_.Age)" } | Sort-Object -Unique)
    if ($match -and $distinctBuilds.Count -gt 1) {
        $captured = ($candidates | Sort-Object Guid, Age -Unique | ForEach-Object { "{$($_.Guid)} age $($_.Age)" }) -join ', '
        return [pscustomobject]@{ Status = 'Ambiguous'; Detail = "The trace recorded $($distinctBuilds.Count) different builds with $ExpectedPdbLeaf ($captured). The symbol directory's binary is one of them, but which one the analyzed process ran cannot be told from the trace's image list."; TraceCandidates = $candidates.Count }
    }
    if ($match) {
        return [pscustomobject]@{ Status = 'Match'; Detail = "Signature {$($BinaryInfo.Guid)} age $($BinaryInfo.Age) matches the captured image."; TraceCandidates = $candidates.Count }
    }
    $captured = ($candidates | ForEach-Object { "{$($_.Guid)} age $($_.Age)" }) -join ', '
    return [pscustomobject]@{ Status = 'Mismatch'; Detail = "The symbol directory's binary is {$($BinaryInfo.Guid)} age $($BinaryInfo.Age), but the trace captured $captured, so it is a different build and cannot supply this trace's symbols. Any functions that did resolve came from the PDB path recorded in the image; the unresolved share shows how many did not."; TraceCandidates = $candidates.Count }
}

function Get-UnresolvedShare {
    # Share of a module's sampled weight whose function did not resolve (`module!"Unknown"`).
    param(
        [AllowEmptyCollection()][object[]]$FunctionRows,
        [Parameter(Mandatory = $true)][string]$Module
    )
    $moduleRows = @($FunctionRows | Where-Object { $_.Symbol -like "$Module!*" })
    $total = Get-WeightSum $moduleRows
    $unresolved = Get-WeightSum @($moduleRows | Where-Object { $_.Symbol -ieq "$Module!`"Unknown`"" })
    return [pscustomobject]@{
        Module          = $Module
        WeightUs        = $total
        UnresolvedUs    = $unresolved
        UnresolvedRatio = if ($total -gt 0) { [double]$unresolved / $total } else { $null }
        UnresolvedPct   = if ($total -gt 0) { [math]::Round(100.0 * $unresolved / $total, 2) } else { $null }
    }
}

function Get-TraceValidity {
    # Folds the checks into Valid / Degraded / Invalid with reasons (#873). Invalid traces are
    # still exported for inspection, but the script exits non-zero unless -AllowInvalid.
    param(
        $Loss,
        $Identity,
        $AppUnresolved,
        [double]$MaxLostEventsPct = 1.0,
        [double]$MaxUnresolvedPct = 10.0
    )
    $invalid = [System.Collections.Generic.List[string]]::new()
    $degraded = [System.Collections.Generic.List[string]]::new()

    if ($null -eq $Loss -or -not $Loss.Parsed) {
        $degraded.Add('Lost-event statistics could not be read; loss is unknown.')
    }
    else {
        $buffersKnown = $null -ne $Loss.LostBuffers
        if ($Loss.LostEvents -gt 0 -or ($buffersKnown -and $Loss.LostBuffers -gt 0)) {
            $pctText = if ($null -ne $Loss.LostEventsPct) { " ($($Loss.LostEventsPct)% of events)" } else { '' }
            $buffersText = if ($buffersKnown) { "$($Loss.LostBuffers) buffers were lost" } else { 'an unknown number of buffers were lost' }
            $message = "$($Loss.LostEvents) events and $buffersText$pctText."
            if ($buffersKnown -and $Loss.LostBuffers -gt 0) { $invalid.Add("$message Whole buffers were lost, so samples are missing in bursts.") }
            elseif ($null -eq $Loss.LostEventsRatio) { $invalid.Add("$message The share of events lost could not be computed.") }
            elseif (100.0 * $Loss.LostEventsRatio -gt $MaxLostEventsPct) { $invalid.Add("$message That is above the $MaxLostEventsPct% limit; the missing samples are not spread evenly, so shares are unreliable.") }
            else { $degraded.Add($message) }
        }
        if (-not $buffersKnown) {
            $degraded.Add('The lost-buffer count could not be read (only the -detail statistics were available), so whole-buffer loss was not checked.')
        }
    }

    if ($null -ne $Identity) {
        switch ($Identity.Status) {
            'Match' { }
            # Degraded, not Invalid: the symbol engine never loads a PDB whose signature differs,
            # so a mismatch loses function names rather than producing wrong ones, and the
            # unresolved-share check below makes the trace Invalid when too many are lost.
            'Mismatch' { $degraded.Add("Symbol identity: $($Identity.Detail)") }
            default { $degraded.Add("Symbol identity not verified: $($Identity.Detail)") }
        }
    }

    # Compared on the raw weight and exact ratio, not the rounded percentage.
    if ($null -ne $AppUnresolved -and $AppUnresolved.UnresolvedUs -gt 0 -and $null -ne $AppUnresolved.UnresolvedRatio) {
        $message = "$($AppUnresolved.UnresolvedPct)% of $($AppUnresolved.Module)'s sampled weight has no resolved function."
        if (100.0 * $AppUnresolved.UnresolvedRatio -gt $MaxUnresolvedPct) { $invalid.Add("$message Above the $MaxUnresolvedPct% limit.") }
        else { $degraded.Add($message) }
    }

    $status = if ($invalid.Count -gt 0) { 'Invalid' } elseif ($degraded.Count -gt 0) { 'Degraded' } else { 'Valid' }
    return [pscustomobject]@{ Status = $status; Invalid = @($invalid); Degraded = @($degraded) }
}

$script:KernelPdbNames = @('ntkrnlmp.pdb', 'ntoskrnl.pdb', 'ntkrla57.pdb')

function Get-SymbolStoreKey {
    # A PDB's directory key in a symbol store: its signature GUID without dashes, upper-case, then
    # its age in hex -- e.g. ntkrnlmp.pdb/0B3439338F40DDCD568A30E465A9576B1/ntkrnlmp.pdb.
    param([Parameter(Mandatory = $true)]$DebugId)
    return ($DebugId.Guid -replace '-', '').ToUpperInvariant() + ('{0:X}' -f $DebugId.Age)
}

function Save-KernelSymbols {
    # Fetches only the kernel's PDB, identified by the signature the trace recorded, into a local
    # symbol store (#931). Putting the Microsoft symbol server itself on _NT_SYMBOL_PATH makes
    # xperf fetch a PDB for every module of every process in a system-wide trace -- gigabytes --
    # when the ETW-path measurement only needs the kernel's.
    param(
        [AllowEmptyCollection()][object[]]$TraceIds,
        [Parameter(Mandatory = $true)][string]$StoreDirectory,
        [string]$ServerUrl = 'https://msdl.microsoft.com/download/symbols'
    )
    $kernelIds = @($TraceIds | Where-Object { $script:KernelPdbNames -contains $_.PdbLeaf.ToLowerInvariant() } | Sort-Object Guid, Age -Unique)
    foreach ($id in $kernelIds) {
        $key = Get-SymbolStoreKey $id
        $destination = Join-Path (Join-Path (Join-Path $StoreDirectory $id.PdbLeaf) $key) $id.PdbLeaf
        $status = 'cached'
        if (-not (Test-Path -LiteralPath $destination)) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            try {
                Invoke-WebRequest -Uri "$ServerUrl/$($id.PdbLeaf)/$key/$($id.PdbLeaf)" -OutFile $destination -UseBasicParsing
                $status = 'downloaded'
            }
            catch {
                Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
                $status = "unavailable: $($_.Exception.Message)"
            }
        }
        [pscustomobject]@{ Pdb = $id.PdbLeaf; Key = $key; Status = $status }
    }
}

# Kernel functions that make up ETW's own logging path (#912, #931): the cost of the recording
# itself, which can dominate the interval it records.
$script:EtwPathPatterns = @('ntoskrnl.exe!Etwp*', 'ntoskrnl.exe!PerfInfoLogSysCall*', 'ntoskrnl.exe!KeQueryPerformanceCounter*')

function Get-EtwOverhead {
    # Instrumentation overhead within the target process (#931): the share of its sampled weight
    # spent in ETW's logging path. Unresolved kernel frames cannot match the patterns, yet any of
    # them could be ETW logging, so:
    #   - every kernel sample resolved   -> Measured, one share;
    #   - some did not                   -> Bounded: no single share, only the range from "none of
    #                                       the unresolved weight is ETW" to "all of it is";
    #   - too few resolved to be useful  -> SymbolsUnavailable.
    param(
        [AllowEmptyCollection()][object[]]$FunctionRows,
        [int]$Top = 10,
        [double]$MinKernelResolvedPct = 90.0
    )
    $processWeight = Get-WeightSum $FunctionRows
    $kernel = Get-UnresolvedShare -FunctionRows $FunctionRows -Module 'ntoskrnl.exe'
    $kernelResolvedPct = if ($kernel.WeightUs -gt 0) { [math]::Round(100.0 - $kernel.UnresolvedPct, 2) } else { $null }
    $etwRows = @($FunctionRows | Where-Object { $sym = $_.Symbol; @($script:EtwPathPatterns | Where-Object { $sym -like $_ }).Count -gt 0 })
    $etwWeight = Get-WeightSum $etwRows

    $status = 'Measured'
    $note = $null
    $minPct = $null
    $maxPct = $null
    if ($processWeight -le 0) {
        $status = 'NoSamples'; $note = 'The process has no sampled weight in this range.'
    }
    elseif ($kernel.WeightUs -gt 0 -and 100.0 * (1.0 - $kernel.UnresolvedRatio) -lt $MinKernelResolvedPct) {
        $status = 'SymbolsUnavailable'
        $note = "Only $kernelResolvedPct% of ntoskrnl.exe's sampled weight resolved to functions, so the ETW-path share cannot be measured. Check access to the Microsoft symbol server."
    }
    elseif ($kernel.UnresolvedUs -gt 0) {
        $status = 'Bounded'
        $minPct = [math]::Round(100.0 * $etwWeight / $processWeight, 2)
        $maxPct = [math]::Round(100.0 * ($etwWeight + $kernel.UnresolvedUs) / $processWeight, 2)
        $note = "$($kernel.UnresolvedUs) us of ntoskrnl.exe's samples did not resolve and could be ETW logging, so the share is between $minPct% and $maxPct%."
    }

    $topRows = @($etwRows | Sort-Object Weight -Descending | Select-Object -First $Top | ForEach-Object { [pscustomobject]@{ Function = $_.Symbol; WeightUs = $_.Weight } })
    return [pscustomobject]@{
        Status               = $status
        Note                 = $note
        ProcessWeightUs      = $processWeight
        EtwPathWeightUs      = if ($status -in 'Measured', 'Bounded') { $etwWeight } else { $null }
        EtwPathSharePct      = if ($status -eq 'Measured') { [math]::Round(100.0 * $etwWeight / $processWeight, 2) } else { $null }
        EtwPathShareMinPct   = $minPct
        EtwPathShareMaxPct   = $maxPct
        KernelUnresolvedUs   = $kernel.UnresolvedUs
        KernelResolvedPct    = $kernelResolvedPct
        EtwPathPatterns      = $script:EtwPathPatterns
        # @(...) around the whole conditional: PowerShell unwraps a returned array, so zero rows
        # became null and one row a bare object, and the JSON reports changed shape.
        TopEtwPathFunctions  = @(if ($status -in 'Measured', 'Bounded') { $topRows })
    }
}
