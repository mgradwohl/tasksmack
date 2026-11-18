<#
.SYNOPSIS
    Fail if a shipped Windows PE imports a DLL outside an explicit allowlist (#4439).
.DESCRIPTION
    The release .zip and installer ship TaskSmack.exe, TaskSmackHelper.exe and Uninstall.exe with no
    Visual C++ Redistributable beside them: cmake/CompilerOptions.cmake links the CRT and C++ runtime
    statically (/MT) in every configuration but Debug, so a Release executable imports only Windows
    system DLLs. The windows-2025 and windows-11-arm runners have the redistributable installed, so a
    regression to /MD (#1398 shipped one: "MSVCP140.dll was not found") would pass every smoke launch
    and install check and fail on a clean PC. This reads the import tables instead.

    Reads each .exe and .dll under -Path directly from its bytes (DOS header, NT headers, section
    table, then the import and delay-import directories), so it works for x64 and ARM64 with no
    dumpbin or llvm-objdump. A PE that is not x64 or ARM64, or has no readable import table, fails,
    and so does one importing a DLL that is neither allowlisted nor a file shipped beside it. A
    forbidden import is reported as such and fails even when a file of that name ships in the package:
    vcruntime*.dll, msvcp*.dll, concrt*.dll, vccorlib*.dll, msvcr*.dll, ucrtbase.dll, any debug CRT
    (ucrtbased.dll, vcruntime140d.dll, ...) and the UCRT API sets (api-ms-win-crt-*), which a static
    CRT never imports.

    The allowlist is the Windows system DLLs and API sets the app uses, plus the DLLs shipped in the
    package. Delay-load imports count. Add a DLL to $AllowedSystemDlls only when it is part of Windows
    on every supported version (Windows 10 and later).
.PARAMETER Path
    A file, or a folder searched recursively for .exe and .dll files (the extracted .zip's folder, or
    the installed one).
.PARAMETER List
    Also print every import of every file.
#>
#Requires -Version 7
param(
    [Parameter(Mandatory)][string]$Path,
    [switch]$List
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Windows system DLLs the app imports (directly or delay-loaded), lower case.
$AllowedSystemDlls = @(
    'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll',
    'ole32.dll', 'oleaut32.dll', 'comctl32.dll', 'comdlg32.dll', 'shlwapi.dll', 'ws2_32.dll', 'iphlpapi.dll',
    'dwmapi.dll', 'imm32.dll', 'version.dll', 'winmm.dll', 'setupapi.dll', 'cfgmgr32.dll', 'pdh.dll',
    'psapi.dll', 'opengl32.dll', 'bcrypt.dll', 'crypt32.dll', 'wintrust.dll', 'dbghelp.dll', 'dxgi.dll',
    'uxtheme.dll', 'rpcrt4.dll', 'netapi32.dll', 'wtsapi32.dll', 'powrprof.dll', 'userenv.dll', 'secur32.dll',
    'wevtapi.dll', 'xmllite.dll', 'tdh.dll', 'sechost.dll', 'shcore.dll', 'hid.dll', 'cabinet.dll', 'wlanapi.dll',
    'dnsapi.dll', 'mpr.dll', 'propsys.dll', 'wininet.dll', 'winhttp.dll', 'd3d11.dll', 'dcomp.dll',
    'dwrite.dll', 'd2d1.dll', 'msimg32.dll', 'normaliz.dll', 'ncrypt.dll', 'authz.dll', 'virtdisk.dll',
    'avrt.dll', 'xinput1_4.dll', 'dinput8.dll', 'dxcore.dll', 'msvcrt.dll', 'winspool.drv'
)
# API sets are part of Windows 10+ (api-ms-win-core-*, ext-ms-win-*); the UCRT's are the static CRT's job.
$AllowedPatterns = @('^api-ms-win-(?!crt-).+\.dll$', '^ext-ms-win-.+\.dll$')
$ForbiddenPatterns = @(
    '^(vcruntime|msvcp|concrt|vccorlib|vcomp|mfc)\d.*\.dll$', '^msvcr\d+d?\.dll$', '^ucrtbased?\.dll$',
    '^api-ms-win-crt-.+\.dll$'
)

# Every import (import and delay-import directories) of one PE file, as lower-case DLL names. Throws a
# message saying what is wrong when it isn't a readable x64 or ARM64 PE.
function Get-PeImports {
    param([Parameter(Mandatory)][string]$File)
    $b = [IO.File]::ReadAllBytes($File)
    $fail = { param([string]$Why) throw "not a readable PE: $Why" }
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { & $fail 'no MZ header' }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) { & $fail 'no PE signature' }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    if ($machine -ne 0x8664 -and $machine -ne 0xAA64) { & $fail ('machine 0x{0:X4} is neither x64 nor ARM64' -f $machine) }
    $sections = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    if ($opt + 112 -gt $b.Length -or [BitConverter]::ToUInt16($b, $opt) -ne 0x20B) { & $fail 'not a PE32+ optional header' }
    $dirCount = [BitConverter]::ToUInt32($b, $opt + 108)
    $dirs = $opt + 112
    $table = $opt + $optSize
    if ($table + 40 * $sections -gt $b.Length) { & $fail 'section table past the end of the file' }
    $rvaToOffset = {
        param([uint32]$Rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $table + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12)
            $span = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            if ($Rva -ge $va -and $Rva -lt $va + $span) {
                $off = [int64]($Rva - $va) + [BitConverter]::ToUInt32($b, $s + 20)
                if ($off -ge $b.Length) { & $fail "RVA 0x$($Rva.ToString('X')) is past the end of the file" }
                return [int]$off
            }
        }
        & $fail "RVA 0x$($Rva.ToString('X')) is in no section"
    }
    $names = [System.Collections.Generic.List[string]]::new()
    # Directory index, descriptor size, offset of the DLL-name RVA in a descriptor.
    foreach ($dir in @(@(1, 20, 12), @(13, 32, 4))) {
        if ($dir[0] -ge $dirCount) { continue }
        $rva = [BitConverter]::ToUInt32($b, $dirs + 8 * $dir[0])
        if ($rva -eq 0) { continue }
        $o = & $rvaToOffset $rva
        while ($true) {
            if ($o + $dir[1] -gt $b.Length) { & $fail 'import directory runs past the end of the file' }
            $nameRva = [BitConverter]::ToUInt32($b, $o + $dir[2])
            if ($nameRva -eq 0) { break }
            $n = & $rvaToOffset $nameRva
            $e = $n
            while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }
            $names.Add([Text.Encoding]::ASCII.GetString($b, $n, $e - $n).ToLowerInvariant())
            $o += $dir[1]
        }
    }
    , @($names | Select-Object -Unique)
}

