#!/usr/bin/env bash
# Generate ICO and PNG files from SVG using Inkscape and Python/Pillow
#
# Sources (#1508):
#   - assets/icons/tasksmack.svg        master art, rendered for every size not listed below
#   - assets/icons/tasksmack-small.svg  simplified art (fewer burst points, thicker outline, two bars),
#                                       rendered for SMALL_SIZES (16, 24 and 32 px) where the master's
#                                       detail turns to mush
# The .ico is assembled from those per-size PNGs, so its 16/24/32 px frames use the small art too.
#
# Requirements:
#   - Inkscape: sudo apt install inkscape
#   - Python >= 3.14 with Pillow, hash-pinned in requirements-icons.txt. The project venv (.venv,
#     made by tools/setup-dev.sh) is used when present; the script installs Pillow into it:
#       .venv/bin/python -m pip install --require-hashes -r requirements-icons.txt

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SVG_PATH="${SCRIPT_DIR}/../assets/icons/tasksmack.svg"
SMALL_SVG_PATH="${SCRIPT_DIR}/../assets/icons/tasksmack-small.svg"
OUTPUT_DIR="${SCRIPT_DIR}/../assets/icons"

echo "Generating icons from: $SVG_PATH"
echo "Output directory: $OUTPUT_DIR"

# Check for Inkscape
if ! command -v inkscape &> /dev/null; then
    echo "Error: Inkscape not found. Install with:"
    echo "  sudo apt install inkscape"
    exit 1
fi

# Check for Python and Pillow. find_python prefers the project venv, where pip may install
# (Ubuntu's system python3 refuses `pip install` under PEP 668).
if ! PYTHON="$(find_python)"; then
    echo "Error: Python >= 3.14 not found. Run tools/setup-dev.sh to create the project venv (.venv)."
    exit 1
fi

if ! "$PYTHON" -c "import PIL" 2>/dev/null; then
    echo "Installing Pillow (hash-pinned) into $PYTHON's environment..."
    "$PYTHON" -m pip install --require-hashes -r "${REPO_ROOT}/requirements-icons.txt"
fi

# Sizes for icon files (the tracked assets/icons/tasksmack-<size>.png set, as in generate-icons.ps1);
# SMALL_SIZES render from SMALL_SVG_PATH instead of SVG_PATH
SIZES=(16 24 32 48 64 128 256)
SMALL_SIZES=(16 24 32)

# Generate PNGs at each size
echo ""
echo "Generating PNG files..."
for size in "${SIZES[@]}"; do
    PNG_FILE="${OUTPUT_DIR}/tasksmack-${size}.png"
    SOURCE_SVG="$SVG_PATH"
    for small in "${SMALL_SIZES[@]}"; do
        if [[ "$size" == "$small" ]]; then
            SOURCE_SVG="$SMALL_SVG_PATH"
        fi
    done
    echo "  ${size}x${size} from $(basename "$SOURCE_SVG")..."
    inkscape --export-type=png --export-filename="$PNG_FILE" -w "$size" -h "$size" "$SOURCE_SVG" 2>/dev/null
done

# Generate ICO file using Python/Pillow (for Windows/Wine cross-compilation)
echo ""
echo "Generating ICO file..."
"$PYTHON" -c "
from PIL import Image
import os

icon_dir = '${OUTPUT_DIR}'
# One frame per rendered PNG (not a downscale of the 256 px one), so the small art reaches the .ico.
imgs = [Image.open(os.path.join(icon_dir, f'tasksmack-{size}.png')).convert('RGBA')
        for size in [256, 128, 64, 48, 32, 24, 16]]

ico_path = os.path.join(icon_dir, 'tasksmack.ico')
imgs[0].save(ico_path, format='ICO', append_images=imgs[1:])
print(f'Created {ico_path} ({os.path.getsize(ico_path)} bytes)')
"

echo ""
echo "Done! Generated files:"
ls -la "${OUTPUT_DIR}"/tasksmack*
