<#
.SYNOPSIS
    Record the toolchain inputs that decide a Windows TaskSmack.exe's bytes (#2802).
.DESCRIPTION
    Called by the windows-build-inputs action after Package, in release.yml's build-windows and
    verify-reproducibility-windows jobs. Writes <OutputDir>\<package basename>-build-inputs.json
    (TaskSmack-<version>-win64-build-inputs.json, TaskSmack-<version>-windows-arm64-build-inputs.json)
    and appends a summary to GITHUB_STEP_SUMMARY when it is set.

    The build is deterministic, but two runs of the same commit can still link different bytes
    when the runner image's serviced MSVC CRT or Windows SDK payload changes under an unchanged
    directory name (VS 18.10 vs 18.11 shipped both as VC\Tools\MSVC\14.51.36231 and SDK
    10.0.26100.0, #2802). The manifest records what explains such a difference:

      inputs.runner_image      ImageOS / ImageVersion of the GitHub runner image
      inputs.visual_studio     the installation clang's MSVC toolset came from (vswhere)
      inputs.msvc_toolset      the toolset version and the lib\<arch> directory the link searched
      inputs.windows_sdk       the SDK version and its ucrt\<arch> lib directory
      inputs.lib_search_paths  the -libpath: entries clang passes to lld-link (only its own when LIB is set)
      inputs.libraries         SHA-256 of the CRT, STL and UCRT libraries in those directories: the
                               static set TaskSmack links (/MT: libcmt, libvcruntime, libcpmt,
                               libucrt), the import libraries a /MD build would, and oldnames
      inputs.llvm, inputs.tools, inputs.commit, inputs.source_date_epoch

    Everything a comparison should ignore (job, run id, package name) is under `provenance`;
    compare-build-inputs.ps1 diffs two manifests' `inputs` alone.

    Discovery asks the compiler first: `clang++ -###` prints the lld-link line with the MSVC and
    SDK -libpath: entries clang resolved, which is exactly what the build's link consumed. When LIB
    is set (a developer shell) clang omits them, so the script then resolves the same way clang
    does: vswhere's installation, VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt, and
    the newest SDK under the Installed Roots registry key.
.PARAMETER PackageGlob
    The packaged .zip (one match), whose base name names the manifest.
.PARAMETER OutputDir
    Where the manifest is written; created when missing.
.PARAMETER Job
    The workflow job writing the manifest, recorded under provenance.
.PARAMETER Compiler
    The C++ compiler to ask for its link line; the build's clang++ on PATH by default.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageGlob,
    [Parameter(Mandatory)] [string] $OutputDir,
    [Parameter(Mandatory)] [string] $Job,
    [string] $Compiler = 'clang++'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-EnvOrNull([string] $name) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrEmpty($value)) { return $null }
    return $value
}

function Get-FirstLine($output) {
    $text = @($output | ForEach-Object { "$_" }) -join "`n"
    return ($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
}

function Get-ToolVersion([string] $tool, [string[]] $arguments) {
    $command = Get-Command $tool -ErrorAction SilentlyContinue
    if (-not $command) { return $null }
    try {
        return Get-FirstLine (& $command.Source @arguments 2>&1)
    } catch {
        return $null
    }
}

# The lld-link -libpath: entries from the compiler's dry-run link line. clang quotes each argument
# and escapes backslashes and quotes inside it, so the captured value is unescaped.
function Get-ClangLibSearchPaths([string] $compiler) {
    $command = Get-Command $compiler -ErrorAction SilentlyContinue
    if (-not $command) { return @() }
    $probe = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-build-inputs-probe-$PID.exe"
    try {
        $lines = & $command.Source '-###' -x c++ -fuse-ld=lld NUL -o $probe 2>&1 | ForEach-Object { "$_" }
    } catch {
        return @()
    }
    $paths = @()
    foreach ($line in $lines) {
        if ($line -notmatch 'lld-link') { continue }
        foreach ($match in [regex]::Matches($line, '"-libpath:((?:[^"\\]|\\.)*)"')) {
            $paths += ($match.Groups[1].Value -replace '\\(.)', '$1')
        }
    }
    return $paths
}

# vswhere's instances, newest first, or an empty array where there is no Visual Studio installer.
function Get-VisualStudioInstances {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return @() }
    $json = & $vswhere -all -prerelease -products * -format json -utf8 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $json) { return @() }
    $instances = @($json | ConvertFrom-Json)
    return @($instances | Sort-Object { [version] $_.installationVersion } -Descending)
}

function Get-InstalledSdkRoot {
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots') {
        $root = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).KitsRoot10
        if ($root) { return $root.TrimEnd('\') }
    }
    return $null
}