if (Test-Path -LiteralPath $Path -PathType Container) {
    $root = (Resolve-Path -LiteralPath $Path).Path
    $files = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })
} elseif (Test-Path -LiteralPath $Path -PathType Leaf) {
    $files = @(Get-Item -LiteralPath $Path)
} else {
    Write-Host "::error::$Path doesn't exist."
    exit 1
}
if ($files.Count -eq 0) {
    Write-Host "::error::No .exe or .dll files under $Path."
    exit 1
}
# DLLs shipped in the package may be imported by its other files.
$shipped = @($files | Where-Object { $_.Extension -eq '.dll' } | ForEach-Object { $_.Name.ToLowerInvariant() })

$failures = [System.Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    try {
        $imports = Get-PeImports -File $file.FullName
    } catch {
        $failures.Add("$($file.FullName): $($_.Exception.Message)")
        continue
    }
    if ($List) { Write-Host "$($file.Name): $($imports -join ', ')" }
    foreach ($dll in $imports) {
        if ($ForbiddenPatterns | Where-Object { $dll -match $_ }) {
            $failures.Add("$($file.Name) imports $dll, a Visual C++ or UCRT runtime DLL the package must not need: the CRT is linked statically (/MT, cmake/CompilerOptions.cmake), so a PC without the redistributable would fail to start it (#1398).")
        } elseif ($dll -in $AllowedSystemDlls -or $dll -in $shipped -or ($AllowedPatterns | Where-Object { $dll -match $_ })) {
            continue
        } else {
            $failures.Add("$($file.Name) imports $dll, which is neither an allowed Windows system DLL nor shipped with it. If it is part of Windows on every supported version, add it to `$AllowedSystemDlls in tools/check-windows-imports.ps1.")
        }
    }
}
if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Host "::error::$f" }
    Write-Host "$($failures.Count) import check failure(s) in $Path."
    exit 1
}
Write-Host "Import check passed: $($files.Count) file(s) under $Path import only allowed DLLs ($(($files.Name | Sort-Object) -join ', '))."
