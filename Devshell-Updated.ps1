# Load Visual Studio DevShell
Import-Module "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"

Enter-VsDevShell a73be645 -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

# Prepend LLVM
$llvm = "C:\Program Files\LLVM"
if (Test-Path "$llvm\bin") {
    $env:PATH = "$llvm\bin;$env:PATH"
    $env:LLVM_ROOT = $llvm
}

# Prepend CMake
$cmake = "C:\Program Files\CMake\bin"
if (Test-Path $cmake) {
    $env:PATH = "$cmake;$env:PATH"
    $env:CMAKE_BIN_DIR = $cmake
}

# Prepend VCPKG
$vcpkg = "${env:USERPROFILE}\source\vcpkg"
if (Test-Path $vcpkg) {
    $env:PATH = "$vcpkg;$env:PATH"
    $env:VCPKG_ROOT = $vcpkg
}

# Qt6 via vcpkg
$qtPrefix = "$vcpkg\installed\x64-windows"
$qtConfig = "$qtPrefix\share\Qt6"
if (Test-Path $qtConfig) {
    $env:QT6_PREFIX_PATH = $qtPrefix
    $env:Qt6_DIR = $qtConfig
}

try {
    $clangPath = (Get-Command clang++.exe -ErrorAction Stop).Source
    Write-Host "Using clang from:  $clangPath" -ForegroundColor Green
} catch {
    Write-Warning "clang++.exe not found in PATH"
}
try {
    $cmakePath = (Get-Command cmake.exe -ErrorAction Stop).Source
    Write-Host "Using cmake from: $cmakePath" -ForegroundColor Green
} catch {
    Write-Warning "cmake.exe not found in PATH"
}
try {
    $vcpkgPath = (Get-Command vcpkg.exe -ErrorAction Stop).Source
    Write-Host "Using vcpkg from: $vcpkgPath" -ForegroundColor Green
} catch {
    Write-Warning "vcpkg.exe not found in PATH"
}
if ($env:Qt6_DIR) { Write-Host "Using Qt6 from:  $env:Qt6_DIR" -ForegroundColor Green }