<#
.SYNOPSIS
    The NSIS installer's checks on a previously installed TaskSmack folder (#4444, #4445).
.DESCRIPTION
    cmake/CPackProjectConfig.cmake.in embeds this script in the installer and runs it (Windows PowerShell,
    through nsExec) from the upgrade step, which an elevated installer, or a SYSTEM WinGet or Intune
    upgrade, runs silently.

    -Mode Uninstaller: exit 0 when running the old Uninstall.exe at -Path is safe, 1 when it isn't.
      Safe means either of
        - admin-only: the file, its folder and every folder above can be changed by no one but SYSTEM,
          Administrators and TrustedInstaller (the owners, the write rights of every allow entry), none is
          a link or junction, and the volume is a fixed NTFS one: the same test TaskSmackHelper and
          Replace Task Manager apply to an install (BrokerHelper::checkUnchangeableNamed), or
        - signed: a valid Authenticode signature whose signer certificate has the common name -Signer
          (the subject's CN, never a thumbprint: Azure Artifact Signing's certificates rotate).
    -Mode Folder: exit 0 when -Path, or the nearest folder above it that exists, is admin-only (a
      warning in the installer's log otherwise; nothing is refused).
    -Mode Remove: delete a previous install's files from -Path (the folder) by the installer's own list,
      when its Uninstall.exe isn't trusted. A link or junction is removed, never followed, and nothing else
      in the folder is touched.

    Any error counts as not trusted. The first line written says why. Unelevated and read-only except in
    Remove mode, so tests/tools/test_nsis_upgrade.py runs it on temporary folders.
.PARAMETER Mode
    Uninstaller, Folder or Remove.
.PARAMETER Path
    The Uninstall.exe, or the folder.
.PARAMETER Signer
    The signer certificate's common name.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Uninstaller', 'Folder', 'Remove')][string]$Mode,
    [Parameter(Mandatory)][string]$Path,
    [string]$Signer = 'Matt Gradwohl'
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

# Deletes $Item; a link or junction is removed itself, its target never entered.
function Remove-Tree([string]$Item) {
    if (-not (Test-Path -LiteralPath $Item)) { return }
    $info = Get-Item -LiteralPath $Item -Force
    if ($info.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        if ($info.PSIsContainer) { [System.IO.Directory]::Delete($info.FullName) } else { [System.IO.File]::Delete($info.FullName) }
        return
    }
    if ($info.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $Item -Force) { Remove-Tree $child.FullName }
        [System.IO.Directory]::Delete($info.FullName)
    } else {
        [System.IO.File]::SetAttributes($info.FullName, [System.IO.FileAttributes]::Normal)
        [System.IO.File]::Delete($info.FullName)
    }
}

try {
    switch ($Mode) {
        'Uninstaller' {
            $problem = Get-AdminOnlyProblem $Path
            if (-not $problem) { Write-Output "$Path is trusted: it and its folders can be changed only by administrators."; exit 0 }
            $sig = Get-AuthenticodeSignature -LiteralPath $Path
            $subject = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
            if ($sig.Status -eq 'Valid' -and $subject -match "(^|,\s*)CN=$([regex]::Escape($Signer))(,|$)") {
                Write-Output "$Path is trusted: validly signed by $Signer."
                exit 0
            }
            Write-Output "$Path is not trusted: $problem, and its signature is $($sig.Status) (signer '$subject', not $Signer)."
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
        'Remove' {
            $folder = [System.IO.Path]::GetFullPath($Path)
            if (-not (Test-Path -LiteralPath $folder -PathType Container)) { Write-Output "$folder doesn't exist."; exit 0 }
            foreach ($name in 'TaskSmack.exe', 'TaskSmackHelper.exe', 'LICENSE.txt', 'Uninstall.exe', 'third_party_licenses', 'share\tasksmack') {
                Remove-Tree (Join-Path $folder $name)
            }
            foreach ($empty in (Join-Path $folder 'share'), $folder) {
                if ((Test-Path -LiteralPath $empty -PathType Container) -and -not (Get-ChildItem -LiteralPath $empty -Force)) {
                    [System.IO.Directory]::Delete($empty)
                }
            }
            Write-Output "Removed TaskSmack's files from $folder."
            exit 0
        }
    }
} catch {
    Write-Output "$Path is not trusted: $($_.Exception.Message)"
    exit 1
}
