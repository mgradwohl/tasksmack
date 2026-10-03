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

    $pattern = if ($TargetProcessId -ne 0) {
        '^\s*(?:.+?) \(' + [regex]::Escape([string]$TargetProcessId) + '\),\s*([0-9]+),\s*([0-9.]+),\s*(.+)$'
    }
    else {
        '^\s*' + [regex]::Escape($TargetProcessName) + ' \(([0-9]+)\),\s*([0-9]+),\s*([0-9.]+),\s*(.+)$'
    }

    foreach ($line in $Lines) {
        if ($line -match $pattern) {
            if ($TargetProcessId -ne 0) {
                [pscustomobject]@{
                    ProcessName = $TargetProcessName
                    ProcessId   = $TargetProcessId
                    Weight      = [int64]$Matches[1]
                    Usage       = [double]$Matches[2]
                    Symbol      = $Matches[3].Trim()
                }
            }
            else {
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
    $lostPct = $null
    if ($null -ne $lostEvents -and ($recorded + $lostEvents) -gt 0) {
        $lostPct = [math]::Round(100.0 * $lostEvents / ($recorded + $lostEvents), 3)
    }
    # A trace with no loss prints no warning, so a parsed header with no lost-event line means 0.
    if ($null -eq $lostEvents -and $null -ne $lostBuffers) { $lostEvents = 0 }
    return [pscustomobject]@{
        Parsed         = ($null -ne $lostEvents)
        LostEvents     = $lostEvents
        LostBuffers    = if ($null -ne $lostBuffers) { $lostBuffers } else { 0 }
        RecordedEvents = if ($providerRows -gt 0) { $recorded } else { $null }
        LostEventsPct  = $lostPct
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
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null } # 'MZ'
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) { return $null } # 'PE\0\0'
    $fileHeader = $peOffset + 4
    $sectionCount = [BitConverter]::ToUInt16($bytes, $fileHeader + 2)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $fileHeader + 16)
    $optional = $fileHeader + 20
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $dataDirectories = switch ($magic) { 0x10B { $optional + 96 } 0x20B { $optional + 112 } default { return $null } }
    $debugRva = [BitConverter]::ToUInt32($bytes, $dataDirectories + 6 * 8) # IMAGE_DIRECTORY_ENTRY_DEBUG
    $debugSize = [BitConverter]::ToUInt32($bytes, $dataDirectories + 6 * 8 + 4)
    if ($debugRva -eq 0 -or $debugSize -eq 0) { return $null }

    # RVA -> file offset through the section table.
    $sections = $optional + $optionalSize
    $debugOffset = $null
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $s = $sections + 40 * $i
        $virtualSize = [BitConverter]::ToUInt32($bytes, $s + 8)
        $virtualAddress = [BitConverter]::ToUInt32($bytes, $s + 12)
        $rawSize = [BitConverter]::ToUInt32($bytes, $s + 16)
        $rawPointer = [BitConverter]::ToUInt32($bytes, $s + 20)
        $span = [Math]::Max($virtualSize, $rawSize)
        if ($debugRva -ge $virtualAddress -and $debugRva -lt $virtualAddress + $span) {
            $debugOffset = $rawPointer + ($debugRva - $virtualAddress)
            break
        }
    }
    if ($null -eq $debugOffset) { return $null }

    for ($entry = $debugOffset; $entry + 28 -le $debugOffset + $debugSize; $entry += 28) {
        $type = [BitConverter]::ToUInt32($bytes, $entry + 12)
        $dataSize = [BitConverter]::ToUInt32($bytes, $entry + 16)
        $dataPointer = [BitConverter]::ToUInt32($bytes, $entry + 24)
        if ($type -eq 2 -and $dataSize -ge 24 -and $dataPointer + 24 -le $bytes.Length) { # IMAGE_DEBUG_TYPE_CODEVIEW
            if ([Text.Encoding]::ASCII.GetString($bytes, $dataPointer, 4) -ne 'RSDS') { continue }
            $guidBytes = New-Object byte[] 16
            [Array]::Copy($bytes, $dataPointer + 4, $guidBytes, 0, 16)
            $age = [BitConverter]::ToUInt32($bytes, $dataPointer + 20)
            $pathEnd = $dataPointer + 24
            while ($pathEnd -lt $bytes.Length -and $pathEnd -lt $dataPointer + $dataSize -and $bytes[$pathEnd] -ne 0) { $pathEnd++ }
            $pdb = [Text.Encoding]::UTF8.GetString($bytes, $dataPointer + 24, $pathEnd - ($dataPointer + 24))
            return [pscustomobject]@{
                Guid    = ([guid]::new($guidBytes)).ToString().ToLowerInvariant()
                Age     = [int]$age
                Pdb     = $pdb
                PdbLeaf = (Split-Path -Leaf $pdb)
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
    elseif ($Loss.LostEvents -gt 0 -or $Loss.LostBuffers -gt 0) {
        $pctText = if ($null -ne $Loss.LostEventsPct) { " ($($Loss.LostEventsPct)% of events)" } else { '' }
        $message = "$($Loss.LostEvents) events and $($Loss.LostBuffers) buffers were lost$pctText."
        if ($Loss.LostBuffers -gt 0) { $invalid.Add("$message Whole buffers were lost, so samples are missing in bursts.") }
        elseif ($null -eq $Loss.LostEventsPct) { $invalid.Add("$message The share of events lost could not be computed.") }
        elseif ($Loss.LostEventsPct -gt $MaxLostEventsPct) { $invalid.Add("$message That is above the $MaxLostEventsPct% limit; the missing samples are not spread evenly, so shares are unreliable.") }
        else { $degraded.Add($message) }
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

    if ($null -ne $AppUnresolved -and $null -ne $AppUnresolved.UnresolvedPct -and $AppUnresolved.UnresolvedPct -gt 0) {
        $message = "$($AppUnresolved.UnresolvedPct)% of $($AppUnresolved.Module)'s sampled weight has no resolved function."
        if ($AppUnresolved.UnresolvedPct -gt $MaxUnresolvedPct) { $invalid.Add("$message Above the $MaxUnresolvedPct% limit.") }
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
    # spent in ETW's logging path. Reported as unavailable, not as a low share, when the kernel's
    # functions did not resolve, since unresolved frames cannot match the patterns at all.
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
    if ($processWeight -le 0) {
        $status = 'NoSamples'; $note = 'The process has no sampled weight in this range.'
    }
    elseif ($kernel.WeightUs -gt 0 -and $kernelResolvedPct -lt $MinKernelResolvedPct) {
        $status = 'SymbolsUnavailable'
        $note = "Only $kernelResolvedPct% of ntoskrnl.exe's sampled weight resolved to functions, so the ETW-path share cannot be measured. Check access to the Microsoft symbol server."
    }

    $topRows = @($etwRows | Sort-Object Weight -Descending | Select-Object -First $Top | ForEach-Object { [pscustomobject]@{ Function = $_.Symbol; WeightUs = $_.Weight } })
    return [pscustomobject]@{
        Status               = $status
        Note                 = $note
        ProcessWeightUs      = $processWeight
        EtwPathWeightUs      = if ($status -eq 'Measured') { $etwWeight } else { $null }
        EtwPathSharePct      = if ($status -eq 'Measured') { [math]::Round(100.0 * $etwWeight / $processWeight, 2) } else { $null }
        KernelResolvedPct    = $kernelResolvedPct
        EtwPathPatterns      = $script:EtwPathPatterns
        TopEtwPathFunctions  = if ($status -eq 'Measured') { $topRows } else { @() }
    }
}
