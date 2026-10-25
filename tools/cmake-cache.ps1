# CMakeCache.txt reading shared by the PowerShell tools (bench.ps1, profile-etw-common.ps1),
# dot-sourced by them (#1479). Works on PowerShell 7.0 and later.

function Read-CMakeCache {
    # The entries of a CMakeCache.txt as CMake itself reads them (cmState::ParseCacheEntry):
    # "KEY":TYPE=VALUE, then KEY:TYPE=VALUE, then the untyped "KEY"=VALUE and KEY=VALUE. CMake
    # quotes a key holding ':', and any other character -- '-', '.', '+' of a custom build type's
    # CMAKE_CXX_FLAGS_<CONFIG> -- is part of an unquoted key. One trailing carriage return is part
    # of the line ending, trailing spaces, tabs and carriage returns are dropped, and a value in
    # single quotes (how CMake writes one with trailing whitespace) loses them. Keyed
    # case-sensitively (CMake's keys are); a later entry wins, as in CMake. Kept in step with
    # CACHE_ENTRY_PATTERNS / parse_cmake_cache in tools/bench-manifest.py.
    param([string]$Path)
    $valuePattern = '(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$'
    $entryPatterns = @(
        ('^"([^"]*)":[^=]*=' + $valuePattern)
        ('^([^=:]*):[^=]*=' + $valuePattern)
        ('^"([^"]*)"=' + $valuePattern)
        ('^([^=]*)=' + $valuePattern)
    )
    $cache = [hashtable]::new([StringComparer]::Ordinal)
    foreach ($raw in [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8).Split("`n")) {
        # One trailing carriage return is part of the line ending (cmSystemTools::GetLineFromStream).
        $line = $(if ($raw.EndsWith("`r", [StringComparison]::Ordinal)) { $raw.Substring(0, $raw.Length - 1) } else { $raw }).TrimStart(' ', "`t")
        # Blank lines, '#' comments and '//' help text are not entries (cmCacheManager::LoadCache).
        if (-not $line -or $line.StartsWith('#', [StringComparison]::Ordinal) -or $line.StartsWith('//', [StringComparison]::Ordinal)) { continue }
        foreach ($pattern in $entryPatterns) {
            $match = [regex]::Match($line, $pattern)
            if ($match.Success) {
                $value = $match.Groups[2].Value
                if ($value.Length -ge 2 -and $value[0] -eq "'" -and $value[-1] -eq "'") { $value = $value.Substring(1, $value.Length - 2) }
                $cache[$match.Groups[1].Value] = $value
                break
            }
        }
    }
    return $cache
}

function ConvertTo-CMakeUpper {
    # CMake's cmSystemTools::UpperCase: ASCII letters only, as the <CONFIG> suffix of
    # CMAKE_CXX_FLAGS_<CONFIG> is built. cmake_upper in tools/bench-manifest.py.
    param([string]$Value)
    return [regex]::Replace($Value, '[a-z]', { param($m) [string][char]([int][char]$m.Value - 32) })
}
