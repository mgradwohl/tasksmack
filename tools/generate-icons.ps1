# Generate ICO file from SVG using Inkscape and Python/Pillow
# Requirements:
#   - Inkscape (for SVG to PNG conversion)
#   - Python 3 with Pillow (pip install pillow) for ICO creation

param(
    [string]$SvgPath = "$PSScriptRoot/../assets/icons/tasksmack.svg",
    [string]$OutputDir = "$PSScriptRoot/../assets/icons"
)

$ErrorActionPreference = "Stop"

# Resolve paths
$SvgPath = Resolve-Path $SvgPath -ErrorAction Stop
$OutputDir = Resolve-Path $OutputDir -ErrorAction Stop

Write-Host "Generating icons from: $SvgPath"
Write-Host "Output directory: $OutputDir"

# Find Inkscape
$inkscapePaths = @(
    "C:\Program Files\Inkscape\bin\inkscape.exe",
    "$env:LOCALAPPDATA\Programs\Inkscape\bin\inkscape.exe",
    (Get-Command "inkscape" -ErrorAction SilentlyContinue).Source
) | Where-Object { $_ -and (Test-Path $_) }

if (-not $inkscapePaths) {
    Write-Error "Inkscape not found. Install from https://inkscape.org/ or via: winget install Inkscape.Inkscape"
    exit 1
}
$inkscape = $inkscapePaths[0]
Write-Host "Using Inkscape: $inkscape"

# Check for Python and Pillow
$python = Get-Command "python" -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Error "Python not found. Install from https://python.org/ or via: winget install Python.Python.3"
    exit 1
}

$pillowCheck = & python -c "import PIL" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing Pillow..."
    & pip install pillow --quiet
}

# Sizes for icons
$sizes = @(16, 24, 32, 48, 64, 128, 256)

# Generate PNGs at each size
Write-Host ""
Write-Host "Generating PNG files..."
foreach ($size in $sizes) {
    $pngFile = Join-Path $OutputDir "tasksmack-$size.png"
    Write-Host "  ${size}x${size}..."
    & $inkscape --export-type=png --export-filename="$pngFile" -w $size -h $size "$SvgPath" 2>$null
}

# Generate ICO file using Python/Pillow
Write-Host ""
Write-Host "Generating ICO file..."
$pythonScript = @"
from PIL import Image
import os

icon_dir = r'$OutputDir'
source = Image.open(os.path.join(icon_dir, 'tasksmack-256.png')).convert('RGBA')

imgs = []
for size in [256, 128, 64, 48, 32, 24, 16]:
    if size == 256:
        imgs.append(source.copy())
    else:
        imgs.append(source.resize((size, size), Image.Resampling.LANCZOS))

ico_path = os.path.join(icon_dir, 'tasksmack.ico')
imgs[0].save(ico_path, format='ICO', append_images=imgs[1:])
print(f'Created {ico_path} ({os.path.getsize(ico_path)} bytes)')
"@

& python -c $pythonScript

# Copy 256px as main PNG
Copy-Item (Join-Path $OutputDir "tasksmack-256.png") (Join-Path $OutputDir "tasksmack.png")

Write-Host ""
Write-Host "Done! Generated files:"
Get-ChildItem $OutputDir -Filter "tasksmack*" | ForEach-Object { Write-Host "  $($_.Name) ($($_.Length) bytes)" }
