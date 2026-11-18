# Tests for tools/check-windows-imports.ps1 (#4439): the import checker that fails a shipped Windows
# PE importing the MSVC runtime or any DLL outside its allowlist. The PEs are built here from bytes
# (a DOS header, PE32+ headers and one section holding import and delay-import tables), x64 and ARM64,
# so nothing is compiled, launched or installed. The positive case is also run on a system binary.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$checker = Join-Path $PSScriptRoot 'check-windows-imports.ps1'
$pwsh = (Get-Process -Id $PID).Path

# A minimal PE32+ with one section (RVA 0x1000, file offset 0x200) holding the import directory, the
# delay-import directory and the DLL names.
function New-TestPe {
    param([uint16]$Machine = 0x8664, [string[]]$Imports = @(), [string[]]$DelayImports = @())
    $importSize = 20 * ($Imports.Count + 1)
    $delaySize = 32 * ($DelayImports.Count + 1)
    $nameRva = 0x1000 + $importSize + $delaySize
    $section = [byte[]]::new($importSize + $delaySize)
    $names = [System.IO.MemoryStream]::new()
    $nameRvas = @{}
    foreach ($dll in $Imports + $DelayImports) {
        $nameRvas[$dll] = $nameRva + [int]$names.Length
        $bytes = [Text.Encoding]::ASCII.GetBytes($dll + "`0")
        $names.Write($bytes, 0, $bytes.Length)
    }
    for ($i = 0; $i -lt $Imports.Count; $i++) {
        [BitConverter]::GetBytes([uint32]$nameRvas[$Imports[$i]]).CopyTo($section, 20 * $i + 12)
    }
    for ($i = 0; $i -lt $DelayImports.Count; $i++) {
        [BitConverter]::GetBytes([uint32]1).CopyTo($section, $importSize + 32 * $i)
        [BitConverter]::GetBytes([uint32]$nameRvas[$DelayImports[$i]]).CopyTo($section, $importSize + 32 * $i + 4)
    }
    $section += $names.ToArray()
    $file = [byte[]]::new(0x200 + $section.Length)
    $file[0] = 0x4D; $file[1] = 0x5A
    [BitConverter]::GetBytes([int]0x40).CopyTo($file, 0x3C)
    $pe = 0x40
    [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($file, $pe)
    [BitConverter]::GetBytes($Machine).CopyTo($file, $pe + 4)
    [BitConverter]::GetBytes([uint16]1).CopyTo($file, $pe + 6)      # one section
    [BitConverter]::GetBytes([uint16]240).CopyTo($file, $pe + 20)   # optional header size
    $opt = $pe + 24
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($file, $opt)
    [BitConverter]::GetBytes([uint32]16).CopyTo($file, $opt + 108)  # data directory count
    $dirs = $opt + 112
    if ($Imports.Count -gt 0) {
        [BitConverter]::GetBytes([uint32]0x1000).CopyTo($file, $dirs + 8)
        [BitConverter]::GetBytes([uint32]$importSize).CopyTo($file, $dirs + 12)
    }
    if ($DelayImports.Count -gt 0) {
        [BitConverter]::GetBytes([uint32](0x1000 + $importSize)).CopyTo($file, $dirs + 8 * 13)
        [BitConverter]::GetBytes([uint32]$delaySize).CopyTo($file, $dirs + 8 * 13 + 4)
    }
    $sec = $opt + 240
    [Text.Encoding]::ASCII.GetBytes('.idata').CopyTo($file, $sec)
    [BitConverter]::GetBytes([uint32]$section.Length).CopyTo($file, $sec + 8)    # virtual size
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($file, $sec + 12)            # virtual address
    [BitConverter]::GetBytes([uint32]$section.Length).CopyTo($file, $sec + 16)   # raw size
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($file, $sec + 20)             # raw offset
    $section.CopyTo($file, 0x200)
    , $file
}

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-check-imports-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $script:n = 0
    # Writes one case's package folder, runs the checker on it and returns its exit code and output.
    function Invoke-Case {
        param([hashtable]$Files)
        $dir = Join-Path $root ('case' + (++$script:n))
        New-Item -ItemType Directory -Path $dir | Out-Null
        foreach ($name in $Files.Keys) { [IO.File]::WriteAllBytes((Join-Path $dir $name), $Files[$name]) }
        $out = & $pwsh -NoProfile -File $checker -Path $dir -List 2>&1 | Out-String
        [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
    }
    function Assert-Passes {
        param([string]$What, [hashtable]$Files)
        $r = Invoke-Case $Files
        Assert-True ($r.Code -eq 0) "$What should pass, exit $($r.Code): $($r.Out)"
    }
    function Assert-Fails {
        param([string]$What, [hashtable]$Files, [string]$Mentions)
        $r = Invoke-Case $Files
        Assert-True ($r.Code -ne 0) "$What should fail: $($r.Out)"
        Assert-True ($r.Out -match [regex]::Escape($Mentions)) "$What should mention $Mentions : $($r.Out)"
    }

    $system = @('kernel32.dll', 'USER32.dll', 'advapi32.dll', 'api-ms-win-core-synch-l1-2-0.dll', 'xmllite.dll')
    foreach ($machine in 0x8664, 0xAA64) {
        $arch = '0x{0:X4}' -f $machine
        Assert-Passes "system DLLs only ($arch)" @{ 'TaskSmack.exe' = (New-TestPe $machine $system) }
        Assert-Passes "delay-load of a system DLL ($arch)" @{ 'TaskSmack.exe' = (New-TestPe $machine @('kernel32.dll') @('dbghelp.dll')) }
        Assert-Fails "VCRUNTIME140.dll ($arch)" @{ 'TaskSmack.exe' = (New-TestPe $machine ($system + 'VCRUNTIME140.dll')) } 'vcruntime140.dll'
    }
    Assert-Passes 'no imports at all' @{ 'TaskSmack.exe' = (New-TestPe) }

    foreach ($bad in 'MSVCP140.dll', 'msvcp140_1.dll', 'vcruntime140_1.dll', 'CONCRT140.dll', 'vccorlib140.dll',
            'ucrtbase.dll', 'ucrtbased.dll', 'msvcp140d.dll', 'vcruntime140d.dll', 'msvcp140d_atomic_wait.dll',
            'api-ms-win-crt-runtime-l1-1-0.dll', 'api-ms-win-crt-stdio-l1-1-0.dll', 'msvcr120.dll') {
        Assert-Fails "forbidden $bad" @{ 'TaskSmackHelper.exe' = (New-TestPe 0x8664 @('kernel32.dll', $bad)) } $bad.ToLowerInvariant()
    }
    Assert-Fails 'a delay-load of the MSVC runtime' @{ 'Uninstall.exe' = (New-TestPe 0x8664 @('kernel32.dll') @('msvcp140.dll')) } 'msvcp140.dll'
    Assert-Fails 'a DLL outside the allowlist' @{ 'TaskSmack.exe' = (New-TestPe 0x8664 @('kernel32.dll', 'surprise.dll')) } 'surprise.dll'

    # A DLL shipped beside the exe may be imported; the runtime may not, even when it ships.
    $dll = New-TestPe 0x8664 @('kernel32.dll')
    Assert-Passes 'a shipped DLL' @{ 'TaskSmack.exe' = (New-TestPe 0x8664 @('kernel32.dll', 'helper.dll')); 'helper.dll' = $dll }
    Assert-Fails 'a shipped runtime DLL' @{ 'TaskSmack.exe' = (New-TestPe 0x8664 @('kernel32.dll', 'vcruntime140.dll')); 'vcruntime140.dll' = $dll } 'vcruntime140.dll'
    Assert-Fails 'a shipped DLL that imports the runtime' @{ 'TaskSmack.exe' = (New-TestPe 0x8664 @('kernel32.dll')); 'helper.dll' = (New-TestPe 0xAA64 @('msvcp140.dll')) } 'helper.dll imports msvcp140.dll'

    # Files the checker can't read fail it rather than pass unchecked.
    Assert-Fails 'a file that is not a PE' @{ 'TaskSmack.exe' = [Text.Encoding]::ASCII.GetBytes('not an executable at all, just text padded out past sixty-four bytes') } 'not a readable PE'
    Assert-Fails 'an x86 machine' @{ 'TaskSmack.exe' = (New-TestPe 0x014C @('kernel32.dll')) } 'neither x64 nor ARM64'
    $truncated = New-TestPe 0x8664 @('kernel32.dll')
    Assert-Fails 'a truncated PE' @{ 'TaskSmack.exe' = $truncated[0..0x120] } 'not a readable PE'

    # A folder with no executable, and a missing path, fail.
    $empty = Join-Path $root 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    & $pwsh -NoProfile -File $checker -Path $empty *> $null
    Assert-True ($LASTEXITCODE -ne 0) 'An empty folder should fail.'
    & $pwsh -NoProfile -File $checker -Path (Join-Path $root 'missing') *> $null
    Assert-True ($LASTEXITCODE -ne 0) 'A missing path should fail.'

    # A system binary the allowlist covers, read through the same parser.
    $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
    if (Test-Path -LiteralPath $cmdExe) {
        $out = & $pwsh -NoProfile -File $checker -Path $cmdExe -List 2>&1 | Out-String
        Assert-True ($out -match 'kernel32\.dll|api-ms-win|msvcrt\.dll') "cmd.exe's imports weren't read: $out"
        Assert-True ($out -notmatch 'vcruntime|msvcp') "cmd.exe imports the MSVC runtime?: $out"
    }
    Write-Host 'check-windows-imports tests passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
