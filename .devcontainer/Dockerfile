# TaskSmack dev container: Ubuntu 24.04 (the Linux CI runner OS) with the same LLVM major,
# GUI build dependencies, and Xvfb test runtime that Linux CI installs.
# See .devcontainer/install-toolchain.sh for where each package list comes from.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# Keep in sync with LLVM_VERSION in .github/workflows/ci.yml and PYTHON_VERSION in
# tools/setup-dev.sh (Renovate bumps all of them together).
ARG LLVM_VERSION=22
ARG PYTHON_VERSION=3.14
# ubuntu:24.04 already ships a non-root "ubuntu" user (UID 1000); VS Code remaps its
# UID/GID to the host user's on Linux (updateRemoteUserUID).
ARG USERNAME=ubuntu

COPY install-toolchain.sh /tmp/install-toolchain.sh
RUN bash /tmp/install-toolchain.sh "${LLVM_VERSION}" "${PYTHON_VERSION}" "${USERNAME}" \
    && rm -f /tmp/install-toolchain.sh

ENV TASKSMACK_PYTHON_VERSION=${PYTHON_VERSION}

USER ${USERNAME}
