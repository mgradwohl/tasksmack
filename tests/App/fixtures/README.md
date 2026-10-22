# Test fixtures for the App tests

## config-v0.8.0.toml

A `config.toml` saved by the released TaskSmack v0.8.0, not written by hand.
`test_UserConfigPersistence.cpp` (`AConfigSavedByV080LoadsEveryValueAndSavesThemBack`) loads it with today's
`UserConfig`, checks every value, then saves it, loads it again and checks the values still hold (#2725).

How it was made, on 2026-10-21:

- **Build:** v0.8.0's `TaskSmack-0.8.0-Linux.deb` from the GitHub release. Its SHA-256,
  `ea44d9c7955f4349141e421209141b1ff61d38c9cf6d4e6fc8d8b9075155edf4`, matches the release's digest.
  It was unpacked with `dpkg-deb -x`, not installed.
- **Where it ran:** Ubuntu 26.04 in WSL, on a headless Xvfb display (1920x1080), with `SDL_VIDEO_DRIVER=x11`
  and Mesa llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`).
- **Config:** `HOME` and `XDG_CONFIG_HOME` pointed at a new, empty scratch folder, so v0.8.0 started from its
  defaults. v0.8.0 has no `TASKSMACK_CONFIG_DIR`.
- **Input:** sent with `xdotool` to that X server only. A screenshot was taken after each step.

The steps, in v0.8.0's own UI:

1. In the "Limited Data Available" notice, ticked "Don't show again" and pressed OK (`show_privilege_notice = false`).
2. In Settings (the gear in the title bar), changed these, then pressed Apply:
   - Theme: Dracula;
   - Font Size: Large;
   - Metric Refresh Rate: 2 seconds;
   - Metric History: 10 minutes.
3. On the Processes tab, used the column header's right-click menu to hide User (shown by default) and show
   Threads (hidden by default).
4. Resized the window to 1500x860 and moved it to (150, 90) with `xdotool windowsize` / `windowmove`. Xvfb has no
   window manager, so there was no border to drag.
5. Closed it with the title bar's Close button, which is v0.8.0's normal exit and saves the config.

v0.8.0 saved no Processes table layout: it turned ImGui's `.ini` off and had no `[process_table]`. Sort column,
column widths and column order were therefore not changed, and the test checks that the missing layout loads as
empty.

The file is v0.8.0's own Linux output, byte for byte, except for a final newline: v0.8.0 wrote none, and the
repository's `end-of-file-fixer` hook added one. Windows writes the same TOML keys to `%APPDATA%\TaskSmack\config.toml`;
only `[window] x`/`y` are in that display's coordinates.
