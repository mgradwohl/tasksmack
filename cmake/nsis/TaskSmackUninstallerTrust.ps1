<#
.SYNOPSIS
    The NSIS installer's checks on a previously installed TaskSmack folder (#4444, #4445, #4659, #4660).
.DESCRIPTION
    cmake/CPackProjectConfig.cmake.in embeds this script in the installer and runs it (Windows PowerShell,
    through nsExec) from the upgrade step, which an elevated installer, or a SYSTEM WinGet or Intune
    upgrade, runs silently.

    -Mode Uninstaller: exit 0 when running the old Uninstall.exe at -Path, elevated, is safe, 1 when it isn't.
      Safe means admin-only: the file, its folder and every folder above can be changed by no one but SYSTEM,
      Administrators and TrustedInstaller (the owners, the write rights of every allow entry), none is a link
      or junction, and the volume is a fixed NTFS one: the same test TaskSmackHelper and Replace Task Manager
      apply to an install (BrokerHelper::checkUnchangeableNamed). A valid signature isn't enough (#4660): the
      verdict is on a path a standard user could swap before the installer starts it, and even the genuine
      uninstaller deletes its files by path, through any junction a standard user planted in the folder.
    -Mode Folder: exit 0 when -Path, or the nearest folder above it that exists, is admin-only (a
      warning in the installer's log otherwise; nothing is refused).

    Any error counts as not trusted. The first line written says why. It only reads (#4659: an elevated
    delete in a folder a standard user can write follows their junctions), so tests/tools/test_nsis_upgrade.py
    runs it on temporary folders, unelevated.
.PARAMETER Mode
    Uninstaller or Folder.
.PARAMETER Path
    The Uninstall.exe, or the folder.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Uninstaller', 'Folder')][string]$Mode,
    [Parameter(Mandatory)][string]$Path
)
$ErrorActionPreference = 'Stop'

# SYSTEM, BUILTIN\Administrators, NT SERVICE\TrustedInstaller.
$TrustedSids = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
$Rights = [System.Security.AccessControl.FileSystemRights]
# What changes an item itself: its data, its attributes, its name and its security.
$ChangesItem = $Rights::WriteData -bor $Rights::AppendData -bor $Rights::WriteExtendedAttributes -bor
    $Rights::WriteAttributes -bor $Rights::Delete -bor $Rights::ChangePermissions -bor $Rights::TakeOwnership
# What lets someone replace a folder's child, or the folder: removing children, the folder's own name and security.
$ChangesChildren = $Rights::DeleteSubdirectoriesAndFiles -bor $Rights::Delete -bor $Rights::ChangePermissions -bor
    $Rights::TakeOwnership

# Why $Item can be changed by someone who isn't an administrator, or $null when no one can.
function Get-ChangeProblem([string]$Item, $Mask) {
    $info = Get-Item -LiteralPath $Item -Force
    if ($info.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { return "$Item is a link or junction" }
    # .NET's own reader, not Get-Acl: no module to load, and the same answer in any PSModulePath.
    $acl = $info.GetAccessControl()
    $owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin $TrustedSids) { return "$Item is owned by $owner" }
    foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -ne 'Allow') { continue }
        # An inherit-only entry (CREATOR OWNER's, say) applies to what is made inside, not to this item.
        if ($rule.PropagationFlags -band [System.Security.AccessControl.PropagationFlags]::InheritOnly) { continue }
        $sid = $rule.IdentityReference.Value
        if ($sid -in $TrustedSids) { continue }
        if (($rule.FileSystemRights -band $Mask) -ne 0) { return "$sid can change $Item" }
    }
    return $null
}

# Why $Target (a file or a folder) and the folders above it aren't admin-only, or $null when they are.
function Get-AdminOnlyProblem([string]$Target) {
    $full = [System.IO.Path]::GetFullPath($Target)
    $root = [System.IO.Path]::GetPathRoot($full)
    if (-not $root -or $root.StartsWith('\\')) { return "$full isn't on a local drive" }
    $drive = New-Object System.IO.DriveInfo($root)
    if ($drive.DriveType -ne 'Fixed' -or $drive.DriveFormat -ne 'NTFS') { return "$root isn't a fixed NTFS volume" }
    $item = Get-Item -LiteralPath $full -Force
    $problem = Get-ChangeProblem $full $ChangesItem
    if ($problem) { return $problem }
    $folder = if ($item.PSIsContainer) { $full } else { [System.IO.Path]::GetDirectoryName($full) }
    if (-not $item.PSIsContainer) {
        $problem = Get-ChangeProblem $folder ($ChangesItem -bor $ChangesChildren)
        if ($problem) { return $problem }
    }
    $parent = [System.IO.Path]::GetDirectoryName($folder)
    while ($parent) {
        $problem = Get-ChangeProblem $parent $ChangesChildren
        if ($problem) { return $problem }
        $parent = [System.IO.Path]::GetDirectoryName($parent)
    }
    return $null
}

try {
    switch ($Mode) {
        'Uninstaller' {
            $problem = Get-AdminOnlyProblem $Path
            if (-not $problem) { Write-Output "$Path is trusted: it and its folders can be changed only by administrators."; exit 0 }
            Write-Output "$Path is not trusted: $problem."
            exit 1
        }
        'Folder' {
            $existing = [System.IO.Path]::GetFullPath($Path)
            while ($existing -and -not (Test-Path -LiteralPath $existing)) { $existing = [System.IO.Path]::GetDirectoryName($existing) }
            if (-not $existing) { Write-Output "$Path has no folder above it that exists."; exit 1 }
            $problem = Get-AdminOnlyProblem $existing
            if (-not $problem) { Write-Output "$Path can be changed only by administrators."; exit 0 }
            Write-Output "$Path isn't admin-only: $problem."
            exit 1
        }
    }
} catch {
    Write-Output "$Path is not trusted: $($_.Exception.Message)"
    exit 1
}
