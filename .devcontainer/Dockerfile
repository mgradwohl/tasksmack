# TaskSmack's dev container (#136): Ubuntu 26.04, as CI's Linux jobs run, with the toolchain and libraries
# tools/setup-dev.sh installs (LLVM 23, CMake, Ninja, Python 3.14, the GUI and systemd/libbpf/libcurl headers, xvfb).
# One script for a workstation, a container and the docs, so the three can't drift.
FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
ARG USERNAME=dev
ARG USER_UID=1000

# setup-dev.sh calls sudo for apt; as root here, sudo is only the pass-through it expects.
RUN apt-get update \
    && apt-get install -y --no-install-recommends sudo ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

# Only what the script reads, so a change elsewhere in the repo doesn't rebuild this layer. Its project venv is
# made in the checkout by devcontainer.json's postCreateCommand, not here.
COPY tools/setup-dev.sh tools/common.sh /tmp/tasksmack-setup/tools/
COPY requirements.txt requirements-glad.txt /tmp/tasksmack-setup/
RUN /tmp/tasksmack-setup/tools/setup-dev.sh \
    && rm -rf /tmp/tasksmack-setup /var/lib/apt/lists/*

# A non-root user for the workspace, with sudo for anything else a session needs.
RUN (id -u ubuntu >/dev/null 2>&1 && userdel -r ubuntu || true) \
    && useradd --create-home --shell /bin/bash --uid "${USER_UID}" "${USERNAME}" \
    && echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > "/etc/sudoers.d/${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/${USERNAME}"
USER ${USERNAME}
