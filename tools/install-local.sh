#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
PREFIX="${PREFIX:-${ROOT_DIR}/packaging/dist}"
HICOLOR_DIR="${HOME}/.local/share/icons/hicolor"
INDEX_FILE="${HICOLOR_DIR}/index.theme"

BUILD_BIN="${ROOT_DIR}/build/optimized/TaskSmack"
ALT_BUILD_BIN="${ROOT_DIR}/build/optimized/bin/TaskSmack"

if [[ -x "${BUILD_BIN}" ]]; then
    :
elif [[ -x "${ALT_BUILD_BIN}" ]]; then
    BUILD_BIN="${ALT_BUILD_BIN}"
else
    echo "Missing optimized build at ${BUILD_BIN} (or ${ALT_BUILD_BIN}). Run the optimized build first." >&2
    exit 1
fi

# Use CMake install to mirror the optimized build layout (binary + libs at prefix root, assets under assets/)
cmake --install "${ROOT_DIR}/build/optimized" --prefix "${PREFIX}"

# Install launcher entry and icon into the user's local desktop paths
DESKTOP_SRC="${ROOT_DIR}/assets/linux/app.tasksmack.TaskSmack.desktop"
DESKTOP_DST="${HOME}/.local/share/applications/app.tasksmack.TaskSmack.desktop"
ICON_SRC_PNG="${ROOT_DIR}/assets/icons/tasksmack-256.png"
ICON_DST="${HOME}/.local/share/icons/hicolor/256x256/apps/app.tasksmack.TaskSmack.png"
BINARY_PATH="${PREFIX}/TaskSmack"

install -Dm644 "${DESKTOP_SRC}" "${DESKTOP_DST}"
sed -i "s|^Exec=.*|Exec=${BINARY_PATH}|" "${DESKTOP_DST}"
install -Dm644 "${ICON_SRC_PNG}" "${ICON_DST}"

update-desktop-database "${HOME}/.local/share/applications"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache "${HOME}/.local/share/icons/hicolor" || true
fi

# Ensure a complete hicolor index so gtk-update-icon-cache keeps all sizes
mkdir -p "${HICOLOR_DIR}"
cat >"${INDEX_FILE}" <<'EOF'
[Icon Theme]
Name=Hicolor
Directories=16x16/apps,24x24/apps,32x32/apps,48x48/apps,128x128/apps,256x256/apps,scalable/apps

[16x16/apps]
Size=16
Type=Fixed
Context=Applications

[24x24/apps]
Size=24
Type=Fixed
Context=Applications

[32x32/apps]
Size=32
Type=Fixed
Context=Applications

[48x48/apps]
Size=48
Type=Fixed
Context=Applications

[128x128/apps]
Size=128
Type=Fixed
Context=Applications

[256x256/apps]
Size=256
Type=Fixed
Context=Applications

[scalable/apps]
Size=256
Type=Scalable
MinSize=8
MaxSize=512
Context=Applications
EOF

update-desktop-database "${HOME}/.local/share/applications"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache "${HICOLOR_DIR}" || true
fi

echo "Installed to ${PREFIX} and registered launcher entry app.tasksmack.TaskSmack"
