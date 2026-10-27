<!--
The "Which file should I download?" section every release page opens with (#3144). changelog.yml's
release-notes job fills it in through tools/compose-release-notes.py, which drops this comment and
replaces:
  {{VERSION}}       the release's version, without the "v" (0.10.0);
  {{VERIFY_FILES}}  the files only needed to check a download: "`SHA256SUMS`, `SHA256SUMS.bundle`" when
                    the release has them, "The `.bundle` files" when it has a bundle per file (v0.9.x).
Keep it in step with the asset names in CONTRIBUTING.md's "Release Artifacts".
-->
## Which file should I download?

| Your computer | Download this | Notes |
|---|---|---|
| **Windows, most PCs** (Intel or AMD) | **`TaskSmack-{{VERSION}}-win64.exe`** | The installer. **Recommended**: it installs to Program Files, adds a Start menu entry and an uninstaller, and is needed for **Retry as administrator**. |
| Windows on Arm (Snapdragon X, Surface Pro 9 5G and later) | **`TaskSmack-{{VERSION}}-windows-arm64.exe`** | The same installer, for ARM64. |
| **Ubuntu 24.04+ / Debian 13+**, x64 | **`TaskSmack-{{VERSION}}-Linux.deb`** | Install with `sudo apt install ./TaskSmack-{{VERSION}}-Linux.deb`, then run `TaskSmack`. Includes the polkit helper for **Authenticate and retry**. |
| Ubuntu 24.04+ / Debian 13+, ARM64 | **`TaskSmack-{{VERSION}}-linux-aarch64.deb`** | The same `.deb`, for ARM64. |
| Other Linux (Fedora 39+, RHEL 10+, Arch, …) | `TaskSmack-{{VERSION}}-Linux.tar.gz` (x64) or `TaskSmack-{{VERSION}}-linux-aarch64.tar.gz` (ARM64) | Unpack it and run `TaskSmack`. It has no privileged helper. |

- **Portable Windows build:** `TaskSmack-{{VERSION}}-win64.zip` / `-windows-arm64.zip` run without installing. Retry as administrator only works when TaskSmack sits in a folder only administrators can change, which is where the installer puts it.
- **Not sure which architecture you have?**
  - **Windows:** Settings › System › About › *System type* ("x64-based" or "ARM-based").
  - **Linux:** `uname -m` (`x86_64` or `aarch64`).
- **Requirements:**
  - Windows 10 version 1903 or later.
  - The Linux packages need **glibc 2.38 or newer**. On Ubuntu 22.04, Debian 12 or RHEL 9, [build from source](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md#quick-start).
- **You don't need the other files to install.** {{VERIFY_FILES}}, `*-build-inputs.json` and the `*-sbom.spdx.json` are for checking downloads and reproducing builds; see "Verifying downloads" below.
- **Windows SmartScreen may say the file "isn't commonly downloaded".** The files are signed by **Matt Gradwohl**, but a new release starts with no SmartScreen reputation.
  - In Edge, choose **Keep › Show more › Keep anyway**.
  - When you run it, choose **More info › Run anyway**.
