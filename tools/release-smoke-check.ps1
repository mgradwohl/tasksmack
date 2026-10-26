<#
.SYNOPSIS
    Launch a packaged Windows TaskSmack.exe twice on Mesa llvmpipe and check that it quits cleanly.
.DESCRIPTION
    release.yml's smoke check of an extracted Windows .zip: validate-windows-package runs it before
    anything is published, and verify-published-release on the x64 .zip downloaded from the release
    (#2721).

    Runs a temporary copy of the extracted package with llvmpipe beside the exe, its own
    TASKSMACK_CONFIG_DIR, a private %TEMP% and an info-level log captured from stdout. It quits by
    itself (#2720): TASKSMACK_SCREENSHOT (#2827) writes one frame AliveSeconds after startup and
    closes the window through the same close request as the title bar's Close button, so every
    layer detaches and ShellLayer::onDetach() saves config.toml; a run that wrote no frame exits
    non-zero. Fails unless it quits with exit code 0 in the time allowed, on any [critical] or
    [error] log line, unless the log shows OpenGL 3.3+ from llvmpipe (proof the real renderer came
    up, not an error box), and unless config.toml was saved with its [theme] table.

    It runs twice (#2726): from an empty config dir, then from a copy of the config.toml v0.8.0
    saved (tests/App/fixtures/config-v0.8.0.toml), which must be saved in today's format
    (config_version) with the settings changed in v0.8.0's UI when the fixture was made kept
    (tests/App/fixtures/README.md; each differs from the default). Keep that list in step with
    tools/release-smoke-check.sh.

    Then, as a user deleting the portable .zip's folder would (#2488): no new TaskSmack-named entry
    may appear under %APPDATA% or %LOCALAPPDATA% outside the config dirs and %TEMP%, and the folder
    TaskSmack ran from must delete with no file still locked.

    Appends a summary to $env:GITHUB_STEP_SUMMARY when it is set, and exits 1 on any failure.
.PARAMETER PackageRoot
    The extracted .zip's top-level directory, which holds TaskSmack.exe. Only a copy of it is run,
    unless -InPlace.
.PARAMETER MesaDllDir
    The directory holding Mesa's llvmpipe DLLs (.github/actions/setup-mesa-llvmpipe's dll-dir).
.PARAMETER Fixture
    The config.toml v0.8.0 saved: tests/App/fixtures/config-v0.8.0.toml.
.PARAMETER Title
    The heading of the step summary.
.PARAMETER WorkDir
    The folder for the copy, the config dirs, the private %TEMP%, logs and screenshots. Give each run its
    own, so each starts from empty config dirs.
.PARAMETER InPlace
    Run TaskSmack.exe where it is, as installed by the NSIS installer (#2302), instead of a copy: the Mesa
    DLLs go beside it for the launches and are deleted again afterwards (a file they'd overwrite fails the
    check), and the folder stays, for the uninstaller to remove. The last check is then that those DLLs
    delete, rather than the whole folder.
#>
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$MesaDllDir,
    [Parameter(Mandatory)][string]$Fixture,
    [string]$Title = "Windows package smoke check ($env:RUNNER_ARCH)",
    [string]$WorkDir = (Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }) 'tasksmack-smoke'),
    [switch]$InPlace
)

# How long startup may take, and how long after startup the screenshot (and so the quit) comes.
$StartupTimeoutSeconds = 60
$AliveSeconds = 5
# How deep under %APPDATA% and %LOCALAPPDATA% the leave-no-trace listing looks: deep enough for
# Local\Microsoft\Windows\WER\ReportArchive\<report>, and a few seconds' work.
$TraceScanDepth = 5
$env:GALLIUM_DRIVER = 'llvmpipe'
$env:TASKSMACK_LOG_LEVEL = 'info'

