# Tests for tools/build-windows-installer.ps1, the Windows installer's second pass (#2302): the .zip's files
# become the folder project.nsi installs from, only when they're exactly the files its uninstaller deletes,
# and makensis gets the uninstaller to embed. makensis is a stub script here that writes, as the
# "installer", the define it was given and the files it would have packaged; nothing is installed or run.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'build-windows-installer.ps1'
$pwsh = (Get-Process -Id $PID).Path
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-installer-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # The stub makensis: the project is its last argument, as makensis takes it.
    $stub = Join-Path $root 'makensis.ps1'
    Set-Content -LiteralPath $stub -Value @'
$project = $args[-1]
$define = @($args | Where-Object { $_ -like '/DTASKSMACK_UNINSTALLER=*' })
if ($env:STUB_MAKENSIS_EXIT) { exit [int]$env:STUB_MAKENSIS_EXIT }
$text = Get-Content -LiteralPath $project
$null = ($text | Where-Object { $_ -match '^!define INST_DIR "(.+)"$' } | Select-Object -First 1) -match '"(.+)"'
$instDir = [IO.Path]::GetFullPath($Matches[1])
$null = ($text | Where-Object { $_ -match '^OutFile "(.+)"$' } | Select-Object -First 1) -match '"(.+)"'
$files = Get-ChildItem -LiteralPath $instDir -Recurse -File | ForEach-Object { $_.FullName.Substring($instDir.Length + 1) } | Sort-Object
Set-Content -LiteralPath $Matches[1] -Value (@("define=$($define -join '|')", "args=$($args.Count)") + $files)
exit 0
'@

    $case = 0
    # The folders above each file, as relative paths ('a\b\c.txt' gives 'a' and 'a\b').
    function Get-Folders([string[]]$Paths) {
        @($Paths | ForEach-Object {
                $parts = $_ -split '\\'
                for ($i = 1; $i -lt $parts.Count; $i++) { ($parts[0..($i - 1)]) -join '\' }
            } | Sort-Object -Unique)
    }
    # A first pass's project folder, a .zip and an uninstaller, as build-windows leaves them; $Files are the
    # .zip's files and $Deleted the files project.nsi's uninstaller deletes. Its uninstall section has CPack's
    # shape (from a real project.nsi): a Delete line for every entry of the tree, folders included, in sorted
    # order, then an RMDir line per folder, deepest first; $Removed overrides the folders RMDir'd.
    function New-Case {
        param([string[]]$Files, [string[]]$Deleted, [string[]]$Removed, [string]$ZipFolder = 'TaskSmack-1.0.0-win64',
            [string]$UninstallerName = 'Uninstall.exe')
        $script:case++
        $dir = Join-Path $root "case$script:case"
        $nsis = (Join-Path $dir 'build/dist/_CPack_Packages/win64/NSIS') -replace '\\', '/'
        $project = New-Item -ItemType Directory -Force -Path (Join-Path $dir 'project')
        $folders = Get-Folders $Deleted
        if (-not $PSBoundParameters.ContainsKey('Removed')) { $Removed = $folders }
        $lines = @(
            '!define VERSION "1.0.0"',
            "!define INST_DIR `"$nsis/TaskSmack-1.0.0-win64`"",
            "OutFile `"$nsis/TaskSmack-1.0.0-win64.exe`"",
            'Section "Uninstall"'
        ) + @(@($Deleted) + @($folders) | Sort-Object -Unique | ForEach-Object { "  Delete `"`$INSTDIR\$_`"" }) +
        @(@($Removed) | Sort-Object -Descending | ForEach-Object { "  RMDir `"`$INSTDIR\$_`"" }) + @(
            '  Delete "$INSTDIR\Uninstall.exe"',
            '  RMDir "$INSTDIR"',
            '  Delete "$SMPROGRAMS\$MUI_TEMP\TaskSmack.lnk"',
            '  RMDir $MUI_TEMP',
            'SectionEnd'
        )
        Set-Content -LiteralPath (Join-Path $project 'project.nsi') -Value $lines
        Set-Content -LiteralPath (Join-Path $project 'NSIS.InstallOptions.ini') -Value '[Settings]'
        $staging = Join-Path $dir "zip/$ZipFolder"
        foreach ($file in $Files) {
            $path = Join-Path $staging $file
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
            Set-Content -LiteralPath $path -Value "contents of $file"
        }
        $zip = Join-Path $dir 'TaskSmack-1.0.0-win64.zip'
        Compress-Archive -Path (Join-Path $dir 'zip/*') -DestinationPath $zip
        $uninstaller = Join-Path $dir $UninstallerName
        Set-Content -LiteralPath $uninstaller -Value 'a signed uninstaller'
        [pscustomobject]@{ Dir = $dir; Project = $project.FullName; Zip = $zip; Uninstaller = $uninstaller; Nsis = $nsis }
    }

    function Invoke-Build {
        param($Case, [int]$MakensisExit = 0)
        $env:STUB_MAKENSIS_EXIT = if ($MakensisExit) { "$MakensisExit" } else { '' }
        $output = & $pwsh -NoProfile -File $script -ProjectDir $Case.Project -Zip $Case.Zip -Uninstaller $Case.Uninstaller `
            -OutputDir (Join-Path $Case.Dir 'out') -Makensis $stub 2>&1 | Out-String
        [pscustomobject]@{ Code = $LASTEXITCODE; Output = $output }
    }

    $files = @('TaskSmack.exe', 'TaskSmackHelper.exe', 'LICENSE.txt', 'share\tasksmack\assets\themes\a.toml',
        'share\tasksmack\assets\fonts\b.ttf', 'third_party_licenses\SDL3\LICENSE.txt', 'third_party_licenses\THIRD_PARTY_NOTICES.txt')

    # The .zip's files, in the folder project.nsi names, packaged with the given uninstaller.
    $c = New-Case -Files $files -Deleted $files
    $r = Invoke-Build $c
    Assert-True ($r.Code -eq 0) "A matching .zip: exit $($r.Code)`n$($r.Output)"
    $installer = Join-Path $c.Dir 'out/TaskSmack-1.0.0-win64.exe'
    Assert-True (Test-Path -LiteralPath $installer) "No installer in the output folder:`n$($r.Output)"
    $made = @(Get-Content -LiteralPath $installer)
    Assert-True ($made[0] -ceq "define=/DTASKSMACK_UNINSTALLER=$((Resolve-Path $c.Uninstaller).Path)") "makensis got [$($made[0])]"
    Assert-True ($made[1] -ceq 'args=3') "makensis got $($made[1]) arguments, expected /V2, the define and the project"
    $packaged = @($made | Select-Object -Skip 2)
    Assert-True ((($packaged | Sort-Object) -join '|') -ceq (($files | Sort-Object) -join '|')) "Packaged [$($packaged -join ', ')]"
    Assert-True (Test-Path -LiteralPath (Join-Path $c.Nsis 'project.nsi')) 'project.nsi not copied beside INST_DIR'
    Assert-True (Test-Path -LiteralPath (Join-Path $c.Nsis 'NSIS.InstallOptions.ini')) 'NSIS.InstallOptions.ini not copied beside INST_DIR'
    Write-Host '  A .zip matching the uninstaller: built'

    # A file the uninstaller wouldn't delete, or one it deletes that isn't installed.
    $r = Invoke-Build (New-Case -Files ($files + 'extra.txt') -Deleted $files)
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'Only in the .zip: extra\.txt; only deleted: none') "An extra file: exit $($r.Code)`n$($r.Output)"
    Write-Host '  A file the uninstaller would leave behind: refused'
    $r = Invoke-Build (New-Case -Files $files -Deleted ($files + 'gone.txt'))
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'Only in the .zip: none; only deleted: gone\.txt') "A missing file: exit $($r.Code)`n$($r.Output)"
    Write-Host '  A file the uninstaller deletes that the .zip lacks: refused'

    # The folders: each one the uninstaller removes must be one of the .zip's, and each of the .zip's removed.
    $r = Invoke-Build (New-Case -Files $files -Deleted $files -Removed ((Get-Folders $files) + 'elsewhere'))
    Assert-True ($r.Code -eq 1 -and $r.Output -match "folders aren't the ones .* Only in the .zip: none; only removed: elsewhere") "A stray RMDir: exit $($r.Code)`n$($r.Output)"
    Write-Host '  An RMDir of a folder the .zip lacks: refused'
    $r = Invoke-Build (New-Case -Files $files -Deleted $files -Removed @(Get-Folders $files | Where-Object { $_ -ne 'third_party_licenses\SDL3' }))
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'Only in the .zip: third_party_licenses\\SDL3; only removed: none') "A folder left behind: exit $($r.Code)`n$($r.Output)"
    Write-Host '  A folder the uninstaller would leave behind: refused'

    # The .zip of another architecture or version.
    $r = Invoke-Build (New-Case -Files $files -Deleted $files -ZipFolder 'TaskSmack-1.0.0-windows-arm64')
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'top-level folder is TaskSmack-1\.0\.0-windows-arm64') "Another package: exit $($r.Code)`n$($r.Output)"
    Write-Host "  Another package's .zip: refused"

    # A folder left from an earlier run isn't packaged.
    $c = New-Case -Files $files -Deleted $files
    New-Item -ItemType Directory -Force -Path (Join-Path $c.Nsis 'TaskSmack-1.0.0-win64') | Out-Null
    $r = Invoke-Build $c
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'already exists') "An existing INST_DIR: exit $($r.Code)`n$($r.Output)"
    Write-Host '  An INST_DIR that already exists: refused'

    # makensis failing fails the build.
    $r = Invoke-Build (New-Case -Files $files -Deleted $files) -MakensisExit 3
    Assert-True ($r.Code -eq 1 -and $r.Output -match 'makensis exited with 3') "A failing makensis: exit $($r.Code)`n$($r.Output)"
    Write-Host '  makensis failing: refused'

    # A path that can't go into the copy command's quotes.
    $r = Invoke-Build (New-Case -Files $files -Deleted $files -UninstallerName 'Un%install.exe')
    Assert-True ($r.Code -eq 1 -and $r.Output -match "can't hold a quote or a percent sign") "A % in the path: exit $($r.Code)`n$($r.Output)"
    Write-Host "  An uninstaller path with a percent sign: refused"

    Write-Host 'build-windows-installer.ps1 tests passed'
}
finally {
    Remove-Item Env:STUB_MAKENSIS_EXIT -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
