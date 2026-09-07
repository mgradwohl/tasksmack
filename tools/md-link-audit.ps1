param(
    [Parameter(Mandatory = $false)]
    [string]$RepoRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

function Get-GitHubSlug([string]$text) {
    $t = $text.Trim().ToLowerInvariant().Replace('`', '')
    $t = [regex]::Replace($t, '[^a-z0-9\s-]', '')
    $t = [regex]::Replace($t, '[\s-]+', '-')
    $t = $t.Trim('-')
    return $t
}

function Get-MarkdownAnchors([string]$path) {
    $anchors = New-Object 'System.Collections.Generic.HashSet[string]'
    $counts = @{}

    $lines = Get-Content -LiteralPath $path -ErrorAction Stop
    foreach ($line in $lines) {
        $m = [regex]::Match($line, '^(#{1,6})\s+(.*?)(\s+#*\s*)?$')
        if (-not $m.Success) {
            continue
        }

        $title = $m.Groups[2].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($title)) {
            continue
        }

        $base = Get-GitHubSlug $title
        if ([string]::IsNullOrWhiteSpace($base)) {
            continue
        }

        $n = 0
        if ($counts.ContainsKey($base)) {
            $n = [int]$counts[$base]
        }

        $counts[$base] = $n + 1
        $slug = if ($n -eq 0) { $base } else { "$base-$n" }
        [void]$anchors.Add($slug)
    }

    return $anchors
}

function Add-Broken(
    [System.Collections.Generic.List[object]]$broken,
    [string]$kind,
    [string]$file,
    [int]$line,
    [string]$raw,
    [string]$detail
) {
    $broken.Add(
        [pscustomobject]@{
            Kind = $kind
            File = $file
            Line = $line
            Raw = $raw
            Detail = $detail
        }
    ) | Out-Null
}

