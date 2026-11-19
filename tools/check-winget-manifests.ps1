#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Check a release's WinGet manifests: schema, version, installer URLs and hashes (#3822).
.DESCRIPTION
    release.yml's dry run builds the manifests of the previous published release and checks them here, with no
    token and no submission, so the publish-winget job's manifest step is exercised before a real tag. Checks
    the version folder's three manifests: each one's ManifestVersion (and its schema header, when it has one) is
    -ManifestVersion, PackageIdentifier is TaskSmack.TaskSmack and PackageVersion is -Version; the installer
    manifest lists exactly the x64 and ARM64 NSIS installers at their release URLs for -Tag, and each
    InstallerSha256 equals the SHA-256 of the installer of that name in -InstallerDir. Each installer's
    ExpectedReturnCodes (its own, else the manifest's) map exit code 32, the installer's "TaskSmack is running"
    (#4436), to packageInUse. Prints every problem and exits 1; exits 0 when there is none. Nothing is
    downloaded and nothing is submitted.
.PARAMETER ManifestDir
    The version's folder (manifests\t\TaskSmack\TaskSmack\<version>), as winget-manifest.ps1 and
    `wingetcreate update --out` write it.
.PARAMETER Version
    X.Y.Z, no leading v.
.PARAMETER Tag
    The release's tag, vX.Y.Z; its release page's downloads are the installers' URLs.
.PARAMETER Repository
    owner/name of the repository that published the release.
.PARAMETER ManifestVersion
    The schema version release.yml's wingetcreate pin writes (WINGETCREATE_MANIFEST_VERSION).
.PARAMETER InstallerDir
    A folder holding the release's installers, to hash and compare with each InstallerSha256.
#>
param(
    [Parameter(Mandatory)][string]$ManifestDir,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$ManifestVersion,
    [Parameter(Mandatory)][string]$InstallerDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PackageId = 'TaskSmack.TaskSmack'

# A small YAML reader for the subset WinGet manifests use (#4437): block mappings and sequences at any
# indentation (wingetcreate writes "Installers:" with its "- Architecture:" items at column 0, our templates
# indent them), plain, single- and double-quoted scalars, [a, b] and {} flow values, | and > block scalars, plain
# multi-line scalars, and comments. Anchors, tags, multi-line quoted or flow values and multiple documents are not
# supported and throw, so a manifest outside the subset is reported, never misread. Mappings come back as
# case-sensitive Dictionary[string, object], sequences as List[object], every scalar as a string.
function Remove-YamlComment([string]$Line) {
    $quote = [char]0
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $c = $Line[$i]
        if ($quote -ne [char]0) {
            if ($quote -eq [char]'"' -and $c -eq [char]'\') { $i++ } elseif ($c -eq $quote) { $quote = [char]0 }
        } elseif (($c -eq [char]"'" -or $c -eq [char]'"') -and ($i -eq 0 -or ' [{,'.Contains($Line[$i - 1]))) { $quote = $c }
        elseif ($c -eq [char]'#' -and ($i -eq 0 -or [char]::IsWhiteSpace($Line[$i - 1]))) { return $Line.Substring(0, $i) }
    }
    return $Line
}

function ConvertFrom-YamlScalar([string]$Text) {
    $t = $Text.Trim()
    if ($t.Length -ge 2 -and $t.StartsWith("'") -and $t.EndsWith("'")) { return $t.Substring(1, $t.Length - 2).Replace("''", "'") }
    if ($t.Length -ge 2 -and $t.StartsWith('"') -and $t.EndsWith('"')) {
        return [regex]::Replace($t.Substring(1, $t.Length - 2), '\\(.)', { param($m) switch ($m.Groups[1].Value) { 'n' { "`n" } 't' { "`t" } default { $m.Groups[1].Value } } })
    }
    if ($t.StartsWith("'") -or $t.StartsWith('"')) { throw "unterminated or multi-line quoted scalar: $t" }
    if ($t -match '^[&*!]') { throw "anchors, aliases and tags aren't supported: $t" }
    return $t
}

function ConvertFrom-YamlFlow([string]$Text) {
    if ($Text -match '^\{\s*\}$') { return , [System.Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal) }
    if ($Text -notmatch '^\[(.*)\]$') { throw "unsupported flow value: $Text" }
    $list = [System.Collections.Generic.List[object]]::new()
    $inner = $Matches[1].Trim()
    if ($inner) {
        foreach ($m in [regex]::Matches($inner, '\s*("(?:[^"\\]|\\.)*"|''(?:[^'']|'''')*''|[^,]*?)\s*(?:,|$)')) {
            if ($m.Length -eq 0) { continue }
            $list.Add((ConvertFrom-YamlScalar $m.Groups[1].Value))
        }
    }
    return , $list
}

# The next line that holds content (not blank, not a comment), or $null; the position stays on it.
function Get-YamlLine($State) {
    while ($State.Pos -lt $State.Lines.Count -and $State.Lines[$State.Pos].Text -eq '') { $State.Pos++ }
    if ($State.Pos -ge $State.Lines.Count) { return $null }
    return $State.Lines[$State.Pos]
}

function Get-YamlKeyPattern { return '^(?<k>"(?:[^"\\]|\\.)*"|''(?:[^'']|'''')*''|[^\s''"\[\{#&*!|>%@`-][^:]*?|-[^\s:][^:]*?):(?:\s+(?<v>.*))?$' }

function Read-YamlBlock($State, [int]$MinIndent) {
    $line = Get-YamlLine $State
    if ($null -eq $line -or $line.Indent -lt $MinIndent) { return $null }
    if ($line.Text -match '^-(\s|$)') { return , (Read-YamlSequence $State $line.Indent) }
    if ($line.Text -cmatch (Get-YamlKeyPattern)) { return , (Read-YamlMapping $State $line.Indent) }
    $State.Pos++
    return ConvertFrom-YamlScalar $line.Text
}

function Read-YamlSequence($State, [int]$Indent) {
    $list = [System.Collections.Generic.List[object]]::new()
    while ($true) {
        $line = Get-YamlLine $State
        if ($null -eq $line -or $line.Indent -ne $Indent -or $line.Text -notmatch '^-(\s+(?<rest>.*))?$') { break }
        $rest = "$($Matches['rest'])"
        if ($rest -eq '') {
            $State.Pos++
        } else {
            # The item's content sits after the dash: re-read the line as if it began there.
            $State.Lines[$State.Pos] = [pscustomobject]@{ Indent = $Indent + $line.Text.Length - $rest.Length; Text = $rest; Raw = $line.Raw; Number = $line.Number }
        }
        $list.Add((Read-YamlBlock $State ($Indent + 1)))
    }
    $next = Get-YamlLine $State
    if ($null -ne $next -and $next.Indent -gt $Indent) { throw "line $($next.Number): unexpected indentation" }
    return , $list
}

function Read-YamlMapping($State, [int]$Indent) {
    $map = [System.Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    while ($true) {
        $line = Get-YamlLine $State
        if ($null -eq $line -or $line.Indent -ne $Indent -or $line.Text -match '^-(\s|$)') { break }
        if ($line.Text -cnotmatch (Get-YamlKeyPattern)) { throw "line $($line.Number): expected 'key: value', found '$($line.Text)'" }
        $key = ConvertFrom-YamlScalar $Matches['k']
        $value = "$($Matches['v'])".Trim()
        if ($map.ContainsKey($key)) { throw "line $($line.Number): duplicate key '$key'" }
        $State.Pos++
        if ($value -eq '') {
            $next = Get-YamlLine $State
            # A sequence may sit at the key's own indent (wingetcreate's "Installers:" list) or deeper.
            if ($null -ne $next -and ($next.Indent -gt $Indent -or ($next.Indent -eq $Indent -and $next.Text -match '^-(\s|$)'))) {
                $map[$key] = Read-YamlBlock $State $next.Indent
            } else { $map[$key] = '' }
        } elseif ($value -match '^[|>][+-]?\d*$') {
            $body = [System.Collections.Generic.List[string]]::new()
            while ($State.Pos -lt $State.Lines.Count) {
                $raw = $State.Lines[$State.Pos]
                if ($raw.Raw.Trim() -ne '' -and $raw.Raw.Length - $raw.Raw.TrimStart().Length -le $Indent) { break }
                $body.Add($raw.Raw); $State.Pos++
            }
            $indents = @($body | Where-Object { $_.Trim() } | ForEach-Object { $_.Length - $_.TrimStart().Length })
            $strip = if ($indents.Count) { ($indents | Measure-Object -Minimum).Minimum } else { 0 }
            $rows = @($body | ForEach-Object { if ($_.Length -ge $strip) { $_.Substring($strip) } else { '' } })
            $map[$key] = (($rows -join $(if ($value.StartsWith('|')) { "`n" } else { ' ' })).TrimEnd())
        } elseif ($value.StartsWith('[') -or $value.StartsWith('{')) {
            $map[$key] = ConvertFrom-YamlFlow $value
        } else {
            while ($true) {
                $more = Get-YamlLine $State
                if ($null -eq $more -or $more.Indent -le $Indent -or $value.StartsWith("'") -or $value.StartsWith('"')) { break }
                $value = "$value $($more.Text)"; $State.Pos++
            }
            $map[$key] = ConvertFrom-YamlScalar $value
        }
    }
    $next = Get-YamlLine $State
    if ($null -ne $next -and $next.Indent -gt $Indent) { throw "line $($next.Number): unexpected indentation" }
    return , $map
}

# The manifest text as a mapping; throws a message naming the line when the text is outside the subset.
function ConvertFrom-ManifestYaml([string]$Text) {
    $lines = [System.Collections.Generic.List[object]]::new()
    $number = 0
    foreach ($raw in ($Text -split '\r?\n')) {
        $number++
        $raw = $raw.TrimEnd()
        if ($raw -match '^ *\t') { throw "line ${number}: tabs can't indent YAML" }
        $body = (Remove-YamlComment $raw).TrimEnd()
        $content = $body.TrimStart()
        if ($content -eq '---' -and $number -eq 1) { $content = '' }
        elseif ($content -eq '---' -or $content -eq '...') { throw "line ${number}: multiple documents aren't supported" }
        $lines.Add([pscustomobject]@{ Indent = $body.Length - $content.Length; Text = $content; Raw = $raw; Number = $number })
    }
    $state = @{ Lines = $lines; Pos = 0 }
    if ($null -eq (Get-YamlLine $state)) { return , [System.Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal) }
    $doc = Read-YamlBlock $state 0
    $extra = Get-YamlLine $state
    if ($null -ne $extra) { throw "line $($extra.Number): unexpected content" }
    if ($doc -isnot [System.Collections.Generic.Dictionary[string, object]]) { throw 'the document is not a mapping' }
    return , $doc
}

# The top-level key's value as a string, one item, or nothing when the key is absent (so callers can count).
function Get-YamlScalars($Document, [string]$Key) {
    $value = $null
    if ($Document.TryGetValue($Key, [ref]$value)) { return "$value" }
}

# Whether an ExpectedReturnCodes value is a list with an item mapping the installer's exit code for a running
# TaskSmack (#4436: 32, TASKSMACK_IN_USE_EXIT_CODE in cmake/CPackProjectConfig.cmake.in) to WinGet's packageInUse.
function Test-InUseReturnCode($Codes) {
    $InUseExitCode = '32'
    $InUseResponse = 'packageInUse'
    if ($Codes -isnot [System.Collections.Generic.List[object]]) { return $false }
    foreach ($item in $Codes) {
        if ($item -isnot [System.Collections.Generic.Dictionary[string, object]]) { continue }
        $code = $response = ''
        [void]$item.TryGetValue('InstallerReturnCode', [ref]$code)
        [void]$item.TryGetValue('ReturnResponse', [ref]$response)
        if ("$code" -ceq $InUseExitCode -and "$response" -ceq $InUseResponse) { return $true }
    }
    return $false
}

# The problems with the manifests in $Directory, each a line of text; empty when there are none.
function Test-WingetManifests([string]$Directory, [string]$Version, [string]$Tag, [string]$Repository,
    [string]$ManifestVersion, [string]$InstallerDir) {
    $problems = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        $problems.Add("$Directory isn't a folder")
        return $problems.ToArray()
    }
    if ((Split-Path -Leaf $Directory) -cne $Version) { $problems.Add("the folder is named $(Split-Path -Leaf $Directory), not $Version") }
    $files = @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.yaml')
    $installerFile = @($files | Where-Object { $_.Name -ceq "$PackageId.installer.yaml" })
    $versionFile = @($files | Where-Object { $_.Name -ceq "$PackageId.yaml" })
    $localeFile = @($files | Where-Object { $_.Name -cmatch "^$([regex]::Escape($PackageId))\.locale\.[A-Za-z-]+\.yaml$" })
    if ($installerFile.Count -ne 1 -or $versionFile.Count -ne 1 -or $localeFile.Count -lt 1) {
        $problems.Add("expected $PackageId.yaml, $PackageId.installer.yaml and a $PackageId.locale.<tag>.yaml, found: $(($files | ForEach-Object Name) -join ', ')")
    }
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        $name = $file.Name
        try { $doc = ConvertFrom-ManifestYaml $text } catch { $problems.Add("$name can't be read as YAML: $($_.Exception.Message)"); continue }
        $found = @(Get-YamlScalars $doc 'ManifestVersion')
        if ($found.Count -ne 1 -or $found[0] -cne $ManifestVersion) { $problems.Add("$name is on ManifestVersion [$($found -join ', ')], not $ManifestVersion") }
        $schemas = @([regex]::Matches($text, '(?m)^# yaml-language-server: \$schema=https://aka\.ms/winget-manifest\.[A-Za-z]+\.(\S+)\.schema\.json\s*$') |
                ForEach-Object { $_.Groups[1].Value })
        if ($schemas.Count -gt 0 -and ($schemas.Count -ne 1 -or $schemas[0] -cne $ManifestVersion)) { $problems.Add("$name's schema header is [$($schemas -join ', ')], not $ManifestVersion") }
        $ids = @(Get-YamlScalars $doc 'PackageIdentifier')
        if ($ids.Count -ne 1 -or $ids[0] -cne $PackageId) { $problems.Add("$name's PackageIdentifier is [$($ids -join ', ')], not $PackageId") }
        $versions = @(Get-YamlScalars $doc 'PackageVersion')
        if ($versions.Count -ne 1 -or $versions[0] -cne $Version) { $problems.Add("$name's PackageVersion is [$($versions -join ', ')], not $Version") }
    }
    if ($installerFile.Count -eq 1) {
        $entries = @()
        $rootCodes = $null
        try {
            $doc = ConvertFrom-ManifestYaml (Get-Content -LiteralPath $installerFile[0].FullName -Raw)
            $list = $null
            if ($doc.TryGetValue('Installers', [ref]$list) -and $list -is [System.Collections.Generic.List[object]]) { $entries = @($list) }
            [void]$doc.TryGetValue('ExpectedReturnCodes', [ref]$rootCodes)
        } catch { $null = $_ }   # The loop above already reported the unreadable manifest.
        $expected = [ordered]@{ x64 = "TaskSmack-$Version-win64.exe"; arm64 = "TaskSmack-$Version-windows-arm64.exe" }
        $seen = @{}
        foreach ($entry in $entries) {
            if ($entry -isnot [System.Collections.Generic.Dictionary[string, object]]) { $problems.Add("an Installers item isn't a mapping"); continue }
            $arch = $url = $hash = ''
            [void]$entry.TryGetValue('Architecture', [ref]$arch)
            [void]$entry.TryGetValue('InstallerUrl', [ref]$url)
            [void]$entry.TryGetValue('InstallerSha256', [ref]$hash)
            if (-not $expected.Contains($arch)) { $problems.Add("an installer entry has Architecture '$arch', not x64 or arm64"); continue }
            if ($seen.ContainsKey($arch)) { $problems.Add("the $arch installer is listed twice"); continue }
            $seen[$arch] = $true
            $installer = $expected[$arch]
            # #4436: the in-use exit code, from the installer's own ExpectedReturnCodes, which replace the root's.
            $codes = $rootCodes
            $own = $null
            if ($entry.TryGetValue('ExpectedReturnCodes', [ref]$own)) { $codes = $own }
            if (-not (Test-InUseReturnCode $codes)) {
                $problems.Add("the $arch installer's ExpectedReturnCodes don't map InstallerReturnCode 32 to ReturnResponse packageInUse (#4436)")
            }
            $wantUrl = "https://github.com/$Repository/releases/download/$Tag/$installer"
            if ($url -cne $wantUrl) { $problems.Add("the $arch InstallerUrl is [$url], not $wantUrl") }
            $path = Join-Path $InstallerDir $installer
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $problems.Add("$installer isn't in $InstallerDir to hash"); continue }
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($hash -notmatch '^[0-9A-Fa-f]{64}$') { $problems.Add("the $arch InstallerSha256 is [$hash], not 64 hex digits") }
            elseif ($hash -ine $actual) { $problems.Add("the $arch InstallerSha256 is $hash, but $installer hashes to $actual") }
        }
        foreach ($arch in $expected.Keys) { if (-not $seen.ContainsKey($arch)) { $problems.Add("the installer manifest lists no $arch installer") } }
    }
    return $problems.ToArray()
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { Write-Error "-Version must be X.Y.Z, not '$Version'."; exit 2 }
    if ($Tag -cne "v$Version") { Write-Error "-Tag must be v$Version, not '$Tag'."; exit 2 }
    $problems = @(Test-WingetManifests $ManifestDir $Version $Tag $Repository $ManifestVersion $InstallerDir)
    if ($problems.Count -gt 0) {
        foreach ($problem in $problems) { Write-Host "::error title=WinGet manifests::$problem" }
        Write-Error "$($problems.Count) problem(s) with the WinGet manifests in ${ManifestDir}:`n$($problems -join "`n")"
        exit 1
    }
    Write-Host "The WinGet manifests in $ManifestDir are on ManifestVersion $ManifestVersion, for $PackageId $Version, with both installers' URLs and SHA-256 matching."
}