$package = @(Get-ChildItem -Path $PackageGlob -File -ErrorAction SilentlyContinue)
if ($package.Count -ne 1) {
    Write-Host "::error::Expected exactly one package matching '$PackageGlob', found $($package.Count)."
    exit 1
}
$package = $package[0]

$arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
    'Arm64' { 'arm64' }
    'X64' { 'x64' }
    default { "$_".ToLowerInvariant() }
}

$searchPaths = @(Get-ClangLibSearchPaths $Compiler)
$msvcLibDir = $searchPaths | Where-Object { $_ -match '\\VC\\Tools\\MSVC\\[^\\]+\\lib\\' } | Select-Object -First 1
$ucrtLibDir = $searchPaths | Where-Object { $_ -match '\\Lib\\[^\\]+\\ucrt\\' } | Select-Object -First 1
$umLibDir = $searchPaths | Where-Object { $_ -match '\\Lib\\[^\\]+\\um\\' } | Select-Object -First 1
$discovery = "$Compiler -### (lld-link -libpath: entries)"

$instances = @(Get-VisualStudioInstances)
$vs = $null
if ($msvcLibDir) {
    $vs = $instances | Where-Object { $msvcLibDir.StartsWith($_.installationPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
}
if (-not $vs -and $instances.Count -gt 0) { $vs = $instances[0] }

if (-not $msvcLibDir -and $vs) {
    # LIB was set or clang printed no MSVC path: resolve the toolset the way clang's setup-config
    # path does, from the installation's default toolset version.
    $discovery = 'vswhere + Microsoft.VCToolsVersion.default.txt + Installed Roots registry key'
    $defaultVersionFile = Join-Path $vs.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (Test-Path -LiteralPath $defaultVersionFile -PathType Leaf) {
        $toolsetVersion = (Get-Content -LiteralPath $defaultVersionFile -Raw).Trim()
        $candidate = Join-Path $vs.installationPath "VC\Tools\MSVC\$toolsetVersion\lib\$arch"
        if (Test-Path -LiteralPath $candidate -PathType Container) { $msvcLibDir = $candidate }
    }
}
if (-not $ucrtLibDir) {
    $sdkRoot = Get-InstalledSdkRoot
    if ($sdkRoot) {
        $newest = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Lib') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName "ucrt\$arch") -PathType Container) } |
            Sort-Object { [version] $_.Name } -Descending | Select-Object -First 1
        if ($newest) {
            $ucrtLibDir = Join-Path $newest.FullName "ucrt\$arch"
            $umLibDir = Join-Path $newest.FullName "um\$arch"
        }
    }
}

if (-not $msvcLibDir) {
    Write-Host "::error::No MSVC toolset lib directory found for $arch (clang printed no -libpath: and vswhere found no usable installation)."
    exit 1
}
if (-not $ucrtLibDir) {
    Write-Host "::error::No Windows SDK ucrt lib directory found for $arch."
    exit 1
}

$toolsetVersion = if ($msvcLibDir -match '\\VC\\Tools\\MSVC\\([^\\]+)\\') { $Matches[1] } else { $null }
$sdkVersion = if ($ucrtLibDir -match '\\Lib\\([^\\]+)\\ucrt\\') { $Matches[1] } else { $null }
$defaultToolset = $null
if ($vs) {
    $file = Join-Path $vs.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (Test-Path -LiteralPath $file -PathType Leaf) { $defaultToolset = (Get-Content -LiteralPath $file -Raw).Trim() }
}

# The libraries whose bytes end up in, or decide, TaskSmack.exe. The static set is what /MT links
# (cmake/CompilerOptions.cmake); the import libraries are what a /MD build would, recorded so a
# serviced payload shows up even when it only touched them.
$libraryNames = [ordered]@{
    msvc = @('libcmt.lib', 'libvcruntime.lib', 'libcpmt.lib', 'libconcrt.lib', 'msvcrt.lib', 'vcruntime.lib', 'msvcprt.lib', 'oldnames.lib')
    ucrt = @('libucrt.lib', 'ucrt.lib')
}
$libraries = [ordered]@{}
$missing = @()
foreach ($set in $libraryNames.Keys) {
    $dir = if ($set -eq 'msvc') { $msvcLibDir } else { $ucrtLibDir }
    foreach ($name in $libraryNames[$set]) {
        $path = Join-Path $dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $missing += $path
            continue
        }
        $item = Get-Item -LiteralPath $path
        $libraries[$name] = [ordered]@{
            path   = $item.FullName
            size   = $item.Length
            sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

$visualStudio = $null
if ($vs) {
    $catalog = $vs.PSObject.Properties['catalog']
    $visualStudio = [ordered]@{
        installation_path       = $vs.installationPath
        installation_version    = $vs.installationVersion
        product_display_version = if ($catalog -and $catalog.Value.PSObject.Properties['productDisplayVersion']) { $catalog.Value.productDisplayVersion } else { $null }
        product_id              = if ($vs.PSObject.Properties['productId']) { $vs.productId } else { $null }
        display_name            = if ($vs.PSObject.Properties['displayName']) { $vs.displayName } else { $null }
    }
}

$manifest = [ordered]@{
    schema     = 1
    about      = 'The toolchain inputs that decide this TaskSmack.exe (#2802). Two manifests with equal `inputs` built from the same commit should give the same bytes; compare them with .github/actions/windows-build-inputs/compare-build-inputs.ps1.'
    provenance = [ordered]@{
        package     = $package.Name
        job         = $Job
        workflow    = Get-EnvOrNull 'GITHUB_WORKFLOW'
        run_id      = Get-EnvOrNull 'GITHUB_RUN_ID'
        run_attempt = Get-EnvOrNull 'GITHUB_RUN_ATTEMPT'
        ref         = Get-EnvOrNull 'GITHUB_REF_NAME'
        discovery   = $discovery
        env_lib     = Get-EnvOrNull 'LIB'
    }
    inputs     = [ordered]@{
        arch              = $arch
        commit            = Get-EnvOrNull 'GITHUB_SHA'
        source_date_epoch = Get-EnvOrNull 'SOURCE_DATE_EPOCH'
        runner_image      = [ordered]@{
            image_os      = Get-EnvOrNull 'ImageOS'
            image_version = Get-EnvOrNull 'ImageVersion'
            runner_os     = Get-EnvOrNull 'RUNNER_OS'
            runner_arch   = Get-EnvOrNull 'RUNNER_ARCH'
            windows       = [Environment]::OSVersion.Version.ToString()
        }
        visual_studio     = $visualStudio
        msvc_toolset      = [ordered]@{
            version         = $toolsetVersion
            default_version = $defaultToolset
            lib_dir         = $msvcLibDir
        }
        windows_sdk       = [ordered]@{
            version      = $sdkVersion
            ucrt_lib_dir = $ucrtLibDir
            um_lib_dir   = $umLibDir
        }
        lib_search_paths  = @($searchPaths)
        libraries         = $libraries
        libraries_missing = @($missing)
        llvm              = [ordered]@{
            clang    = Get-ToolVersion $Compiler @('--version')
            lld_link = Get-ToolVersion 'lld-link' @('--version')
        }
        tools             = [ordered]@{
            cmake = Get-ToolVersion 'cmake' @('--version')
            ninja = Get-ToolVersion 'ninja' @('--version')
        }
    }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$manifestPath = Join-Path (Resolve-Path -LiteralPath $OutputDir).Path "$($package.BaseName)-build-inputs.json"
$json = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($manifestPath, $json + "`n")
Write-Host "Wrote $manifestPath"
Write-Host $json

$vsLine = if ($visualStudio) { "$($visualStudio.installation_version) ($($visualStudio.display_name))" } else { 'not found' }
$image = $manifest.inputs.runner_image
$imageLine = if ($image.image_os -or $image.image_version) { "$($image.image_os) $($image.image_version)".Trim() } else { 'not a GitHub-hosted runner (no ImageOS/ImageVersion)' }
$summary = @(
    "### Windows build inputs ($arch, $Job)",
    '',
    "- Runner image: ``$imageLine``",
    "- Visual Studio: ``$vsLine``",
    "- MSVC toolset: ``$toolsetVersion`` (lib: ``$msvcLibDir``)",
    "- Windows SDK: ``$sdkVersion`` (ucrt lib: ``$ucrtLibDir``)",
    "- clang: ``$($manifest.inputs.llvm.clang)``",
    '',
    '| Library | SHA-256 | Size |',
    '|---|---|---|'
)
foreach ($name in $libraries.Keys) {
    $summary += "| ``$name`` | ``$($libraries[$name].sha256)`` | $($libraries[$name].size) |"
}
if ($missing.Count -gt 0) {
    $missingList = ($missing | ForEach-Object { '`' + $_ + '`' }) -join ', '
    $summary += ''
    $summary += "Not present: $missingList"
}
$summary += ''
$summary += '<details><summary>' + [IO.Path]::GetFileName($manifestPath) + '</summary>'
$summary += ''
$summary += '```json'
$summary += $json
$summary += '```'
$summary += '</details>'
$summary += ''
if ($env:GITHUB_STEP_SUMMARY) {
    $summary -join "`n" | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
}
