<#
.SYNOPSIS
    The NSIS installer's checks on a TaskSmack folder, and its lock of the install folder (#4444, #4445, #4659,
    #4660, #4669).
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
    -Mode Folder: exit 0 when -Path, or the nearest folder above it that exists, is admin-only (the
      uninstaller deletes nothing in one that isn't).
    -Mode CanLock: exit 0 when the install folder -Path is admin-only, or Lock can make it so; 1 when it can't:
      it holds something an install doesn't put there, it or a folder above it is a link or junction, or a
      folder above it isn't admin-only, so the folder itself could be replaced. Changes nothing (#4669).
    -Mode Lock: makes the install folder -Path admin-only, as Program Files is, or exits 1 and says why. The
      folder is owned by Administrators, with only SYSTEM and Administrators able to change it and Users able to
      read and run what is in it, and everything in it inherits that. It goes top-down, and each folder is locked
      before what is in it is read, so no one who isn't an administrator can add, remove or swap anything in it by
      then. icacls runs with /L, so a link or junction is changed itself, never its target, and any link or
      junction in the folder refuses the lock. The whole tree is checked afterwards (#4669).

    Any error counts as not trusted. The first line written says why. Only Lock changes anything (#4659: an
    elevated delete in a folder a standard user can write follows their junctions; #4669: so does an elevated
    write). tests/tools/test_nsis_upgrade.py runs the script on temporary folders, unelevated.
.PARAMETER Mode
    Uninstaller, Folder, CanLock or Lock.
.PARAMETER Path
    The Uninstall.exe, or the folder.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Uninstaller', 'Folder', 'CanLock', 'Lock')][string]$Mode,
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

# What an install puts in its folder (CPack's file list): the only names a folder may hold for Lock to take it.
$InstallNames = @('TaskSmack.exe', 'TaskSmackHelper.exe', 'LICENSE.txt', 'Uninstall.exe', 'third_party_licenses', 'share')
$AdministratorsSid = '*S-1-5-32-544'
$Icacls = Join-Path ([System.Environment]::SystemDirectory) 'icacls.exe'

# Whether $Item is a reparse point (a link, a junction, a mount point): its own attributes, never its target's.
function Test-ReparsePoint([string]$Item) {
    return ([System.IO.File]::GetAttributes($Item) -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
}

# Why a folder above $Folder could be replaced by someone who isn't an administrator (and with it $Folder), or $null.
function Get-AboveProblem([string]$Folder) {
    $root = [System.IO.Path]::GetPathRoot($Folder)
    if (-not $root -or $root.StartsWith('\\')) { return "$Folder isn't on a local drive" }
    $drive = New-Object System.IO.DriveInfo($root)
    if ($drive.DriveType -ne 'Fixed' -or $drive.DriveFormat -ne 'NTFS') { return "$root isn't a fixed NTFS volume" }
    $parent = [System.IO.Path]::GetDirectoryName($Folder)
    while ($parent) {
        $problem = Get-ChangeProblem $parent $ChangesChildren
        if ($problem) { return $problem }
        $parent = [System.IO.Path]::GetDirectoryName($parent)
    }
    return $null
}

# The first thing in $Folder an install doesn't put there (anything in share but share\tasksmack too), or $null.
function Get-ForeignEntry([string]$Folder) {
    foreach ($child in [System.IO.Directory]::EnumerateFileSystemEntries($Folder)) {
        if ([System.IO.Path]::GetFileName($child) -notin $InstallNames) { return $child }
    }
    $share = Join-Path $Folder 'share'
    if ([System.IO.Directory]::Exists($share)) {
        if (Test-ReparsePoint $share) { return "$share (a link or junction)" }
        foreach ($child in [System.IO.Directory]::EnumerateFileSystemEntries($share)) {
            if ([System.IO.Path]::GetFileName($child) -ne 'tasksmack') { return $child }
        }
    }
    return $null
}

# Why Lock can't take the install folder $Folder, or $null when it can. Changes nothing.
function Get-LockProblem([string]$Folder) {
    if (-not [System.IO.Directory]::Exists($Folder)) { return "$Folder doesn't exist" }
    if (Test-ReparsePoint $Folder) { return "$Folder is a link or junction" }
    $foreign = Get-ForeignEntry $Folder
    if ($foreign) { return "$Folder holds $foreign, which isn't TaskSmack's" }
    return Get-AboveProblem $Folder
}

# icacls on $Arguments, on a link or junction itself and never its target (/L), quietly; throws when it fails.
function Invoke-Icacls([string[]]$Arguments) {
    $output = & $Icacls @Arguments /L /C /Q 2>&1
    if ($LASTEXITCODE -ne 0) { throw "icacls $($Arguments -join ' ') failed ($LASTEXITCODE): $output" }
}

# Makes what is in $Folder, itself admin-only already, owned by Administrators and inherit $Folder's permissions,
# then each folder in it the same way. Nothing in $Folder can be added, removed or swapped by anyone else by now.
function Lock-Contents([string]$Folder) {
    $children = @([System.IO.Directory]::EnumerateFileSystemEntries($Folder))
    foreach ($child in $children) {
        if (Test-ReparsePoint $child) { throw "$child is a link or junction" }
    }
    if ($children.Count -eq 0) { return }
    $all = Join-Path $Folder '*'
    Invoke-Icacls @($all, '/setowner', $AdministratorsSid)
    Invoke-Icacls @($all, '/reset')
    foreach ($child in $children) {
        # Again, now that no one else can change it: a folder made a junction since the first look isn't entered.
        if (Test-ReparsePoint $child) { throw "$child is a link or junction" }
        if ([System.IO.Directory]::Exists($child)) { Lock-Contents $child }
    }
}

# Why something in $Folder (not $Folder itself) can be changed by someone who isn't an administrator, or $null.
function Get-TreeProblem([string]$Folder) {
    foreach ($child in [System.IO.Directory]::EnumerateFileSystemEntries($Folder)) {
        if (Test-ReparsePoint $child) { return "$child is a link or junction" }
        $isFolder = [System.IO.Directory]::Exists($child)
        $mask = if ($isFolder) { $ChangesItem -bor $ChangesChildren } else { $ChangesItem }
        $problem = Get-ChangeProblem $child $mask
        if ($problem) { return $problem }
        if ($isFolder) {
            $problem = Get-TreeProblem $child
            if ($problem) { return $problem }
        }
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
        'CanLock' {
            $folder = [System.IO.Path]::GetFullPath($Path)
            if ([System.IO.Directory]::Exists($folder) -and -not (Get-AdminOnlyProblem $folder)) {
                Write-Output "$folder can be changed only by administrators."
                exit 0
            }
            $problem = Get-LockProblem $folder
            if ($problem) { Write-Output "$folder can't be made admin-only: $problem."; exit 1 }
            Write-Output "$folder can be made admin-only."
            exit 0
        }
        'Lock' {
            $folder = [System.IO.Path]::GetFullPath($Path)
            if ([System.IO.Directory]::Exists($folder) -and -not (Get-AdminOnlyProblem $folder)) {
                Write-Output "$folder can be changed only by administrators."
                exit 0
            }
            $problem = Get-LockProblem $folder
            if ($problem) { Write-Output "$folder can't be made admin-only: $problem."; exit 1 }
            # The folder first: Administrators own it, so no one else keeps an owner's right to change its permissions;
            # then its own entries go and its parent's stop applying, and only these three remain.
            Invoke-Icacls @($folder, '/setowner', $AdministratorsSid)
            Invoke-Icacls @($folder, '/reset')
            Invoke-Icacls @($folder, '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F', "${AdministratorsSid}:(OI)(CI)F", '*S-1-5-32-545:(OI)(CI)RX')
            # Now no one else can change what it holds: what was checked above is checked again, for good.
            if (Test-ReparsePoint $folder) { throw "$folder is a link or junction" }
            $foreign = Get-ForeignEntry $folder
            if ($foreign) { throw "$folder holds $foreign, which isn't TaskSmack's" }
            Lock-Contents $folder
            $problem = Get-AdminOnlyProblem $folder
            if (-not $problem) { $problem = Get-TreeProblem $folder }
            if ($problem) { Write-Output "$folder couldn't be made admin-only: $problem."; exit 1 }
            Write-Output "Made $folder admin-only: only SYSTEM and Administrators can change it."
            exit 0
        }
    }
} catch {
    Write-Output "$Path is not trusted: $($_.Exception.Message)"
    exit 1
}
