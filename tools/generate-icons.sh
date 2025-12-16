#!/usr/bin/env bash
# Generate ICO and PNG files from SVG using Inkscape and Python/Pillow
# Requirements:
#   - Inkscape: sudo apt install inkscape
#   - Python 3 with Pillow: pip install pillow

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SVG_PATH="${SCRIPT_DIR}/../assets/icons/tasksmack.svg"
OUTPUT_DIR="${SCRIPT_DIR}/../assets/icons"

echo "Generating icons from: $SVG_PATH"
echo "Output directory: $OUTPUT_DIR"

# Check for Inkscape
if ! command -v inkscape &> /dev/null; then
    echo "Error: Inkscape not found. Install with:"
    echo "  sudo apt install inkscape"
    exit 1
fi

# Check for Python and Pillow
if ! command -v python3 &> /dev/null; then
    echo "Error: Python 3 not found. Install with:"
    echo "  sudo apt install python3 python3-pip"
    exit 1
fi

if ! python3 -c "import PIL" 2>/dev/null; then
    echo "Installing Pillow..."
    pip3 install --user pillow
fi

# Sizes for icon files
SIZES=(16 24 32 48 64 128 256 512)

# Generate PNGs at each size
echo ""
echo "Generating PNG files..."
for size in "${SIZES[@]}"; do
    PNG_FILE="${OUTPUT_DIR}/tasksmack-${size}.png"
    echo "  ${size}x${size}..."
    inkscape --export-type=png --export-filename="$PNG_FILE" -w "$size" -h "$size" "$SVG_PATH" 2>/dev/null
done

# Generate ICO file using Python/Pillow (for Windows/Wine cross-compilation)
echo ""
echo "Generating ICO file..."
python3 -c "
from PIL import Image
import os

icon_dir = '${OUTPUT_DIR}'
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
"

# Copy 256px as main PNG
cp "${OUTPUT_DIR}/tasksmack-256.png" "${OUTPUT_DIR}/tasksmack.png"
echo "Copied 256x256 PNG as tasksmack.png"

echo ""
echo "Done! Generated files:"
ls -la "${OUTPUT_DIR}"/tasksmack*
