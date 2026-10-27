<#
.SYNOPSIS
    Build the Windows NSIS installer from a packaged .zip and a prebuilt uninstaller (#2302).
.DESCRIPTION
    The installer's second pass, which release.yml's build-windows-installer job runs. CPack's NSIS run in
    build-windows is the first: it writes project.nsi (CPack's NSIS script for this build) and copies out
    the uninstaller makensis built (cmake/CPackProjectConfig.cmake.in). An Authenticode signature can't be
    added to files inside an installer, and NSIS writes its uninstaller at install time, so the installer
    release.yml publishes is built again here, from the signed files:

    1. The .zip's top-level folder becomes the folder project.nsi installs from (its INST_DIR, which has
       the same name), so the installer installs exactly the files the .zip holds: with signing on,
       sign-windows' signed .zip.
    2. Those files must be exactly the ones project.nsi's uninstaller deletes, and their folders exactly
       the ones it removes (its Delete and RMDir "$INSTDIR\..." lines, which CPack wrote from the first
       pass's tree), so an uninstall leaves nothing behind.
    3. makensis builds project.nsi with /DTASKSMACK_UNINSTALLER=<Uninstaller>, which makes its
       !uninstfinalize copy that file over the uninstaller it built: the installer embeds and installs
       the given uninstaller (with signing on, the one sign-windows signed) byte for byte.
    4. The installer (project.nsi's OutFile) is copied to OutputDir.

    project.nsi names its folders and resources (the icon, the licence) by the absolute paths of the
    build-windows run, so this runs on the same runner image, in a checkout at the same place: INST_DIR
    and the OutFile folder are created here, and the checkout provides the rest. INST_DIR must not exist
    yet, so nothing stale is packaged.
.PARAMETER ProjectDir
    The folder holding the first pass's project.nsi and NSIS.InstallOptions.ini.
.PARAMETER Zip
    The packaged .zip, one top-level folder holding the files to install.
.PARAMETER Uninstaller
    The uninstaller to embed.
.PARAMETER OutputDir
    Where the installer is copied to.
.PARAMETER Makensis
    makensis.exe (NSIS 3.08 or later). Omitted, $env:MAKENSIS, else the one on PATH.
#>
param(
    [Parameter(Mandatory)][string]$ProjectDir,
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$Uninstaller,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Makensis = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    Write-Host "::error title=Windows installer::$Message"
    exit 1
}

if (-not $Makensis) {
    $Makensis = if ($env:MAKENSIS) { $env:MAKENSIS } else { (Get-Command makensis -ErrorAction SilentlyContinue).Source }
}
if (-not $Makensis -or -not (Test-Path -LiteralPath $Makensis -PathType Leaf)) { Fail "makensis not found ('$Makensis')." }

$projectFile = Join-Path $ProjectDir 'project.nsi'
$optionsFile = Join-Path $ProjectDir 'NSIS.InstallOptions.ini'
foreach ($file in $projectFile, $optionsFile, $Zip, $Uninstaller) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "$file not found." }
}
$Uninstaller = (Resolve-Path -LiteralPath $Uninstaller).Path
# It goes into a makensis /D define and from there into a quoted copy command (CPackProjectConfig.cmake.in).
if ($Uninstaller -match '["''%]') { Fail "The uninstaller's path can't hold a quote or a percent sign: $Uninstaller" }

$script = Get-Content -LiteralPath $projectFile
$instDirLine = @($script | Where-Object { $_ -match '^\s*!define\s+INST_DIR\s+"([^"]+)"\s*$' })
if ($instDirLine.Count -ne 1) { Fail "$projectFile has $($instDirLine.Count) '!define INST_DIR' lines, expected one." }
$null = $instDirLine[0] -match '^\s*!define\s+INST_DIR\s+"([^"]+)"\s*$'
$instDir = [System.IO.Path]::GetFullPath($Matches[1])
$outFileLine = @($script | Where-Object { $_ -match '^\s*OutFile\s+"([^"]+)"\s*$' })
if ($outFileLine.Count -ne 1) { Fail "$projectFile has $($outFileLine.Count) OutFile lines, expected one." }
$null = $outFileLine[0] -match '^\s*OutFile\s+"([^"]+)"\s*$'
$outFile = [System.IO.Path]::GetFullPath($Matches[1])
if (Test-Path -LiteralPath $instDir) { Fail "$instDir (project.nsi's INST_DIR) already exists; refusing to package what's in it." }

# 1. The .zip's files, as INST_DIR. Expand-Archive refuses an entry that would land outside its destination.
$work = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-installer-$([guid]::NewGuid().ToString('N'))"
try {
    Expand-Archive -LiteralPath $Zip -DestinationPath $work
    $top = @(Get-ChildItem -LiteralPath $work -Force)
    if ($top.Count -ne 1 -or -not $top[0].PSIsContainer) {
        Fail "$Zip should hold one top-level folder; it holds: $(($top | ForEach-Object Name) -join ', ')"
    }
    # Both are CPack's package file name (TaskSmack-<version>-<system>), so a .zip from another version or
    # architecture than project.nsi's is refused.
    if ($top[0].Name -ne (Split-Path -Leaf $instDir)) {
        Fail "$Zip's top-level folder is $($top[0].Name), but project.nsi installs from $(Split-Path -Leaf $instDir)."
    }
    $nsisDir = Split-Path -Parent $instDir
    New-Item -ItemType Directory -Force -Path $nsisDir, (Split-Path -Parent $outFile) | Out-Null
    Move-Item -LiteralPath $top[0].FullName -Destination $instDir
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
$installed = @(Get-ChildItem -LiteralPath $instDir -Recurse -File -Force |
        ForEach-Object { $_.FullName.Substring($instDir.Length).TrimStart('\') } | Sort-Object)
Write-Host "$($installed.Count) files from $(Split-Path -Leaf $Zip) in $instDir"

# 2. The uninstaller deletes exactly those files (itself apart) and removes exactly their folders. CPack
# writes a Delete line for every entry of the first pass's tree, folders included, and an RMDir line for
# each folder, so a Delete line whose path is RMDir'd, or is one of the .zip's folders, names a folder, not a
# file; the folder check below then holds the folders to account.
$uninstallName = 'Uninstall.exe'
$installedDirs = @(Get-ChildItem -LiteralPath $instDir -Recurse -Directory -Force |
        ForEach-Object { $_.FullName.Substring($instDir.Length).TrimStart('\') } | Sort-Object)
$removedDirs = @($script | ForEach-Object {
        if ($_ -match '^\s*RMDir\s+"\$INSTDIR\\(.+)"\s*$') { $Matches[1] }
    } | Sort-Object)
$deleted = @($script | ForEach-Object {
        if ($_ -match '^\s*Delete\s+"\$INSTDIR\\(.+)"\s*$') { $Matches[1] }
    } | Where-Object { $_ -ne $uninstallName -and $_ -ne 'AddRemove.exe' -and $removedDirs -notcontains $_ -and $installedDirs -notcontains $_ } |
    Sort-Object)
function Get-Difference([string[]]$Have, [string[]]$Want) {
    $only = @($Have | Where-Object { $Want -notcontains $_ })
    if ($only.Count) { $only -join ', ' } else { 'none' }
}
$onlyInstalled = Get-Difference $installed $deleted
$onlyDeleted = Get-Difference $deleted $installed
if ($onlyInstalled -ne 'none' -or $onlyDeleted -ne 'none') {
    Fail ("$(Split-Path -Leaf $Zip)'s files aren't the ones project.nsi's uninstaller deletes. Only in the .zip: " +
        "$onlyInstalled; only deleted: $onlyDeleted.")
}
$onlyInstalled = Get-Difference $installedDirs $removedDirs
$onlyRemoved = Get-Difference $removedDirs $installedDirs
if ($onlyInstalled -ne 'none' -or $onlyRemoved -ne 'none') {
    Fail ("$(Split-Path -Leaf $Zip)'s folders aren't the ones project.nsi's uninstaller removes. Only in the .zip: " +
        "$onlyInstalled; only removed: $onlyRemoved.")
}
if ($installed -contains $uninstallName) { Fail "$(Split-Path -Leaf $Zip) holds an $uninstallName of its own." }

# 3. makensis, in project.nsi's own folder (where it reads NSIS.InstallOptions.ini from).
$project = Join-Path $nsisDir 'project.nsi'
if ([System.IO.Path]::GetFullPath($ProjectDir).TrimEnd('\') -ne $nsisDir.TrimEnd('\')) {
    Copy-Item -LiteralPath $projectFile -Destination $project
    Copy-Item -LiteralPath $optionsFile -Destination (Join-Path $nsisDir 'NSIS.InstallOptions.ini')
}
if (Test-Path -LiteralPath $outFile) { Remove-Item -LiteralPath $outFile -Force }
Write-Host "Embedding $Uninstaller ($((Get-FileHash -LiteralPath $Uninstaller -Algorithm SHA256).Hash))"
& $Makensis /V2 "/DTASKSMACK_UNINSTALLER=$Uninstaller" $project
if ($LASTEXITCODE -ne 0) { Fail "makensis exited with $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $outFile -PathType Leaf)) { Fail "makensis made no $outFile." }

# 4. The installer.
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$result = Join-Path $OutputDir (Split-Path -Leaf $outFile)
Copy-Item -LiteralPath $outFile -Destination $result -Force
Write-Host "$result $((Get-Item -LiteralPath $result).Length) bytes, SHA-256 $((Get-FileHash -LiteralPath $result -Algorithm SHA256).Hash)"
exit 0