$ErrorActionPreference = 'Stop'
$work = $WorkDir
$tempDir = Join-Path $work 'temp'
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$mesaDlls = @(Get-ChildItem -LiteralPath $MesaDllDir -Filter '*.dll' -File)
if ($InPlace) {
  $pkg = (Resolve-Path -LiteralPath $PackageRoot).Path
  $clashes = @($mesaDlls | Where-Object { Test-Path -LiteralPath (Join-Path $pkg $_.Name) })
  if ($clashes.Count -gt 0) {
    Write-Error "$pkg already holds $(($clashes | ForEach-Object Name) -join ', '), which the Mesa DLLs would overwrite."
    exit 1
  }
} else {
  $pkg = Join-Path $work 'pkg'
  New-Item -ItemType Directory -Force -Path $pkg | Out-Null
  Copy-Item -Path (Join-Path $PackageRoot '*') -Destination $pkg -Recurse
}
$mesaDlls | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pkg }
$exe = Join-Path $pkg 'TaskSmack.exe'
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
# Section, key and the value as TaskSmack writes it.
$fixtureSettings = @(
  @('theme', 'id', "'dracula'"),
  @('font', 'size', "'large'"),
  @('sampling', 'interval_ms', '2000'),
  @('sampling', 'history_max_seconds', '600'),
  @('ui', 'show_privilege_notice', 'false'),
  @('process_columns', 'threads', 'true'),
  @('process_columns', 'user', 'false')
)
# One config dir per launch: an empty one, then one holding v0.8.0's config.toml.
$launches = @(
  @{ Name = 'fresh config'; ConfigDir = (Join-Path $work 'config'); Seed = $null },
  @{ Name = "v0.8.0's config"; ConfigDir = (Join-Path $work 'config-v080'); Seed = $Fixture }
)

