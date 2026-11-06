# Tests tools/release-smoke-check.ps1's work folder (#3530): each run starts in an empty one, so no
# earlier run's screenshot, config.toml or package file can pass its checks. Only the two work-folder
# functions run, loaded from the script's AST: the smoke check itself launches TaskSmack, so it never
# runs here. Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# New-SmokeWorkDir's error for $WorkDir, or '' when it accepts it.
function Get-WorkDirRefusal([string]$WorkDir) {
    try {
        $null = New-SmokeWorkDir -WorkDir $WorkDir -TempRoot $scratch
        return ''
    } catch {
        return $_.Exception.Message
    }
}

$scriptAst = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'release-smoke-check.ps1'), [ref]$null, [ref]$null)
foreach ($name in 'New-SmokeWorkDir', 'Remove-SmokeWorkDir') {
    $functionAst = $scriptAst.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $true)
    Assert-True ($null -ne $functionAst) "release-smoke-check.ps1 has no $name function"
    . ([scriptblock]::Create($functionAst.Extent.Text))
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-smoke-check-test-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    # No -WorkDir: a new, empty folder per run, owned by the run, and never the same one twice.
    $first = New-SmokeWorkDir -WorkDir '' -TempRoot $scratch
    $second = New-SmokeWorkDir -WorkDir '' -TempRoot $scratch
    Assert-True ($first.Owned -and $second.Owned) 'A default work folder must be owned by the run'
    Assert-True ($first.Path -ne $second.Path) "Two runs got the same default work folder: $($first.Path)"
    foreach ($work in $first, $second) {
        Assert-True ((Split-Path -Parent $work.Path) -eq $scratch) "The default work folder $($work.Path) isn't under the temp root $scratch"
        Assert-True (Test-Path -LiteralPath $work.Path -PathType Container) "The default work folder $($work.Path) wasn't created"
        Assert-True (@(Get-ChildItem -LiteralPath $work.Path -Force).Count -eq 0) "The default work folder $($work.Path) isn't empty"
    }

    # The run removes its own folder with everything in it, as a finished run's would hold...
    New-Item -ItemType Directory -Path (Join-Path $first.Path 'config') | Out-Null
    Set-Content -LiteralPath (Join-Path $first.Path 'config\config.toml') -Value "[theme]`nid = 'dracula'"
    Set-Content -LiteralPath (Join-Path $first.Path 'smoke-1.png') -Value 'not a png'
    Remove-SmokeWorkDir -Work $first -Keep $false
    Assert-True (-not (Test-Path -LiteralPath $first.Path)) "The run's own work folder $($first.Path) wasn't removed"
    # ...but keeps it for -KeepWorkDir.
    Set-Content -LiteralPath (Join-Path $second.Path 'smoke-1.png') -Value 'not a png'
    $kept = Remove-SmokeWorkDir -Work $second -Keep $true 6>&1 | Out-String
    Assert-True (Test-Path -LiteralPath (Join-Path $second.Path 'smoke-1.png')) "-KeepWorkDir didn't keep $($second.Path)"
    Assert-True ($kept -match [regex]::Escape($second.Path)) "-KeepWorkDir didn't print the kept folder's path: $kept"
    # A run that failed before it had a folder has nothing to remove.
    Remove-SmokeWorkDir -Work $null -Keep $false

    # A -WorkDir that doesn't exist yet is created, and an empty one is used; neither is the run's.
    $missing = Join-Path $scratch 'given\not yet made'
    $made = New-SmokeWorkDir -WorkDir $missing -TempRoot $scratch
    Assert-True (-not $made.Owned) 'A -WorkDir the caller names must not be owned by the run'
    Assert-True ($made.Path -eq $missing) "A new -WorkDir resolved to $($made.Path), not $missing"
    Assert-True (Test-Path -LiteralPath $missing -PathType Container) "A missing -WorkDir $missing wasn't created"
    $empty = Join-Path $scratch 'given-empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    $refusal = Get-WorkDirRefusal $empty
    Assert-True ($refusal -eq '') "An empty -WorkDir was refused: $refusal"

    # The caller's folder is never removed, even without -KeepWorkDir.
    Set-Content -LiteralPath (Join-Path $missing 'smoke-1.png') -Value 'not a png'
    Remove-SmokeWorkDir -Work $made -Keep $false
    Assert-True (Test-Path -LiteralPath (Join-Path $missing 'smoke-1.png')) "The caller's -WorkDir $missing was removed"

    # A -WorkDir holding anything is refused, and left as it was: here, the last run's screenshot
    # (as the issue's second run would find it), a stale config dir, and a hidden file.
    $refusal = Get-WorkDirRefusal $missing
    Assert-True ($refusal -match "isn't empty") "A -WorkDir holding a screenshot was accepted ('$refusal')"
    Assert-True (Test-Path -LiteralPath (Join-Path $missing 'smoke-1.png')) "Refusing -WorkDir $missing changed what it holds"
    $stale = Join-Path $scratch 'given-stale'
    New-Item -ItemType Directory -Path (Join-Path $stale 'config') | Out-Null
    Assert-True ((Get-WorkDirRefusal $stale) -match "isn't empty") 'A -WorkDir holding only an empty config dir was accepted'
    $hidden = Join-Path $scratch 'given-hidden'
    New-Item -ItemType Directory -Path $hidden | Out-Null
    $hiddenFile = New-Item -ItemType File -Path (Join-Path $hidden '.leftover')
    $hiddenFile.Attributes = $hiddenFile.Attributes -bor [IO.FileAttributes]::Hidden
    Assert-True ((Get-WorkDirRefusal $hidden) -match "isn't empty") 'A -WorkDir holding only a hidden file was accepted'

    # A file is refused too.
    $file = Join-Path $scratch 'a-file'
    Set-Content -LiteralPath $file -Value ''
    Assert-True ((Get-WorkDirRefusal $file) -match 'is a file') 'A -WorkDir that is a file was accepted'

    Write-Host 'release-smoke-check work folder tests passed'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
