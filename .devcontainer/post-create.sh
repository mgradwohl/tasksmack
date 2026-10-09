#!/usr/bin/env bash
# .devcontainer/post-create.sh -- runs once after the dev container is created
# (devcontainer.json postCreateCommand). Creates the project venv the same way
# tools/setup-dev.sh does: GLAD's hash-locked build deps plus the dev tools in requirements.txt.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="python${TASKSMACK_PYTHON_VERSION:?TASKSMACK_PYTHON_VERSION is set by the Dockerfile}"
VENV="${REPO_ROOT}/.venv"

# The workspace is bind-mounted, so an existing .venv may belong to the host. Reuse it only if
# its interpreter runs here and is the expected version; otherwise build a separate one.
if ! "${VENV}/bin/python" -c "import sys; sys.exit(0 if '%d.%d' % sys.version_info[:2] == '${TASKSMACK_PYTHON_VERSION}' else 1)" &>/dev/null; then
    if [[ -e "${VENV}" ]]; then
        echo "Existing .venv is not usable inside the container; recreating it." >&2
    fi
    "${PYTHON}" -m venv --clear "${VENV}"
fi

"${VENV}/bin/python" -m pip install --require-hashes -r "${REPO_ROOT}/requirements-glad.txt"
"${VENV}/bin/python" -m pip install --require-hashes -r "${REPO_ROOT}/requirements.txt"

"${REPO_ROOT}/tools/check-prereqs.sh" || echo "check-prereqs.sh reported problems (see above)." >&2

echo ""
echo "Dev container ready. Next:"
echo "  cmake --workflow --preset dev                                   # configure + build + test (display tests skip)"
echo "  xvfb-run -a -s '-screen 0 1920x1080x24 -noreset' ctest --preset debug   # run the GL/window tests too"
