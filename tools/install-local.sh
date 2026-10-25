#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PREFIX="${PREFIX:-${ROOT_DIR}/packaging/dist}"
HICOLOR_DIR="${HOME}/.local/share/icons/hicolor"
INDEX_FILE="${HICOLOR_DIR}/index.theme"

# The optimized preset puts the binary in bin/; the second path is the pre-bin/ layout
BUILD_BIN="${ROOT_DIR}/build/optimized/bin/TaskSmack"
ALT_BUILD_BIN="${ROOT_DIR}/build/optimized/TaskSmack"

# Refresh the desktop/icon caches when their tools are installed (desktop-file-utils and
# gtk-update-icon-cache are optional; a missing one shouldn't abort the install under set -e)
refresh_desktop_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${HOME}/.local/share/applications" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache "${HICOLOR_DIR}" || true
    fi
}

# One Desktop Entry Exec argument for a path (#2342): quoted, with the quoting rule's escapes
# (\ " ` $), % doubled, then the string type's backslash escape. A newline cannot be represented.
desktop_exec_quote() {
    local path="$1"
    if [[ "${path}" == *$'\n'* ]]; then
        echo "A launcher path cannot contain a newline: ${path}" >&2
        return 1
    fi
    path="${path//\\/\\\\}"
    path="${path//\"/\\\"}"
    path="${path//\`/\\\`}"
    path="${path//\$/\\\$}"
    path="\"${path}\""
    path="${path//%/%%}"
    path="${path//\\/\\\\}"
    printf '%s' "${path}"
}

# Point a desktop entry's Exec line at a binary, without passing the path through sed syntax
set_desktop_exec() {
    local entry="$1" exec_value
    exec_value="$(desktop_exec_quote "$2")" || return 1
    EXEC_LINE="Exec=${exec_value}" awk '/^Exec=/ { print ENVIRON["EXEC_LINE"]; next } { print }' "${entry}" >"${entry}.tmp"
    mv "${entry}.tmp" "${entry}"
}

# Sourced (tests/tools/test_install_local_sh.py): define the functions only
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    return 0
fi

if [[ -x "${BUILD_BIN}" ]]; then
    :
elif [[ -x "${ALT_BUILD_BIN}" ]]; then
    BUILD_BIN="${ALT_BUILD_BIN}"
else
    echo "Missing optimized build at ${BUILD_BIN} (or ${ALT_BUILD_BIN}). Run the optimized build first." >&2
    exit 1
fi

# Use CMake install to mirror the packaged Linux layout (binary + libs under bin/,
# assets under share/<project_name_lower>/assets/ -- see cmake/InstallRules.cmake)
cmake --install "${ROOT_DIR}/build/optimized" --prefix "${PREFIX}"

# Install launcher entry and icon into the user's local desktop paths
DESKTOP_SRC="${ROOT_DIR}/assets/linux/app.tasksmack.TaskSmack.desktop"
DESKTOP_DST="${HOME}/.local/share/applications/app.tasksmack.TaskSmack.desktop"
ICON_SRC_PNG="${ROOT_DIR}/assets/icons/tasksmack-256.png"
ICON_DST="${HOME}/.local/share/icons/hicolor/256x256/apps/app.tasksmack.TaskSmack.png"
BINARY_PATH="${PREFIX}/bin/TaskSmack"

install -Dm644 "${DESKTOP_SRC}" "${DESKTOP_DST}"
set_desktop_exec "${DESKTOP_DST}" "${BINARY_PATH}"
install -Dm644 "${ICON_SRC_PNG}" "${ICON_DST}"

# Ensure a hicolor index so gtk-update-icon-cache keeps all sizes. Only when there is none: an
# existing ~/.local/share/icons/hicolor/index.theme belongs to the user or another app.
mkdir -p "${HICOLOR_DIR}"
if [[ ! -f "${INDEX_FILE}" ]]; then
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
fi

refresh_desktop_caches

echo "Installed to ${PREFIX} and registered launcher entry app.tasksmack.TaskSmack"
