# Tests for tools/check-prereqs.ps1's ccache version parse (#2350): a two-component version
# (ccache 4.14, which setup-dev.ps1 pins) is recognized like a three-component one. Get-CcacheVersion
# is taken from the script with the PowerShell parser and run against a stub ccache function; one
# full run of the checker with a stub ccache.cmd first on PATH checks the row it prints. The LLVM
# rows (#3041) are checked through Write-LlvmToolStatus, taken from the script the same way. Registered
# in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$checker = Join-Path $PSScriptRoot 'check-prereqs.ps1'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($checker, [ref]$null, [ref]$errors)
Assert-True (@($errors).Count -eq 0) "check-prereqs.ps1 does not parse: $errors"
$definition = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-CcacheVersion'
    }, $true)
Assert-True ($null -ne $definition) 'Get-CcacheVersion not found in check-prereqs.ps1'
. ([scriptblock]::Create($definition.Extent.Text))

$minimum = [Version]'4.9.1'
$script:StubOutput = @()
function ccache { $script:StubOutput }

foreach ($case in @(
        @{ Output = @('ccache version 4.14'); Expected = '4.14'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.14.1', 'Features: file-storage http-storage'); Expected = '4.14.1'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.9.1'); Expected = '4.9.1'; MeetsMinimum = $true }
        @{ Output = @('ccache version 4.2'); Expected = '4.2'; MeetsMinimum = $false }
        @{ Output = @('ccache, but not a version line'); Expected = $null; MeetsMinimum = $false }
        @{ Output = @(); Expected = $null; MeetsMinimum = $false }
    )) {
    $script:StubOutput = $case.Output
    $version = Get-CcacheVersion
    $label = "[$($case.Output -join ' | ')]"
    if ($null -eq $case.Expected) {
        Assert-True ($null -eq $version) "$label parsed as [$version], expected none"
    }
    else {
        Assert-True ($version -is [Version] -and $version -eq [Version]$case.Expected) "$label parsed as [$version], expected $($case.Expected)"
    }
    Assert-True ([bool]($version -and $version -ge $minimum) -eq $case.MeetsMinimum) "$label against $minimum"
}

# An LLVM row (#3041): the compilers (-RequirePinned) fail on a major other than the pinned one, the
# other LLVM tools only warn, and below the minimum or missing fails either way.
foreach ($name in 'Write-Status', 'Write-LlvmToolStatus') {
    $function = $ast.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }.GetNewClosure(), $true)
    Assert-True ($null -ne $function) "$name not found in check-prereqs.ps1"
    . ([scriptblock]::Create($function.Extent.Text))
}
$MIN_CLANG_VERSION = 22
$PinnedLlvmMajor = 23
foreach ($case in @(
        @{ Major = '23'; RequirePinned = $true; Ok = $true; Row = '^✓ clang\s+23\s+\(>= 22\)' }
        @{ Major = '22'; RequirePinned = $true; Ok = $false; Row = '^✗ clang\s+22\s+\(= 23\)' }
        @{ Major = '24'; RequirePinned = $true; Ok = $false; Row = '^✗ clang\s+24\s+\(= 23\)' }
        @{ Major = '22'; RequirePinned = $false; Ok = $true; Row = '^! clang\s+22\s+\(>= 22\)' }
        @{ Major = '21'; RequirePinned = $false; Ok = $false; Row = '^✗ clang\s+21\s+\(>= 22\)' }
        @{ Major = ''; RequirePinned = $true; Ok = $false; Row = '^✗ clang\s+NOT FOUND' }
    )) {
    $script:LlvmSkewSeen = $false
    $script:LlvmCompilerMajor = $null
    $printed = @(Write-LlvmToolStatus -Name 'clang' -Path 'C:\LLVM\bin\clang.exe' -Major $case.Major -RequirePinned:$case.RequirePinned 6>&1)
    $result = @($printed | Where-Object { $_ -is [bool] })
    $row = (@($printed | Where-Object { $_ -isnot [bool] }) | ForEach-Object { "$_" }) -join ''
    $label = "clang $($case.Major) (RequirePinned $($case.RequirePinned))"
    Assert-True ($result.Count -eq 1 -and $result[0] -eq $case.Ok) "$label returned [$result]"
    Assert-True ($row -match $case.Row) "$label row: [$row]"
    Assert-True ([bool]$script:LlvmCompilerMajor -eq ($case.RequirePinned -and $case.Major -in '22', '24')) "$label compiler major: [$($script:LlvmCompilerMajor)]"
}