# Every TaskSmack-named file or folder under %APPDATA% and %LOCALAPPDATA%, case-insensitively,
# listed before the launches and again after them. A new one is a trace the .zip leaves behind,
# unless it is under a config dir or a %TEMP% (the private one below, or the runner's own,
# which is under %LOCALAPPDATA% and may be named by its 8.3 short path, so both spellings).
$allowedRoots = @($launches.ConfigDir) + @($tempDir, [System.IO.Path]::GetTempPath(), (Join-Path $env:LOCALAPPDATA 'Temp')) |
  ForEach-Object { [System.IO.Path]::GetFullPath($_).TrimEnd('\') + '\' }
function Get-TaskSmackEntries {
  foreach ($root in @($env:APPDATA, $env:LOCALAPPDATA)) {
    Get-ChildItem -LiteralPath $root -Recurse -Depth $TraceScanDepth -Force -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match 'tasksmack' } | ForEach-Object { $_.FullName }
  }
}
# The value of <key> in [<section>] of a config.toml, as written.
function Get-TomlValue([string[]]$Lines, [string]$Section, [string]$Key) {
  $current = ''
  foreach ($line in $Lines) {
    if ($line -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
    if ($current -eq $Section -and $line -match '^(\S+) = (.*)$' -and $Matches[1] -eq $Key) { return $Matches[2] }
  }
  return $null
}
$entriesBefore = @(Get-TaskSmackEntries)

# A private %TEMP% for tasksmack-debug.log.
$env:TMP = $tempDir
$env:TEMP = $tempDir
# The wait covers startup, the AliveSeconds before the capture and the shutdown's save.
$quitSeconds = $StartupTimeoutSeconds + $AliveSeconds + 30
$failure = $null
$renderer = ''
$version = ''
$configSaved = $false
$index = 0
foreach ($launch in $launches) {
  $index++
  $run = "TaskSmack.exe ($($launch.Name))"
  $configDir = $launch.ConfigDir
  New-Item -ItemType Directory -Force -Path $configDir | Out-Null
  $configFile = Join-Path $configDir 'config.toml'
  if ($launch.Seed) { Copy-Item -LiteralPath $launch.Seed -Destination $configFile }
  $shot = Join-Path $work "smoke-$index.png"
  $stdoutLog = Join-Path $work "stdout-$index.log"
  $stderrLog = Join-Path $work "stderr-$index.log"
  $env:TASKSMACK_CONFIG_DIR = $configDir
  $env:TASKSMACK_SCREENSHOT = "$shot;after=$AliveSeconds"

  $proc = Start-Process -FilePath $exe -WorkingDirectory $pkg -PassThru `
    -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
  $null = $proc.Handle  # cache the handle, or ExitCode is empty once the process has gone
  if ($proc.WaitForExit($quitSeconds * 1000)) {
    if ($proc.ExitCode -ne 0) {
      $failure = "$run exited with code $($proc.ExitCode); a clean quit after its screenshot exits 0"
    }
  } else {
    $failure = "$run didn't quit by itself within $quitSeconds s of starting"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    $proc.WaitForExit(10000) | Out-Null
  }

  $lines = @(Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue) +
    @(Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue)
  Write-Host "=== $run log ==="
  $lines | ForEach-Object { Write-Host $_ }
  Write-Host "=== end of log ==="

  $errorLines = @($lines | Where-Object { $_ -match '\[(critical|error)\]' })
  $rendererLine = $lines | Where-Object { $_ -match '\]\s+Renderer:\s*(.+)$' } | Select-Object -First 1
  $runRenderer = if ($rendererLine -match '\]\s+Renderer:\s*(.+)$') { $Matches[1].Trim() } else { '' }
  $versionLine = $lines | Where-Object { $_ -match '\]\s+Version:\s*(.+)$' } | Select-Object -First 1
  $runVersion = if ($versionLine -match '\]\s+Version:\s*(.+)$') { $Matches[1].Trim() } else { '' }
  if ($index -eq 1) { $renderer = $runRenderer; $version = $runVersion }

  if (-not $failure -and $errorLines.Count -gt 0) {
    $failure = "$run's log has $($errorLines.Count) [critical]/[error] line(s):`n$($errorLines -join "`n")"
  }
  if (-not $failure -and -not ($lines -match '\]\s+Entering main loop')) {
    $failure = "$run's log never reached 'Entering main loop' (Application::run): startup never finished"
  }
  if (-not $failure -and $runRenderer -notmatch 'llvmpipe') {
    $failure = "Expected the llvmpipe renderer in $run's log, found '$runRenderer'"
  }
  if (-not $failure) {
    if ($runVersion -match '^(\d+)\.(\d+)') {
      $glVersion = [version]::new([int]$Matches[1], [int]$Matches[2])
      if ($glVersion -lt [version]'3.3') { $failure = "$run got OpenGL $glVersion, below the required 3.3 ('$runVersion')" }
    } else {
      $failure = "No parseable OpenGL version in $run's log (found '$runVersion')"
    }
  }
  if (-not $failure -and -not (Test-Path -LiteralPath $shot -PathType Leaf)) {
    $failure = "$run exited 0 but wrote no screenshot at $shot"
  }

  # The settings saved on the way out: with no input nothing writes config.toml before then, so
  # its being there proves the shutdown's save ran (for v0.8.0's file, so does config_version,
  # which v0.8.0 didn't write). The [theme] table and its id are always written.
  if (-not $failure) {
    if (-not (Test-Path -LiteralPath $configFile -PathType Leaf)) {
      $failure = "No config.toml in $configDir after $run quit: the settings weren't saved on exit"
    } else {
      $config = Get-Content -LiteralPath $configFile -Raw
      Write-Host "=== $run config.toml ==="
      Write-Host $config
      Write-Host "=== end of config.toml ==="
      if ($config -notmatch '(?m)^\[theme\]\r?\nid = (''[^''\r\n]+''|"[^"\r\n]+")\r?$') {
        $failure = "config.toml in $configDir has no [theme] table with an id: $run's save wrote something else"
      } elseif ($launch.Seed) {
        $configLines = @(Get-Content -LiteralPath $configFile)
        if (-not ($configLines -match '^config_version = \d+$')) {
          $failure = "config.toml in $configDir has no config_version: $run's shutdown save didn't rewrite v0.8.0's file"
        } else {
          $lost = @(foreach ($setting in $fixtureSettings) {
            $actual = Get-TomlValue $configLines $setting[0] $setting[1]
            if ($actual -cne $setting[2]) {
              "[$($setting[0])] $($setting[1]) is $(if ($null -eq $actual) { 'missing' } else { $actual }), not v0.8.0's $($setting[2])"
            }
          })
          if ($lost.Count -gt 0) { $failure = "$run didn't keep v0.8.0's settings:`n$($lost -join "`n")" }
        }
      }
    }
  }
  if ($failure) { break }
}
$configSaved = -not $failure

# Leave no trace: no new TaskSmack-named entry outside the config dirs and %TEMP%, then the
# folder it ran from deletes, which fails on any file still locked.
$leftNoTrace = $false
if (-not $failure) {
  $traces = @(Get-TaskSmackEntries | Where-Object {
    $entry = $_
    $entriesBefore -notcontains $entry -and
      -not ($allowedRoots | Where-Object { ($entry + '\').StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) })
  })
  if ($traces.Count -gt 0) {
    $failure = "TaskSmack left $($traces.Count) new TaskSmack-named entr$(if ($traces.Count -eq 1) { 'y' } else { 'ies' }) under %APPDATA%/%LOCALAPPDATA%, outside the config dirs and %TEMP%:`n$($traces -join "`n")"
  }
}
if ($InPlace) {
  # The Mesa DLLs go even after a failure, so they never stay in an installed copy.
  try {
    $mesaDlls | ForEach-Object { Remove-Item -LiteralPath (Join-Path $pkg $_.Name) -Force -ErrorAction Stop }
    $leftNoTrace = -not $failure
  } catch {
    if (-not $failure) { $failure = "The Mesa DLLs beside TaskSmack.exe in $pkg didn't delete after it quit: $($_.Exception.Message)" }
  }
} elseif (-not $failure) {
  try {
    Remove-Item -LiteralPath $pkg -Recurse -Force -ErrorAction Stop
    $leftNoTrace = $true
  } catch {
    $failure = "The folder TaskSmack.exe ran from ($pkg) didn't delete after it quit: $($_.Exception.Message)"
  }
}
$cleanedUp = if ($InPlace) { 'the Mesa DLLs deleted from its folder' } else { 'its folder deleted' }

$summary = @(
  "### $Title",
  '',
  "- Result: $(if ($failure) { 'FAILED' } else { 'passed' })",
  "- OpenGL renderer: ``$renderer``",
  "- OpenGL version: ``$version``",
  "- Software driver: Mesa llvmpipe from $(if ($env:RUNNER_ARCH -eq 'ARM64') { 'mmozeiko/build-mesa' } else { 'mesa-dist-win' }) ($(if ($InPlace) { 'beside the installed exe for the check only' } else { 'temporary copy only, not in the package' }))",
  "- Quit by itself with config.toml saved, from a fresh config and from v0.8.0's (its settings kept): $(if ($configSaved) { 'yes' } else { 'no' })",
  "- Left no trace (no new %APPDATA%/%LOCALAPPDATA% entries; $cleanedUp): $(if ($leftNoTrace) { 'yes' } else { 'no' })"
)
if ($failure) { $summary += "- Failure: $failure" }
if ($env:GITHUB_STEP_SUMMARY) {
  $summary -join "`n" | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
} else {
  $summary | ForEach-Object { Write-Host $_ }
}

if ($failure) {
  Write-Error $failure
  exit 1
}
Write-Host "Smoke check passed: TaskSmack.exe started on '$renderer' (OpenGL $version) with no errors, quit by itself after $AliveSeconds s with config.toml saved, from a fresh config and from v0.8.0's (whose settings it kept), and left no trace."