$repoRootFull = (Resolve-Path -LiteralPath $RepoRoot).Path
$repoRootNorm = ($repoRootFull.TrimEnd('\') + '\')

$mdFiles = @()
$mdFiles += Get-ChildItem -LiteralPath $repoRootFull -File -Filter '*.md'
$ghDir = Join-Path $repoRootFull '.github'
if (Test-Path -LiteralPath $ghDir) {
    $mdFiles += Get-ChildItem -LiteralPath $ghDir -Recurse -File -Filter '*.md'
}
$docsDir = Join-Path $repoRootFull 'docs'
$docsDirNorm = ($docsDir.TrimEnd('\') + '\')
if (Test-Path -LiteralPath $docsDir) {
    $mdFiles += Get-ChildItem -LiteralPath $docsDir -Recurse -File -Filter '*.md'
}

$anchorCache = @{}
function Get-AnchorsCached([string]$path) {
    if (-not $anchorCache.ContainsKey($path)) {
        $anchorCache[$path] = Get-MarkdownAnchors $path
    }
    return $anchorCache[$path]
}

$broken = New-Object 'System.Collections.Generic.List[object]'
$externalPrefixes = @('http://', 'https://', 'mailto:', 'data:')

foreach ($md in $mdFiles) {
    $lines = Get-Content -LiteralPath $md.FullName

    # Reference definitions: [ref]: target
    $refDefs = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $m = [regex]::Match($lines[$i], '^\s*\[([^\]]+)\]:\s*(\S+)(?:\s+"[^"]*")?\s*$')
        if ($m.Success) {
            $key = $m.Groups[1].Value.Trim().ToLowerInvariant()
            $refDefs[$key] = $m.Groups[2].Value.Trim()
        }
    }

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $lineNo = $i + 1
        $line = $lines[$i]

        # Inline links and images: [text](target) / ![alt](target)
        foreach ($m in [regex]::Matches($line, '!?\[[^\]]*\]\(([^)]+)\)')) {
            $target = $m.Groups[1].Value.Trim()

            if ($target.StartsWith('<') -and $target.EndsWith('>')) {
                $target = $target.Substring(1, $target.Length - 2).Trim()
            }

            # Strip optional title: (path "title")
            if ($target -match '\s' -and -not $target.StartsWith('#')) {
                $target = ($target -split '\s+', 2)[0]
            }

            if ([string]::IsNullOrWhiteSpace($target)) {
                continue
            }

            $isExternal = $false
            foreach ($p in $externalPrefixes) {
                if ($target.StartsWith($p)) {
                    $isExternal = $true
                    break
                }
            }
            if ($isExternal) {
                continue
            }

            $pathPart = $target
            $anchor = ''
            $hashIndex = $target.IndexOf('#')
            if ($hashIndex -ge 0) {
                $pathPart = $target.Substring(0, $hashIndex)
                $anchor = $target.Substring($hashIndex + 1)
            }

            $pathPart = [uri]::UnescapeDataString($pathPart)
            $anchor = [uri]::UnescapeDataString($anchor)

            # Docsify hash-route (e.g. "#/guide/user-guide" or "#/guide/user-guide?id=heading"),
            # used by docs/ site pages (see docs/index.html's window.$docsify config) for
            # cross-page navigation. Resolved against docs/ as the site root, not as a
            # same-page anchor -- a real same-page anchor can never contain "/".
            if ($pathPart -eq '' -and $anchor.StartsWith('/') -and (Test-Path -LiteralPath $docsDir)) {
                $docsifyAnchor = ''
                $route = $anchor
                $idIndex = $route.IndexOf('?id=')
                if ($idIndex -ge 0) {
                    $docsifyAnchor = [uri]::UnescapeDataString($route.Substring($idIndex + 4))
                    $route = $route.Substring(0, $idIndex)
                }
                $route = $route.Trim('/')
                if ($route -eq '') { $route = 'README' }
                if (-not $route.EndsWith('.md')) { $route += '.md' }
                # GetFullPath (not Resolve-Path, which requires the path to already exist)
                # collapses ".." segments so a crafted route like "../../../../etc/passwd"
                # can be caught before ever touching the filesystem outside docs/.
                $routePath = [System.IO.Path]::GetFullPath((Join-Path $docsDir $route))
                if (-not ($routePath.ToLowerInvariant().StartsWith($docsDirNorm.ToLowerInvariant()))) {
                    Add-Broken $broken 'missing-file' $md.FullName $lineNo $target "Docsify route target escapes docs/: '$target'"
                } elseif (-not (Test-Path -LiteralPath $routePath)) {
                    Add-Broken $broken 'missing-file' $md.FullName $lineNo $target "Missing docsify route target '$routePath'"
                } elseif ($docsifyAnchor -ne '') {
                    $routeAnchors = Get-AnchorsCached (Resolve-Path -LiteralPath $routePath).Path
                    if (-not $routeAnchors.Contains($docsifyAnchor)) {
                        Add-Broken $broken 'missing-anchor' $md.FullName $lineNo $target "Missing anchor '#$docsifyAnchor' in $routePath (docsify route)"
                    }
                }
                continue
            }

            # Anchor-only link
            if ($pathPart -eq '' -and $anchor -ne '') {
                $anchorsHere = Get-AnchorsCached $md.FullName
                if (-not $anchorsHere.Contains($anchor)) {
                    Add-Broken $broken 'missing-anchor' $md.FullName $lineNo $target "Missing anchor '#$anchor' in $($md.FullName)"
                }
                continue
            }

            if ($pathPart -eq '') {
                continue
            }

            # Drop query string
            $qIndex = $pathPart.IndexOf('?')
            if ($qIndex -ge 0) {
                $pathPart = $pathPart.Substring(0, $qIndex)
            }

            $targetPath = if ($pathPart.StartsWith('/')) {
                Join-Path $repoRootFull ($pathPart.TrimStart('/'))
            } else {
                Join-Path $md.DirectoryName $pathPart
            }

            if (-not (Test-Path -LiteralPath $targetPath)) {
                Add-Broken $broken 'missing-file' $md.FullName $lineNo $target "Missing file '$targetPath'"
                continue
            }

            $fullTarget = (Resolve-Path -LiteralPath $targetPath).Path
            if (-not ($fullTarget.ToLowerInvariant().StartsWith($repoRootNorm.ToLowerInvariant()))) {
                continue
            }

            if ($anchor -ne '') {
                $anchorsThere = Get-AnchorsCached $fullTarget
                if (-not $anchorsThere.Contains($anchor)) {
                    Add-Broken $broken 'missing-anchor' $md.FullName $lineNo $target "Missing anchor '#$anchor' in $fullTarget"
                }
            }
        }

        # Reference-style uses: [text][ref] or [text][]
        foreach ($m in [regex]::Matches($line, '!?\[([^\]]+)\]\[([^\]]*)\]')) {
            $textLabel = $m.Groups[1].Value
            $ref = $m.Groups[2].Value
            if ([string]::IsNullOrWhiteSpace($ref)) {
                $ref = $textLabel
            }
            $refKey = $ref.Trim().ToLowerInvariant()

            if (-not $refDefs.ContainsKey($refKey)) {
                Add-Broken $broken 'missing-refdef' $md.FullName $lineNo "ref:$refKey" "Missing reference definition for [$refKey]"
                continue
            }

            $target = $refDefs[$refKey]

            $isExternal = $false
            foreach ($p in $externalPrefixes) {
                if ($target.StartsWith($p)) {
                    $isExternal = $true
                    break
                }
            }
            if ($isExternal) {
                continue
            }

            $pathPart = $target
            $anchor = ''
            $hashIndex = $target.IndexOf('#')
            if ($hashIndex -ge 0) {
                $pathPart = $target.Substring(0, $hashIndex)
                $anchor = $target.Substring($hashIndex + 1)
            }

            $pathPart = [uri]::UnescapeDataString($pathPart)
            $anchor = [uri]::UnescapeDataString($anchor)

            $targetPath = if ($pathPart.StartsWith('/')) {
                Join-Path $repoRootFull ($pathPart.TrimStart('/'))
            } else {
                Join-Path $md.DirectoryName $pathPart
            }

            if (-not (Test-Path -LiteralPath $targetPath)) {
                Add-Broken $broken 'missing-file' $md.FullName $lineNo $target "Missing file '$targetPath' (ref [$refKey])"
                continue
            }

            $fullTarget = (Resolve-Path -LiteralPath $targetPath).Path
            if ($anchor -ne '') {
                $anchorsThere = Get-AnchorsCached $fullTarget
                if (-not $anchorsThere.Contains($anchor)) {
                    Add-Broken $broken 'missing-anchor' $md.FullName $lineNo $target "Missing anchor '#$anchor' in $fullTarget (ref [$refKey])"
                }
            }
        }
    }
}

"Scanned $($mdFiles.Count) markdown files (root + .github + docs)."
"Broken internal link findings: $($broken.Count)"

$grouped = $broken | Group-Object Kind | Sort-Object Name
foreach ($g in $grouped) {
    ""
    "== $($g.Name) =="
    foreach ($item in ($g.Group | Sort-Object File, Line | Select-Object -First 200)) {
        $relFile = $item.File
        if ($relFile.ToLowerInvariant().StartsWith($repoRootNorm.ToLowerInvariant())) {
            $relFile = $relFile.Substring($repoRootNorm.Length).Replace('\\', '/')
        }
        "{0}:{1}: {2} -> {3}" -f $relFile, $item.Line, $item.Raw, $item.Detail
    }
}

if ($broken.Count -gt 0) {
    exit 1
}

exit 0
