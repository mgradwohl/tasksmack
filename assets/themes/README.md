# TaskSmack Themes

This directory contains TOML theme files that customize TaskSmack's appearance.

## Using Themes

1. Select a theme from **Settings** (⚙) → **Theme** dropdown
2. Click **Apply** to activate the theme

## Creating Custom Themes

### Quick Start

1. Copy an existing theme file (e.g., `arctic-fire.toml`)
2. Rename it (e.g., `my-theme.toml`)
3. Edit the colors to your liking
4. Restart TaskSmack or reload themes

### User Themes Directory

You can also place theme files in your user config directory:
- **Linux:** `~/.config/tasksmack/themes/`
- **Windows:** `%APPDATA%/TaskSmack/themes/`

User themes are loaded in addition to built-in themes.

## Theme File Structure

Themes are TOML files with the following sections:

### `[meta]` - Theme Metadata

```toml
[meta]
name = "My Theme"
description = "A brief description"
author = "Your Name"
version = "1.0"
```

### `[heatmap]` - CPU/Memory Heatmap Gradient

5 color stops from cool (idle) to hot (100%):

```toml
[heatmap]
gradient = [
    "#1565C0",  # 0%   - Cool/idle
    "#2196F3",  # 25%
    "#00E676",  # 50%  - Moderate
    "#FFB300",  # 75%  - Warning
    "#E53935",  # 100% - Hot/danger
]
```

### `[accents]` - Chart and UI Accent Colors

8 colors for charts, legends, and highlights:

```toml
[accents]
colors = [
    "#42A5F5",  # Blue
    "#FF7043",  # Orange
    "#00E676",  # Green
    "#AB47BC",  # Purple
    "#FFC107",  # Gold
    "#EC407A",  # Pink
    "#C6FF00",  # Lime
    "#FF8A65",  # Coral
]
```

### `[progress]` - Progress Bar Colors

```toml
[progress]
low = "#00E676"       # 0-50% (healthy)
medium = "#FFB300"    # 50-80% (caution)
high = "#E53935"      # 80-100% (critical)
```

### `[semantic]` - Text Colors

```toml
[semantic]
text_primary = "#E5EAF2"   # Main text
text_disabled = "#9AA4B2"  # Disabled elements
text_muted = "#999999"     # Secondary text
text_error = "#E53935"     # Error messages
text_warning = "#FFB300"   # Warnings
text_success = "#00E676"   # Success messages
text_info = "#42A5F5"      # Info messages
```

### `[status]` - Process State Colors

```toml
[status]
running = "#00E676"    # R - Running
sleeping = "#42A5F5"   # S - Sleeping
disk_sleep = "#FFB300" # D - Disk sleep (I/O)
zombie = "#E53935"     # Z - Zombie
stopped = "#FF7043"    # T - Stopped
idle = "#808080"       # I - Idle kernel thread
```

### `[charts]` - Metric Line/Fill Colors

```toml
[charts]
cpu = "#42A5F5"              # CPU line color
cpu_fill = "#42A5F54D"       # CPU fill (with alpha)
memory = "#00E676"           # Memory line color
memory_fill = "#00E6764D"    # Memory fill
io = "#FF7043"               # I/O line color
io_fill = "#FF70434D"        # I/O fill
```

### `[charts.gpu]` - GPU Metrics Colors

```toml
[charts.gpu]
utilization = "#BA68C8"
utilization_fill = "#BA68C84D"
memory = "#FF7043"
memory_fill = "#FF70434D"
temperature = "#FFD54F"
temperature_fill = "#FFD54F4D"
power = "#00E676"
power_fill = "#00E6764D"
```

### `[buttons.danger]` / `[buttons.success]` - Button Colors

```toml
[buttons.danger]
normal = "#C62828"
hovered = "#E53935"
active = "#8B0000"

[buttons.success]
normal = "#00ACC1"
hovered = "#26C6DA"
active = "#00838F"
```

### `[ui.*]` - ImGui UI Element Colors

The `[ui]` section contains colors for various UI elements:

| Section | Purpose |
|---------|---------|
| `[ui.window]` | Window backgrounds and borders |
| `[ui.frame]` | Input fields, combo boxes |
| `[ui.title]` | Window title bars |
| `[ui.bars]` | Menu and status bars |
| `[ui.scrollbar]` | Scrollbar colors |
| `[ui.controls]` | Checkboxes, sliders |
| `[ui.button]` | Standard buttons |
| `[ui.header]` | Collapsible headers |
| `[ui.separator]` | Separators |
| `[ui.tab]` | Tab bars |
| `[ui.table]` | Table elements |

## Color Format

Colors use hex format:
- `#RRGGBB` - Opaque color
- `#RRGGBBAA` - Color with alpha (transparency)

Alpha values:
- `FF` = fully opaque (100%)
- `80` = 50% transparent
- `4D` = 30% transparent
- `00` = fully transparent

## Tips

1. **Start from an existing theme** - Copy a theme close to what you want
2. **Use a color picker** - Tools like [coolors.co](https://coolors.co) help create palettes
3. **Test with data** - Run TaskSmack with real workloads to see all colors in action
4. **Consider contrast** - Ensure text is readable against backgrounds
5. **Light vs Dark** - Name light themes with `-light` suffix for clarity

## Built-in Themes

| Theme | Description |
|-------|-------------|
| Arctic Fire | Blue → Emerald → Amber → Red gradient (dark) |
| Arctic Fire Light | Light version of Arctic Fire |
| Cyberpunk | Neon pink/cyan cyberpunk aesthetic (dark) |
| Cyberpunk Light | Light version of Cyberpunk |
| Monochrome | Grayscale for accessibility |
| Monochrome Light | Light grayscale variant |
| Ubuntu Dark | Ubuntu terminal-inspired dark theme |
| Ubuntu Light | Ubuntu-inspired light theme |
| Windows Dark | Windows 11 dark mode inspired |
| Windows Light | Windows 11 light mode inspired |

## Troubleshooting

**Theme doesn't appear in list:**
- Check TOML syntax (use a TOML validator)
- Ensure `[meta]` section has `name` field
- Check file extension is `.toml`

**Colors look wrong:**
- Verify hex color format (`#RRGGBB` or `#RRGGBBAA`)
- Check alpha values if elements appear transparent

**Theme resets after restart:**
- Themes are applied via Settings → Apply
- Settings are saved to `config.toml`