# The checker itself, with a stub ccache.cmd ahead of any real ccache: its row shows the version.
$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-check-prereqs-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $root 'ccache.cmd'), "@echo ccache version 4.14`r`n")
    $env:PATH = "$root$([IO.Path]::PathSeparator)$env:PATH"
    $pwsh = (Get-Process -Id $PID).Path
    $output = & $pwsh -NoProfile -File $checker 2>&1 | Out-String
    $row = @($output -split '\r?\n' | Where-Object { $_ -match '^\S+ ccache\s' })
    Assert-True ($row.Count -eq 1) "Expected one ccache row:`n$output"
    Assert-True ($row[0] -match '^\S+ ccache\s+4\.14\s') "ccache row: [$($row[0])]"
    Assert-True ($row[0] -like '*ccache.cmd*') "ccache row does not show the stub's path: [$($row[0])]"
    # The MSVC STL row (#4354): present whether or not this machine has Visual Studio, with CI's floor.
    $msvcRow = @($output -split '\r?\n' | Where-Object { $_ -match '^\S+ MSVC STL\s' })
    Assert-True ($msvcRow.Count -eq 1) "Expected one MSVC STL row:`n$output"
    Assert-True ($msvcRow[0] -match '\(>= 14\.51\)') "MSVC STL row lacks the 14.51 floor: [$($msvcRow[0])]"

    # The Python rows with a non-ASCII user name in the path (#4320): a copy of the checker under a directory
    # named with a CJK name and with an accented one, each holding a real venv, so sys.executable carries it.
    # Python writes a pipe in the ANSI code page unless run with -X utf8 (the CJK name is a UnicodeEncodeError,
    # so every candidate is skipped and python3 shows as failed); the accented name decodes as mojibake if the
    # checker reads it as the OEM code page. Run with a copy of common.ps1, which the checker dot-sources.
    $basePython = $env:TASKSMACK_TEST_PYTHON
    if (-not $basePython) {
        $found = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
        if ($found) { $basePython = $found.Source }
    }
    if (-not $basePython) {
        Write-Host 'no Python found for the non-ASCII path check; skipping it'
    }
    else {
        # Read the child's UTF-8 output as UTF-8, whatever this session's console code page is.
        $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }
        # A developer's PYTHONIOENCODING or PYTHONUTF8 would hide the bug; the checker has to cope without them.
        $savedPythonEnv = @{ PYTHONIOENCODING = $env:PYTHONIOENCODING; PYTHONUTF8 = $env:PYTHONUTF8 }
        Remove-Item Env:PYTHONIOENCODING, Env:PYTHONUTF8 -ErrorAction SilentlyContinue
        try {
            foreach ($name in @(([string][char]0x738B + [char]0x4F1F), ('Jos' + [char]0xE9))) {
                $repo = Join-Path $root $name
                $toolsDir = Join-Path $repo 'tools'
                New-Item -ItemType Directory -Path $toolsDir | Out-Null
                Copy-Item -LiteralPath $checker, (Join-Path $PSScriptRoot 'common.ps1') -Destination $toolsDir
                & $basePython -X utf8 -m venv (Join-Path $repo '.venv')
                Assert-True ($LASTEXITCODE -eq 0) "Could not create a venv under $repo"
                $venvPython = Join-Path $repo '.venv\Scripts\python.exe'
                $output = & $pwsh -NoProfile -File (Join-Path $toolsDir 'check-prereqs.ps1') 2>&1 | Out-String
                $pythonRow = @($output -split '\r?\n' | Where-Object { $_ -match '^\S+ python3\s' })
                Assert-True ($pythonRow.Count -eq 1) "Expected one python3 row under $name`:`n$output"
                Assert-True ($pythonRow[0] -notmatch 'NOT FOUND') "python3 not found under $name (a probe failed on the path?): [$($pythonRow[0])]"
                Assert-True ($pythonRow[0].Contains($venvPython)) "python3 row doesn't show the venv's path under ${name}: [$($pythonRow[0])]"
            }
        }
        finally {
            foreach ($key in $savedPythonEnv.Keys) {
                if ($null -ne $savedPythonEnv[$key]) { Set-Item -LiteralPath "Env:$key" -Value $savedPythonEnv[$key] }
            }
        }
    }

    Write-Host 'check-prereqs.ps1 tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
