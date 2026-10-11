<#
.SYNOPSIS
    Swap the cached LLVM tree in for the runner image's own LLVM (#1406, #1761).
.DESCRIPTION
    Called by setup-windows-llvm's "Use cached LLVM install" step after an exact cache hit.

    The preferred path is replace-don't-overlay: rename the image's LLVM aside and move the cached
    tree into its place. Both are renames on C:, so they are instant and leave no stale files from
    the image's LLVM version behind.

    The rename can be refused with "Access denied" when something on the runner holds a handle
    under the image's LLVM directory (Defender scanning it, or another process). That is usually
    transient, so the rename is retried with backoff. If it is still refused, the cached tree is
    copied over the image's tree instead (robocopy), which only needs the individual files it
    overwrites to be writable, not the whole directory to be free.

    The destination can't move elsewhere (C:\llvm-<version> plus PATH/LLVM_ROOT): the Windows
    configure/build steps in reusable-build-test.yml and release.yml set LLVM_ROOT to
    C:\Program Files\LLVM at step level, which overrides anything exported via GITHUB_ENV, and
    the presets derive CMAKE_C(XX)_COMPILER from LLVM_ROOT.

    The "Verify LLVM" step that follows checks clang's version, so a bad swap fails there.
.PARAMETER Source
    The cached LLVM tree (restored by actions/cache).
.PARAMETER Destination
    Where the build expects LLVM.
.PARAMETER MaxAttempts
    Rename attempts before falling back to the copy.
.PARAMETER DelaySeconds
    Base backoff; attempt N waits N * DelaySeconds before the next try.
#>
[CmdletBinding()]
param(
    [string]$Source = 'C:\llvm-cache\LLVM',
    [string]$Destination = 'C:\Program Files\LLVM',
    [int]$MaxAttempts = 5,
    [int]$DelaySeconds = 3
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Source)) {
    Write-Host "::error title=LLVM cache empty::cache hit, but $Source does not exist"
    exit 1
}

$asideName = (Split-Path -Leaf $Destination) + '.runner-image'
$aside = Join-Path (Split-Path -Parent $Destination) $asideName

if (Test-Path -LiteralPath $Destination) {
    if (Test-Path -LiteralPath $aside) {
        Write-Host "::error title=LLVM swap failed::$aside already exists"
        exit 1
    }
    $renamed = $false
    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            Rename-Item -LiteralPath $Destination -NewName $asideName
            $renamed = $true
            break
        } catch {
            Write-Host "::warning title=Rename of image LLVM refused::attempt $attempt/$MaxAttempts`: $($_.Exception.Message)"
            if ($attempt -lt $MaxAttempts) { Start-Sleep -Seconds ($DelaySeconds * $attempt) }
        }
    }

    if (-not $renamed) {
        # Overlay fallback. Files that only the image's LLVM ships stay behind (e.g. its
        # lib\clang\<major> headers); clang 22 doesn't look at another major's resource directory.
        # /COPY:DAT keeps the cached timestamps, which ccache's compiler check depends on.
        Write-Host "::warning title=Overlaying cached LLVM::could not rename $Destination aside after $MaxAttempts attempts; copying the cached LLVM over it instead"
        robocopy $Source $Destination /E /IS /IT /COPY:DAT /DCOPY:DAT /R:5 /W:5 /NFL /NDL /NP
        # robocopy exit codes below 8 mean success.
        if ($LASTEXITCODE -ge 8) {
            Write-Host "::error title=LLVM swap failed::robocopy of $Source over $Destination exited with $LASTEXITCODE"
            exit 1
        }
        Write-Host "Copied cached LLVM over $Destination"
        exit 0
    }
    Write-Host "Renamed image LLVM aside to $aside"
}

Move-Item -LiteralPath $Source -Destination $Destination
Write-Host "Moved cached LLVM to $Destination"
exit 0
