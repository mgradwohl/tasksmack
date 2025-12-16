#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
PREFIX="${PREFIX:-${ROOT_DIR}/packaging/dist/install}"
BINARY_PATH="${PREFIX}/bin/TaskSmack"
DESKTOP_SRC="${PREFIX}/share/applications/app.tasksmack.TaskSmack.desktop"
ICON_SRC="${PREFIX}/share/icons/hicolor/scalable/apps/app.tasksmack.TaskSmack.svg"
DESKTOP_DST="${HOME}/.local/share/applications/app.tasksmack.TaskSmack.desktop"
ICON_DST="${HOME}/.local/share/icons/hicolor/scalable/apps/app.tasksmack.TaskSmack.svg"
HICOLOR_DIR="${HOME}/.local/share/icons/hicolor"
INDEX_FILE="${HICOLOR_DIR}/index.theme"

if [[ ! -x "${ROOT_DIR}/build/optimized/TaskSmack" ]]; then
    echo "Missing optimized build at ${ROOT_DIR}/build/optimized. Run the optimized build first." >&2
    exit 1
fi

cmake --install "${ROOT_DIR}/build/optimized" --prefix "${PREFIX}"

install -Dm644 "${DESKTOP_SRC}" "${DESKTOP_DST}"
install -Dm644 "${ICON_SRC}" "${ICON_DST}"

# Ensure Exec points to the installed binary
sed -i "s|^Exec=.*|Exec=${BINARY_PATH}|" "${DESKTOP_DST}"

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
