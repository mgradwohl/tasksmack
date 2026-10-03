# TaskSmack UI/UX Review

## Review Scope

| | |
|---|---|
| **Date** | 2026-10-03 |
| **Commit reviewed** | `main` at `789e58ab`. The static source review started at `9a51f23f`; the commits in between, #1081–#1084, are event and settings plumbing and change none of the findings. |
| **Platforms** | Windows 11 (26300). Static review of the Windows and Linux code paths. |
| **Themes** | All 20 bundled themes (`assets/themes/*.toml`) plus the built-in fallback (`Theme::loadDefaultFallbackTheme`) |
| **Font / DPI** | Live: Extra Large at 175 % (3200×2000) and 150 %; Small and Even Huger at 175 %; 900×700 window; maximized. By reasoning from the code: 100 %/Small, 100 %/Medium, 150 %/Large, 200 %/Medium. |
| **Method** | Static inspection of `src/UI` and `src/App`, plus a WCAG contrast, CIEDE2000 and colour-vision simulation (Machado 2009) of every theme's colour roles against the background each is drawn on, plus a driven tour of the running `win-debug` build. |

The tour captured:
- **Every view:** Overview (scrolled), CPU Cores, GPU, Network and I/O, Processes (list, tree, sorted, filtered, selected row), Process Details (Overview, GPU, Network and I/O, Actions), Settings and About.
- **Every theme:** System Overview, Processes with a selected row, and Process Details.

**Limitations.**
- The Linux rendering was reasoned from the code, not run.
- The machine has no discrete GPU, so GPU sensor charts (temperature, power, fan, clock) and multi-GPU layouts are reviewed statically only.
- Win+Up and snap gestures couldn't be sent; UI-020 is reproduced with `ShowWindow(SW_MAXIMIZE)`.
- Colour-blind findings are simulations, not user testing.
- The 100 % and 200 % combinations weren't seen on screen.

## Overall UI Assessment

TaskSmack's structure is sound: a consistent tab hierarchy, a shared chart widget (`HistoryChart` + NowBars), scaled spacing (#942/#980/#1055), and 20 carefully authored themes. The recurring problems are systemic, not cosmetic. Most come from a few shared mechanisms, so a handful of focused changes would lift the whole app.

1. **State and selection are hard to see.**
   - The selected process row is near-invisible in 14 themes.
   - The selected tab is barely distinguishable in 17, because the overline every theme defines is never drawn.
2. **"What is happening now?" needs a hover.**
   - NowBars carry no value or label.
   - Section headers show sample counts instead of readings.
   - Process charts are scaled to the whole machine, so typical processes draw flat lines.
3. **Colour carries meaning it can't reliably carry.**
   - Charts borrow each other's colour fields.
   - Data series reuse the error and warning colours.
   - Pairs on the same chart collide or collapse for colour-blind users.
   - Many series and the axis text fall below contrast floors, especially in light themes.
4. **Numbers aren't typeset for a monitor.**
   - Proportional digits jitter.
   - Units and precision differ between values, axes and tooltips.
   - Axis ticks fall on odd steps (1.9 MB/s, 95.4 MB/s).
   - Chart text renders at about 8 px at the default size.
5. **Terminology follows the implementation (htop headers, Unix nice on Windows) rather than one user-facing vocabulary.**

None of this needs a redesign. The app's technical, information-dense character should stay. The fixes are shared helpers (section header, number grammar, current-value strip, semantic colour roles) and theme data, guarded by a bundled-theme lint test.

## High-Impact Findings

### UI-001 — Selected process row and selected tab are nearly invisible
- **Screen:** Processes table; all three tab bars.
- **Themes:** row in 14 of 20 themes (Ubuntu Light 1.01:1 … Cyberpunk Light 1.27); tab in 17 of 20.
- **Source:** `ProcessesPanel.cpp` `Selectable(SpanAllColumns)` ~L1029 → `ui.header.normal`, often at 50 % alpha. `BeginTabBar` without `ImGuiTabBarFlags_DrawSelectedOverline` at `ShellLayer.cpp:366`, `SystemMetricsPanel.cpp:404` and `ProcessDetailsPanel.cpp:338`.
- **Current:** the selection fill differs from a normal row by 1.01–1.27:1, and hover looks like selection. In 9 themes the unselected tabs are the filled ones. Confirmed on screen in all 20 themes.
- **Why:** selection decides which process Terminate, Kill and Priority act on.
- **Change:** an opaque accent tint ≥ 1.35:1, plus an optional accent edge; pass the overline flag. Candidate colours are per theme below.
- **Scope:** Small · **Issue:** [#1190](https://github.com/mgradwohl/tasksmack/issues/1190)

### UI-002 — Chart series, NowBars and grid lines below visible contrast
- **Screen:** every chart.
- **Themes:** 18 of 20 have at least one series under 3:1 (Latte 18 of 29; Cyberpunk Light `charts.gpu.memory` 1.10; Dracula `charts.gpu.fan` 1.00, the plot colour itself). The grid is 1.01:1 on Tokyo Night.
- **Source:** TOML `charts.*`, `progress.*`, `accents`; `Theme.cpp:491` (grid = window border at 50 % alpha).
- **Why:** lines and bars vanish, and a vanished NowBar reads as zero.
- **Change:** 3:1 variants of the failing series and a dedicated `ui.plot.grid` at 1.3–1.6:1.
- **Scope:** Medium · **Issue:** [#1191](https://github.com/mgradwohl/tasksmack/issues/1191)

### UI-003 — NowBar, legend and tooltip colours don't match their series
- **Screen:** Overview CPU, Process Details CPU, CPU Cores, and every chart tooltip.
- **Themes:** all.
- **Source:** the Total NowBar uses `progressColor` while the line uses `chartCpu`; `charts.cpu == cpu_breakdown.user` in all 20 themes; tooltip rows use `TextColored(seriesColour)` (`ChartWidgets.h:217`); band legends use the 35 % fills.
- **Current:**
  - The bar that matches the blue Total line is actually "User".
  - The tooltip and Process Details' legend show User and Total in the same blue.
  - Tooltip text falls under 4.5:1 for most series in light themes; the Idle row is 1.2–2.0:1.
- **Change:**
  - Draw the Total bar in `chartCpu` and give User its own colour.
  - Tooltip rows become a swatch plus normal text.
  - Band legend swatches become opaque.
- **Scope:** Small · **Issue:** [#1192](https://github.com/mgradwohl/tasksmack/issues/1192)

### UI-004 — Current values are hover-only
- **Screen:** every history chart.
- **Source:** `renderHistoryWithNowBars` passes `""` for both label and value (`ChartWidgets.h` L1541/L1596). This is a long-standing design, not a regression.
- **Current:** to read "CPU 23 %" you hover a 2.25 em bar. Only Power & Battery prints a live value.
- **Change:** values in the legend (stable `##` IDs) or a header strip, with the same precision as the tooltip.
- **Scope:** Medium · **Issue:** [#1193](https://github.com/mgradwohl/tasksmack/issues/1193)

### UI-005 — Chart text renders at the Small preset size, about 8 px at the default font
- **Screen:** every chart's ticks, legend and hints.
- **Source:** `PlotFontGuard` pushes `smallerFont()`, and Medium maps to Small at 6 pt (`Theme.cpp` L678–690).
- **Change:** a chart-text floor of about 10 px at 1.0 scale.
- **Scope:** Small · **Issue:** [#1194](https://github.com/mgradwohl/tasksmack/issues/1194)

### UI-006 — Process CPU and memory charts are flat for typical processes
- **Screen:** Process Details → Overview.
- **Source:** `percentHistoryConfig` (fixed 0–100 % of the machine), `ProcessDetailsPanel.cpp` ~L841/~L1028; integer `percentCompact`.
- **Current:** TaskSmack at 0.8 % draws a flat line in all 20 themes, and the pane shows "0 %" while the table shows 0.6 %.
- **Change:** an eased auto-scale with a minimum span, memory in bytes, and 1 dp below 10 %.
- **Scope:** Small–Medium · **Issue:** [#1195](https://github.com/mgradwohl/tasksmack/issues/1195)

### UI-007 — Text roles below WCAG AA where they're drawn
- **Screen:** chart axes, hints, status and notice text.
- **Themes:** muted text on the plot falls below 4.5:1 in 12 themes (Dracula 1.94, Latte 2.07, Solarized Light 2.18). Solarized Light primary text is 4.13. Warning, error and success fail on most light themes and Nord. Muted is lighter than disabled in 5 light themes.
- **Source:** TOML `semantic.text_*`; `Theme.cpp:489–490` (axis text = muted, on `ui.frame.background`).
- **Change:** candidate values per theme (see Theme Review).
- **Scope:** Small · **Issue:** existing [#1167](https://github.com/mgradwohl/tasksmack/issues/1167) (extended in a comment).

## Medium-Impact Findings

### UI-008 — No semantic colour roles; metrics borrow each other's colours
- **Source:**
  - Cached = `chartCpu`, Swap = `chartIo`, system Power = `chartCpu`, Battery = `chartMemory`.
  - Threads, Handles and Page Faults = CPU, memory and `accentColor(3)`.
  - Process Power = `textInfo`; GPU Power = the memory green in 19 themes.
  - Disk read = the `text_error` hex in 13 themes.
  - Network send and receive have no family across themes.
  - Monochrome loses error/warning/success; Cyberpunk's load ramp runs purple → yellow; the fallback theme is degenerate; the filter hint uses `statusRunning`.
- **Change:** add role fields (with loader fallbacks), fixed hue families, status colours never used for data, and a green → amber → red ramp everywhere.
- **Scope:** Medium · **Issue:** [#1196](https://github.com/mgradwohl/tasksmack/issues/1196)

### UI-009 — Same-chart colour collisions and colour-blind collapse
- **Current:**
  - 12 themes have two series of one chart at ΔE < 1 (e.g. GPU memory = decoder in Arctic Fire, Solarized and Windows Dark; Page Faults = Handles in Cyberpunk and Dracula).
  - 11 themes collapse pairs under protan/deutan simulation (Tokyo Night amber/green 0.9, blue/purple 0.3).
- **Change:** use the next unused palette colour, and separate pairs by ΔL* ≥ 20.
- **Scope:** Medium · **Issue:** [#1197](https://github.com/mgradwohl/tasksmack/issues/1197)

### UI-010 — Series told apart only by colour; stacked fills; legend over the data
- **Source:** `plotLineWithFill` fills by default (GPU Core up to 5 series, ≥ 3 fills); Network separates interface from total by alpha only; `setupLegendDefault` places the legend inside the plot; the Memory legend clips "Peak Used".
- **Change:** one fill per chart, line weight or dash for secondary series, and the legend outside the plot.
- **Scope:** Small–Medium · **Issue:** [#1198](https://github.com/mgradwohl/tasksmack/issues/1198)

### UI-011 — No bundled-theme colour lint
- **Source:** `test_ColorContrast.cpp` has unit cases only; `ThemeLoader` doesn't validate.
- **Change:** `test_BundledThemes` asserting the contrast and separation floors.
- **Scope:** Medium · **Issue:** [#1199](https://github.com/mgradwohl/tasksmack/issues/1199)

### UI-012 — Section headers lack hierarchy; status bar, title bar and dialog chrome are inconsistent
- **Current:**
  - Body-size headers with "(N samples)"; `Inter-Bold` is unused.
  - The status bar's "Ready" never changes, and the FPS readout overprints when narrow.
  - The title-bar buttons have no tooltips.
  - Each dialog places its footer buttons differently.
  - Spacing is ad hoc, and a few raw-pixel literals remain.
- **Change:** a `sectionHeader()` helper, live status text, and `sectionGap()`/`dialogFooter()` helpers.
- **Scope:** Small–Medium · **Issue:** [#1200](https://github.com/mgradwohl/tasksmack/issues/1200)

### UI-013 — Proportional digits jitter; decimals misaligned in mixed-unit columns
- **Source:** `Theme::monospaceFont()` has no callers; the `UNIT_*` widths are measured but unused; `AlignedNumericParts` is flattened back to a string.
- **Change:** tabular digits and three-part numeric cells.
- **Scope:** Medium · **Issue:** [#1201](https://github.com/mgradwohl/tasksmack/issues/1201)

### UI-014 — No single number, unit and duration grammar; odd axis tick steps
- **Current:**
  - "1.2 GB" in values vs "1.2GB" on axes.
  - GPU power at 0, 1 and 2 dp.
  - "Page Faults/s: 12.0/s".
  - Locale applied to values but not axes.
  - Four duration formats.
  - Ticks at 1.9 MB/s, 95.4 MB/s and 19.1 MB steps; 21 y ticks when maximized.
- **Change:** one `Format.h` grammar, 1-2-5 ticks capped by plot height, and `formatDuration`.
- **Scope:** Medium · **Issue:** [#1202](https://github.com/mgradwohl/tasksmack/issues/1202)

### UI-015 — Same concept, different names; htop-jargon headers; Settings and dialog wording
- **Current:**
  - Table headers S, RES, VIRT, SHR, TIME+, PF.
  - RES is "Used" in Details; S is "Status" in Details.
  - Suspend appears as Pause, stop and "Success: stop sent".
  - "16 cores" counts logical processors.
  - Settings: ALL CAPS sections, "Metric Refresh Rate", "Even Huger", and Apply that closes.
  - Confirm dialogs use Yes/No; destructive buttons aren't styled differently.
- **Change:** see the wording table below.
- **Scope:** Small · **Issue:** [#1203](https://github.com/mgradwohl/tasksmack/issues/1203)

### UI-016 — Unix "nice" shown on Windows
- **Source:** `ProcessDetailsPanel.cpp` ~L717, ~L2286, ~L2374–2382.
- **Current:** a 40-step slider for 6 Windows priority classes.
- **Change:** a priority-class control on Windows.
- **Scope:** Small–Medium · **Issue:** [#1204](https://github.com/mgradwohl/tasksmack/issues/1204)

### UI-017 — GPU clock, temperature and power on an unexplained % axis
- **Source:** `GpuSection.cpp` `normalizeToPercent`; 100 °C and an assumed 300 W reference. This regressed after #1008 removed the label suffix that #606 added.
- **Change:** unit-true charts, or the basis stated in the label.
- **Scope:** Small–Medium · **Issue:** [#1205](https://github.com/mgradwohl/tasksmack/issues/1205)

### UI-018 — Unidentified secondary axes; misaligned stacked charts; repeated grid-cell chrome
- **Current:**
  - Process Resources shows 0–450 and 0–250 axes with no key.
  - Battery's axis is hidden.
  - Resources reserves 4 NowBar columns vs CPU's 3, so the time axes don't align.
  - Each CPU core cell repeats "Time (s)" and its ticks.
- **Scope:** Small–Medium · **Issue:** [#1206](https://github.com/mgradwohl/tasksmack/issues/1206)

### UI-019 — Minimum window size not derived from content; dead space in large windows
- **Current:**
  - 200×200 dp minimum: the FPS readout overprints and the toolbar collides; at 900×700, "Network and …" truncates.
  - Maximized, the GPU tab leaves about 40 % of the window empty; Even Huger shows one chart.
  - The Process GPU tab repeats single-GPU data.
- **Scope:** Small–Medium · **Issue:** [#1207](https://github.com/mgradwohl/tasksmack/issues/1207)

### UI-020 — OS-level maximize leaves the window a quarter of the screen with clipped content (bug)
- **Current:** maximizing through the OS API, `ShowWindow(SW_MAXIMIZE)` (the reproduction used for OS-initiated maximize; real Win+Up and snap gestures couldn't be sent), left the window "zoomed" at 1600×783 on a 3200×2000 display, with the UI laid out larger and the title-bar buttons cut off. TaskSmack's own Maximize button works.
- **Scope:** Small–Medium · **Issue:** [#1208](https://github.com/mgradwohl/tasksmack/issues/1208)

### UI-021 — Process table affordances are hidden
- **Current:**
  - The column chooser is right-click only, with no reset.
  - The "List View" button is shown while the tree is displayed.
  - Expanders are "+"/"-" buttons.
  - Headers are centred over right-aligned numbers.
  - There's no row context menu.
  - Names truncate in Tree View while Publisher stays wide.
- **Scope:** Medium · **Issue:** [#1209](https://github.com/mgradwohl/tasksmack/issues/1209)

### UI-022 — "-" means both zero and not available; unsupported default columns; residual empty states
- **Current:**
  - On Windows, SHR and Status are entire columns of "-", and VIRT reads about 2,052 GB everywhere.
  - Five different "unavailable" wordings.
  - 8 bare-text empty states.
  - The Details "Network and I/O" tab appears mid-session.
- **Scope:** Small · **Issue:** [#1210](https://github.com/mgradwohl/tasksmack/issues/1210) (presentation half of #1110)

### UI-023 — Icon vocabulary is overloaded and inconsistently spaced
- **Current:**
  - MICROCHIP stands for both CPU and GPU; GEARS for 3 concepts; GAUGE_HIGH for 3.
  - Icon spacing mixes one and two spaces.
  - `getInterfaceTypeIcon` is dead code.
- **Scope:** Small–Medium · **Issue:** existing [#977](https://github.com/mgradwohl/tasksmack/issues/977) (plan added in a comment).

## Polish Findings

| ID | Finding | Source | Scope | Issue |
|---|---|---|---|---|
| UI-024 | Interface Status lists 13 down or virtual adapters out of 17 | `NetworkSection.cpp` | Small | [#1211](https://github.com/mgradwohl/tasksmack/issues/1211) |
| UI-025 | About credits only Inter; Font Awesome (CC BY 4.0) and Sixtyfour are missing; tagline repeats the name | `AboutLayer.cpp` | Tiny | [#1212](https://github.com/mgradwohl/tasksmack/issues/1212) |
| UI-026 | Priority badge text and slider thumb contrast | `ProcessDetailsPanel.cpp` ~L2487–2522 | Tiny | existing [#1130](https://github.com/mgradwohl/tasksmack/issues/1130) (thumb added) |
| UI-027 | Grid lines invisible (1.01:1 on Tokyo Night) | `Theme.cpp:491` | Small | part of [#1191](https://github.com/mgradwohl/tasksmack/issues/1191) |
| UI-028 | Processes filter hint drawn in the "running" green | `ProcessesPanel.cpp:567` | Tiny | part of [#1196](https://github.com/mgradwohl/tasksmack/issues/1196) |
| UI-029 | Cyberpunk load ramp is purple → yellow; fallback theme has 8 identical accents | `cyberpunk*.toml`, `Theme.cpp:44–206` | Tiny | part of [#1196](https://github.com/mgradwohl/tasksmack/issues/1196) |
| UI-030 | Stacked-band legend swatches are 35 % fills | `SystemMetricsPanel.cpp:692–715` | Small | part of [#1192](https://github.com/mgradwohl/tasksmack/issues/1192) |
| UI-031 | Per-core and per-disk cells repeat full axis chrome | `CpuCoresSection.cpp`, `StorageSection.cpp` | Small | part of [#1206](https://github.com/mgradwohl/tasksmack/issues/1206) |
| UI-032 | Dialog footers differ; ad-hoc vertical rhythm | `SettingsLayer`, `AboutLayer`, `ElevationNoticeLayer` | Small | part of [#1200](https://github.com/mgradwohl/tasksmack/issues/1200) |
| UI-033 | Binary multiples labelled KB/MB/GB; link speed in decimal Mbps beside binary MB/s | `Format.h`, `NetworkSection.cpp` | Small | part of [#1202](https://github.com/mgradwohl/tasksmack/issues/1202) |
| UI-034 | "16 cores" counts logical processors | `SystemMetricsPanel.cpp` L521 | Tiny | part of [#1203](https://github.com/mgradwohl/tasksmack/issues/1203) |
| UI-035 | Process GPU tab repeats single-GPU numbers in two blocks | `ProcessDetailsPanel.cpp` GPU tab | Tiny | part of [#1207](https://github.com/mgradwohl/tasksmack/issues/1207) |

## Theme Review

Contrast thresholds:
- **Body text:** 4.5:1.
- **Chart lines and NowBars, and any selection edge or tab overline:** 3:1.
- **Selection fills against their neighbours** (practical floors, not WCAG numbers): selected row 1.35:1; selected tab 1.3:1.
- **Series sharing a chart:** ΔE2000 ≥ 12, and ≥ 8 under protan/deutan simulation.

Each theme's colours are judged against the background they're drawn on; for example, axis labels are measured on the plot frame, not the window. Ratios and candidate replacement hex values per theme follow.

*Screenshot names below (`theme-<id>.png`, `tour/…`) refer to the review's capture set, which is not committed.*

### Arctic Fire (`arctic-fire.toml`, dark)
**Default theme** (Theme.cpp:253 prefers `arctic-fire`), so its issues reach the most users.
Effective backgrounds: window #141A24, plot/frame #1F2938, popup #191E27, legend #1A202A, selected row #334D73, grid #293549.
Key ratios: primary/window 15.82, primary/plot 13.28, muted/plot 10.18, disabled/window 10.32, warning/window 9.73, error/popup 3.96, grid 1.18, selected row 2.04, selected tab vs tab 1.45.
- **Strengths:** All four text roles are 10-16:1; 19 of 21 series are at least 3:1 on the plot (#1F2938); hue families are clean (CPU blue, memory green, read orange, write cyan).
- **Weak combinations / concerns:**
  - `text_muted` #D0D8E0 (12.1:1) is almost `text_primary` #F0F4FA (15.8:1): muted carries no hierarchy (#1167).
  - `text_error` #E53935 is 4.13 on the window, 3.47 on frames, 3.96 in popups.
  - `charts.gpu.memory` and `charts.gpu.decoder` are both #EC407A on the same GPU chart (GpuSection.cpp:304/352).
  - `charts.peak_line` #FFD54FB3 vs Swap (`charts.io` #FF7043): deuteranopia dE 2.0 on the Memory chart.
  - Priority badge text #FFFFFF on `priority.normal` #00E676 is 1.67:1 (#1130).
- **Seen in screenshot:** theme-arctic-fire.png: User/System legend swatches (35 % fills) are dull navy/brown blocks; the green CPU Total bar matches nothing in the legend.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_error` | #E53935 | 3.96 | #F1473F | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.gpu.fan` | #546E7A | 2.71 | #5C7582 | >= 3.0 vs plot / NowBar track |
| `ui.window.border (grid)` | #33405980 | 1.18 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Arctic Fire Light (`arctic-fire-light.toml`, light)
Effective backgrounds: window #F4F7FB, plot/frame #EDF2F8, popup #E9ECF1, legend #F1F5FA, selected row #D0E0F0, grid #D9E3F0.
Key ratios: primary/window 17.23, primary/plot 16.45, muted/plot 2.53, disabled/window 5.34, warning/window 2.52, error/popup 4.21, grid 1.15, selected row 1.25, selected tab vs tab 1.09.
- **Strengths:** Primary text 17:1; disabled 5.3:1; distinct hue families like the dark variant.
- **Weak combinations / concerns:**
  - Muted/disabled hierarchy is inverted: `text_muted` #999999 is 2.65:1, `text_disabled` #666666 is 5.34:1, so 'disabled' reads stronger than 'secondary' text and the axis labels (muted) are the faintest text on screen.
  - `text_warning` #F57C00 2.52 on window / 2.29 popup; `text_success` #388E3C 3.83; `text_info` #1976D2 3.89 in popups.
  - 15 of 29 series under 3:1 on the plot (#EDF2F8): `charts.net_tx` #F6C445 1.45, accent6 #A8F04F 1.22, `progress.medium` #FFB300 1.59 (the CPU Total bar at 50-80 %), `progress.low` #66BB6A 2.10.
  - Selected tab #E8F0F8 vs tab #D8E8F5 1.09; grid 1.15; selected row (#D0E0F0) 1.25.
  - CVD: `charts.cpu` #1976D2 vs accent3 #7C64C8 (Threads vs Page Faults) deutan 2.8.
- **Seen in screenshot:** theme-arctic-fire-light.png: the yellow/peach legend swatches and the light Memory 'Cached' blue bar are hard to find against #EDF2F8.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #999999 | 2.53 | #6D6D6D | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_error` | #D32F2F | 4.21 | #CC292B | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #F57C00 | 2.29 | #B24B00 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #388E3C | 3.48 | #247A2B | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #1976D2 | 3.89 | #006BC4 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io_write` | #00BCD4 | 2.04 | #009AB0 | >= 3.0 vs plot / NowBar track |
| `charts.net_tx` | #F6C445 | 1.45 | #AE8603 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #F57C00 | 2.4 | #DD6B00 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #E46E97 | 2.67 | #D9658E | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #F57C00 | 2.4 | #DD6B00 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.clock` | #64B5F6 | 1.97 | #3F92CE | >= 3.0 vs plot / NowBar track |
| `progress.low` | #66BB6A | 2.1 | #4B9C51 | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #FFB300 | 1.59 | #BD7E00 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #F57C00B3 | 1.89 | #AF4900B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[1]` | #FF8A50 | 2.07 | #D96D37 | >= 3.0 vs plot / NowBar track |
| `accents.colors[2]` | #4FAF7A | 2.41 | #3D9C69 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #F6C445 | 1.45 | #AE8603 | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #E46E97 | 2.67 | #D9658E | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #A8F04F | 1.22 | #5B9C00 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #FF9E7A | 1.79 | #CA7454 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #D0E0F0 | 1.25 | #B9D1E9 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #C6D4E880 | 1.15 | #C5D3E7 | >= 1.35 vs plot |


### Cyberpunk (`cyberpunk.toml`, dark)
Effective backgrounds: window #0F0A1A, plot/frame #1F142E, popup #130E22, legend #150F25, selected row #4A3366, grid #361D4A.
Key ratios: primary/window 18.07, primary/plot 16.28, muted/plot 11.51, disabled/window 11.58, warning/window 15.78, error/popup 4.89, grid 1.2, selected row 1.81, selected tab vs tab 1.4.
- **Strengths:** Highest text contrast of the set (primary 18:1, muted 12.8:1); every series is above 3:1 on #1F142E.
- **Weak combinations / concerns:**
  - Accent3 == `charts.memory` == #76FF03: Page Faults and Handles are the same green on the Resources charts.
  - `charts.io` #FF4081 vs `charts.io_write` #F50057: dE 9.0 -- disk read and write are two pinks.
  - Progress ramp is purple #7C4DFF -> pink #FF4081 -> yellow #FFEA00: at 80-100 % CPU the Total bar turns the warning colour, not a 'critical' red.
  - CVD: `text_warning` #FFEA00 vs `text_success`/`status.running` #76FF03 protan 1.8; `charts.gpu.utilization` #D500F9 vs `charts.gpu.clock` #7C4DFF protan 1.2.
  - Priority badge text white on #76FF03 = 1.31 (#1130).
- **Seen in screenshot:** theme-cyberpunk.png: Total line cyan, Total NowBar purple (progress.low) -- the most obvious line/bar mismatch of the set.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `ui.window.border (grid)` | #4D266680 | 1.2 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Cyberpunk Light (`cyberpunk-light.toml`, light)
Effective backgrounds: window #F7F4FC, plot/frame #F0ECF8, popup #EBE5F2, legend #F3EDFB, selected row #E0D8F0, grid #E3DDF6.
Key ratios: primary/window 17.81, primary/plot 16.67, muted/plot 3.4, disabled/window 5.88, warning/window 2.49, error/popup 4.03, grid 1.13, selected row 1.27, selected tab vs tab 1.07.
- **Strengths:** Primary 17.8:1; disabled 5.9:1.
- **Weak combinations / concerns:**
  - Muted #808080 3.63 window / 3.40 plot and lighter than disabled #5E5E6A (inverted hierarchy).
  - `text_warning` #F57C00 2.49; success 3.78; info 3.72 in popups.
  - Neon accents on near-white: `charts.gpu.memory`/accent6 #FFE433 1.10, accent3 #7DFF5A 1.11, `charts.net_rx` #00D7FF 1.48 -- these lines effectively vanish.
  - Progress ramp purple -> pink -> orange (#7C4DFF/#FF4081/#FF6D00); 'low' is not green.
  - Selected tab 1.07; grid 1.13; selected row 1.27.
- **Seen in screenshot:** theme-cyberpunk-light.png: CPU Total bar purple while the Total line is blue.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #808080 | 3.4 | #6C6C6C | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_error` | #D32F2F | 4.03 | #C72428 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #F57C00 | 2.19 | #AF4900 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #388E3C | 3.33 | #217729 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #1976D2 | 3.72 | #0068C1 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io_write` | #FF7A1A | 2.24 | #E06300 | >= 3.0 vs plot / NowBar track |
| `charts.net_rx` | #00D7FF | 1.48 | #0094B7 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #F57C00 | 2.33 | #DA6800 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #FFE433 | 1.1 | #9A8900 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #F57C00 | 2.33 | #DA6800 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #F44BFF | 2.47 | #E039EC | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #FF4081 | 2.87 | #FA3B7D | >= 3.0 vs plot / NowBar track |
| `progress.high` | #FF6D00 | 2.43 | #E85D00 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #F57C00B3 | 1.85 | #AC4600B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[0]` | #FF4FA3 | 2.62 | #F24398 | >= 3.0 vs plot / NowBar track |
| `accents.colors[1]` | #00D7FF | 1.48 | #0094B7 | >= 3.0 vs plot / NowBar track |
| `accents.colors[3]` | #7DFF5A | 1.11 | #1B9F00 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #FF7A1A | 2.24 | #E06300 | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #F44BFF | 2.47 | #E039EC | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #FFE433 | 1.1 | #9A8900 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #E0D8F0 | 1.27 | #D3C7EA | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #D7CFF580 | 1.13 | #D1C9EF | >= 1.35 vs plot |


### Dracula (`dracula.toml`, dark)
Effective backgrounds: window #282A36, plot/frame #44475A, popup #20212B, legend #23242F, selected row #363948, grid #535D7F.
Key ratios: primary/window 13.36, primary/plot 8.59, muted/plot 1.94, disabled/window 3.03, warning/window 8.36, error/popup 5.09, grid 1.41, selected row 1.24, selected tab vs tab 1.11.
- **Strengths:** Iconic palette, good primary (13.4:1) and status colours on rows.
- **Weak combinations / concerns:**
  - The plot/frame background is #44475A (Dracula 'current line'), which is the same lightness as Dracula 'comment' #6272A4 used for `text_muted`: axis tick labels are **1.94:1** on the plot.
  - `charts.gpu.fan` #44475A is the plot background itself: **1.00:1**, the fan line is invisible. `charts.gpu.clock`/accent7 #6272A4 1.94.
  - Accent3 == `charts.memory` == #50FA7B (Page Faults == Handles).
  - Header is #44475A80, so the selected row is 1.24:1 against the row.
  - CVD: orange #FFB86C vs green #50FA7B deutan 3.2 -- affects warning/success, running/disk-sleep, progress low/medium, GPU temperature/power.
- **Seen in screenshot:** theme-dracula.png: axis numbers visibly fade into the plot; legend swatches for User/System barely visible.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #6272A4 | 1.94 | #A8B5E7 | >= 4.5 vs plot/frame (axis labels) |
| `charts.io` | #FF5555 | 2.91 | #FF5B5A | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.system` | #FF5555 | 2.91 | #FF5B5A | >= 3.0 vs plot / NowBar track |
| `charts.gpu.clock` | #6272A4 | 1.94 | #8492C4 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #44475A | 1.0 | #9194A7 | >= 3.0 vs plot / NowBar track |
| `progress.high` | #FF5555 | 2.91 | #FF5B5A | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #FF5555 | 2.91 | #FF5B5A | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #6272A4 | 1.94 | #8492C4 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #44475A80 | 1.24 | raise to an opaque mid-tone (see text) | >= 1.35 vs window/row |


### Gruvbox (`gruvbox.toml`, dark)
Effective backgrounds: window #282828, plot/frame #3C3836, popup #1C1F20, legend #1F2122, selected row #32302F, grid #46413E.
Key ratios: primary/window 10.75, primary/plot 8.45, muted/plot 4.17, disabled/window 4.02, warning/window 8.69, error/popup 4.82, grid 1.15, selected row 1.12, selected tab vs tab 1.27.
- **Strengths:** Warm palette with readable text (primary 10.8, muted 5.3 window).
- **Weak combinations / concerns:**
  - Two exact duplicates on the GPU chart: utilization == decoder #D3869B, memory == encoder #8EC07C.
  - Muted #A89984 4.17 on plot; selected row (#3C383680) 1.12; grid 1.15.
  - Yellow/olive collisions: `charts.memory` #B8BB26 vs accent4 #FABD2F (GDI) protan 3.1; warning #FABD2F vs success #B8BB26 protan 3.1 / deutan 6.1 (also running vs disk-sleep, progress low vs medium).
  - `charts.cpu` #83A598 vs accent3 #D3869B deutan 3.0.
- **Seen in screenshot:** theme-gruvbox.png: Total line aqua, Total bar olive, User bar aqua.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #A89984 | 4.17 | #AFA08B | >= 4.5 vs plot/frame (axis labels) |
| `ui.header.normal` | #3C383680 | 1.12 | #444240 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #50494580 | 1.15 | #524B47 | >= 1.35 vs plot |


### Gruvbox Light (`gruvbox-light.toml`, light)
Effective backgrounds: window #FBF1C7, plot/frame #EBDBB2, popup #E3D4AC, legend #EBDBB2, selected row #EBDBB2, grid #C9BA9B.
Key ratios: primary/window 10.22, primary/plot 8.45, muted/plot 3.55, disabled/window 3.24, warning/window 3.33, error/popup 5.85, grid 1.39, selected row 1.21, selected tab vs tab 1.21.
- **Strengths:** Primary 10.2:1; muted 4.29 on window.
- **Weak combinations / concerns:**
  - `text_warning` #B57614 3.33 window / 2.56 popup; success #79740E 3.30 popup.
  - `charts.io` #9D0006 vs `charts.io_write` #AF3A03 dE 10.8: disk read/write are two dark reds (cf. #367).
  - `charts.gpu.memory` == `charts.gpu.encoder` #427B58.
  - Selected row 1.21; selected tab is the window colour (1.21 vs tab).
  - Badge text #1A1A1A on `priority.high` #9D0006 = 2.02 (#1130).
- **Seen in screenshot:** theme-gruvbox-light.png: CPU NowBars olive/teal/maroon are dark and legible; legend swatches pale.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #7C6F64 | 3.55 | #6B5F55 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_warning` | #B57614 | 2.56 | #855000 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #79740E | 3.3 | #625F00 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #076678 | 4.48 | #046577 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.net_tx` | #B57614 | 2.75 | #AD700C | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #B57614 | 2.75 | #AD700C | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #B57614 | 2.75 | #AD700C | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #928374 | 2.68 | #897A6B | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #B57614 | 2.75 | #AD700C | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #B57614 | 2.75 | #AD700C | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #928374 | 2.68 | #897A6B | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #EBDBB2 | 1.21 | #E0C787 | >= 1.35 vs window/row |


### Latte (`latte.toml`, light)
Effective backgrounds: window #EFF1F5, plot/frame #CCD0DA, popup #DEE1E7, legend #E4E8EE, selected row #CCD0DA, grid #C4C8D3.
Key ratios: primary/window 7.06, primary/plot 5.17, muted/plot 2.07, disabled/window 2.3, warning/window 2.64, error/popup 4.14, grid 1.08, selected row 1.37, selected tab vs tab 1.08.
- **Strengths:** Catppuccin palette, primary 7.1:1.
- **Weak combinations / concerns:**
  - Weakest light theme overall. `text_muted` #8C8FA1 **2.07** on the plot (axis labels), 2.83 window; `text_disabled` #9CA0B0 2.30.
  - `text_warning` #FE640B 2.64 / 2.28 popup; `text_success` #40A02B 2.96 / 2.55; filter hint (status.running) 2.17 on its frame.
  - 18 of 29 series under 3:1 on the #CCD0DA plot, including `charts.memory` #40A02B 2.17 and `progress.low` 2.17 (Memory Used line and the low-load CPU bar).
  - Grid 1.08; selected tab 1.08.
  - CVD: `charts.cpu` #1E66F5 vs accent3 #8839EF (Threads vs Faults) deutan 0.9; GPU util #8839EF vs clock #1E66F5 deutan 0.9; `progress.low` vs `progress.medium` protan 1.7.
- **Seen in screenshot:** theme-latte.png: the whole plot area reads low-contrast; axis numbers are grey-on-grey.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #8C8FA1 | 2.07 | #555868 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_disabled` | #9CA0B0 | 2.3 | #878B9A | >= 3.0 vs window (disabled widgets, hints) |
| `semantic.text_error` | #D20F39 | 4.14 | #C90034 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #FE640B | 2.28 | #BA3200 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #40A02B | 2.55 | #0B7500 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #1E66F5 | 3.75 | #0059E3 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.memory` | #40A02B | 2.17 | #258612 | >= 3.0 vs plot / NowBar track |
| `charts.io_write` | #FE640B | 1.93 | #D14300 | >= 3.0 vs plot / NowBar track |
| `charts.net_rx` | #179299 | 2.43 | #008188 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #FE640B | 1.93 | #D14300 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #DF8E1D | 1.7 | #A96400 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #FE640B | 1.93 | #D14300 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.power` | #40A02B | 2.17 | #258612 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.encoder` | #209FB5 | 2.03 | #007F94 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.decoder` | #EA76CB | 1.71 | #B74C9C | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #9CA0B0 | 1.69 | #707482 | >= 3.0 vs plot / NowBar track |
| `progress.low` | #40A02B | 2.17 | #258612 | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #DF8E1D | 1.7 | #A96400 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #4C4F69B3 | 2.92 | #484C65B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[2]` | #40A02B | 2.17 | #258612 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #FE640B | 1.93 | #D14300 | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #EA76CB | 1.71 | #B74C9C | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #DF8E1D | 1.7 | #A96400 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #209FB5 | 2.03 | #007F94 | >= 3.0 vs plot / NowBar track |
| `ui.window.border (grid)` | #BCC0CC80 | 1.08 | #B0B4BF | >= 1.35 vs plot |


### Mocha (`mocha.toml`, dark)
Effective backgrounds: window #1E1E2E, plot/frame #313244, popup #171724, legend #191A27, selected row #282839, grid #3B3D4F.
Key ratios: primary/window 11.34, primary/plot 8.69, muted/plot 4.45, disabled/window 3.36, warning/window 9.27, error/popup 7.64, grid 1.17, selected row 1.13, selected tab vs tab 1.07.
- **Strengths:** Balanced dark theme: primary 11.3, muted 5.8 window / 4.45 plot.
- **Weak combinations / concerns:**
  - `charts.gpu.fan` #585B70 1.88; header #31324480 makes the selected row 1.13.
  - CVD: `charts.cpu` #89B4FA vs accent3 #CBA6F7 protan 1.0 (Threads vs Page Faults); GPU util vs clock the same pair.
  - Peach #FAB387 vs green #A6E3A1 deutan 5.2 (warning/success, progress low/medium).
  - `text_disabled` #6C7086 3.36.
- **Seen in screenshot:** theme-mocha.png: pastel bars clear; User legend swatch dim.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #9399B2 | 4.45 | #949AB3 | >= 4.5 vs plot/frame (axis labels) |
| `charts.gpu.fan` | #585B70 | 1.88 | #787B90 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #31324480 | 1.13 | #383951 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #45475A80 | 1.17 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Monochrome (`monochrome.toml`, dark)
Single-hue (green) by design; the hue choice is taste, but meaning must still survive it.
Effective backgrounds: window #0A140A, plot/frame #142414, popup #0E190E, legend #0F1B0F, selected row #2A4A2A, grid #1A321A.
Key ratios: primary/window 18.14, primary/plot 15.68, muted/plot 12.25, disabled/window 11.68, warning/window 9.34, error/popup 14.68, grid 1.17, selected row 1.88, selected tab vs tab 1.28.
- **Strengths:** Excellent text contrast (primary 18:1).
- **Weak combinations / concerns:**
  - Semantic colours collapse: `text_error` #B9F6CA (pale mint, the *lightest* colour), `text_warning` #81C784, `text_success` #4CAF50, `text_info` #66BB6A -- success vs info dE 4.8, warning vs info 5.0. An error looks like ordinary highlighted text.
  - `progress.high` #B9F6CA is the palest green: high load reads as 'calm'.
  - Every multi-series chart has pairs under dE 10: CPU user #81C784 vs system #4CAF50 9.7; accent4 == memory #4CAF50 (GDI == Handles); accent3 vs memory 4.6; GPU util/clock/encoder 7.1-7.7.
  - `status.running` #4CAF50 vs `status.stopped` #66BB6A 4.8.
- **Seen in screenshot:** theme-monochrome.png: three NowBars in three near-identical greens; legend entries are distinguishable only by order.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `charts.net_rx` | #1B5E20 | 2.07 | #3A783A | >= 3.0 vs plot / NowBar track |
| `charts.gpu.decoder` | #1B5E20 | 2.07 | #3A783A | >= 3.0 vs plot / NowBar track |
| `accents.colors[0]` | #1B5E20 | 2.07 | #3A783A | >= 3.0 vs plot / NowBar track |
| `ui.window.border (grid)` | #1F401F80 | 1.17 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Monochrome Light (`monochrome-light.toml`, light)
Single-hue light variant.
Effective backgrounds: window #F7FBF7, plot/frame #EEF7EB, popup #EBF0EB, legend #F3F9F3, selected row #E8F5E0, grid #DEEFDD.
Key ratios: primary/window 17.17, primary/plot 16.35, muted/plot 2.6, disabled/window 5.45, warning/window 4.91, error/popup 6.82, grid 1.09, selected row 1.07, selected tab vs tab 1.04.
- **Strengths:** Primary 17:1.
- **Weak combinations / concerns:**
  - Semantic collapse as in Monochrome: error #1B5E20, warning #2E7D32, success #4CAF50, info #388E3C (error vs warning dE 10.5; warning vs info 6.2).
  - Muted #999999 2.73 and lighter than disabled #5A6B5A (inverted).
  - `charts.net_rx` #205E24 vs `charts.io` #1B5E20: dE **0.9** on the same Network and I/O tab; memory vs accent4 dE 1.2.
  - Selected row 1.07; selected tab #F5F5F5 vs tab #E8F5E0 1.04, and `active_overline` is #00000000 (no selected-tab cue at all -- moot today, see UI-001).
  - 15 series under 3:1, `charts.net_tx` #B6E8B6 1.26.
- **Seen in screenshot:** theme-monochrome-light.png: CPU Total bar and Total line are different greens; nothing distinguishes the tabs.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #999999 | 2.6 | #707070 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_warning` | #2E7D32 | 4.44 | #2D7C31 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #4CAF50 | 2.41 | #177D27 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #388E3C | 3.57 | #267D2E | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.memory` | #4CAF50 | 2.53 | #3DA044 | >= 3.0 vs plot / NowBar track |
| `charts.io_write` | #00BCD4 | 2.09 | #009BB1 | >= 3.0 vs plot / NowBar track |
| `charts.net_tx` | #B6E8B6 | 1.26 | #6C976D | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.system` | #4CAF50 | 2.53 | #3DA044 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.utilization` | #66BB6A | 2.16 | #4C9E52 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #4CAF50 | 2.53 | #3DA044 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.encoder` | #B6E8B6 | 1.26 | #6C976D | >= 3.0 vs plot / NowBar track |
| `charts.gpu.clock` | #8BD88B | 1.56 | #579D59 | >= 3.0 vs plot / NowBar track |
| `progress.low` | #4CAF50 | 2.53 | #3DA044 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #4CAF50B3 | 1.89 | #0E7722B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[3]` | #49A14A | 2.95 | #48A049 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #55B255 | 2.43 | #44A046 | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #6CC46C | 1.96 | #4A9E4D | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #8BD88B | 1.56 | #579D59 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #B6E8B6 | 1.26 | #6C976D | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #E8F5E0 | 1.08 | #B3DE99 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #CFE7CF80 | 1.09 | #C3DAC3 | >= 1.35 vs plot |


### Nord (`nord.toml`, dark)
Effective backgrounds: window #2E3440, plot/frame #3B4252, popup #393F4F, legend #3B4252, selected row #353B49, grid #444C5E.
Key ratios: primary/window 10.84, primary/plot 8.73, muted/plot 7.45, disabled/window 9.25, warning/window 8.0, error/popup 2.56, grid 1.17, selected row 1.11, selected tab vs tab 1.36.
- **Strengths:** Good text (primary 10.8, muted 9.3).
- **Weak combinations / concerns:**
  - Nord red #BF616A is mid-dark: `text_error` 3.05 window / 2.56 popup; as `charts.io`, `cpu_breakdown.system`, `progress.high` and accent1 it is 2.46 on the #3B4252 plot.
  - `charts.gpu.fan` #4C566A 1.36; selected row 1.11.
  - `charts.net_rx` is Nord yellow #EBCB8B == `text_warning`; `charts.net_tx` #81A1C1 is blue (send/receive hues swapped relative to most themes).
- **Seen in screenshot:** theme-nord.png: red Swap/System bars are dark against the plot.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_error` | #BF616A | 2.56 | #EE8F96 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io` | #BF616A | 2.46 | #D07179 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.system` | #BF616A | 2.46 | #D07179 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.decoder` | #BF616A | 2.46 | #D07179 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #4C566A | 1.36 | #838DA1 | >= 3.0 vs plot / NowBar track |
| `progress.high` | #BF616A | 2.46 | #D07179 | >= 3.0 vs plot / NowBar track |
| `accents.colors[1]` | #BF616A | 2.46 | #D07179 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #5E81AC | 2.5 | #6D8FBA | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #3B425280 | 1.11 | #454D5F | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #4C566A80 | 1.17 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Nord Light (`nord-light.toml`, light)
Effective backgrounds: window #ECEFF4, plot/frame #E5E9F0, popup #DDE1E8, legend #E5E9F0, selected row #D8DEE9, grid #CAD2DC.
Key ratios: primary/window 10.84, primary/plot 10.26, muted/plot 6.06, disabled/window 6.4, warning/window 2.47, error/popup 3.12, grid 1.25, selected row 1.17, selected tab vs tab 1.06.
- **Strengths:** Muted text strongest of the light themes (6.4:1).
- **Weak combinations / concerns:**
  - All semantic text is under 4.5: error #BF616A 3.55, warning #D08770 2.47, success #4F8A3C 3.62, info #5E81AC 3.50 (popups 2.2-3.1).
  - `progress.medium` #D08770 2.34 on plot; selected row 1.17; selected tab 1.06.
  - warning #D08770 vs error #BF616A dE 15.1 / deutan 10.2 -- the closest error/warning pair outside Monochrome.
- **Seen in screenshot:** theme-nord-light.png: legible overall; grid lines visible (best light-theme grid).
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_error` | #BF616A | 3.12 | #9F4751 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #D08770 | 2.17 | #925340 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #4F8A3C | 3.18 | #377026 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #5E81AC | 3.07 | #43658D | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.net_tx` | #6D8EB8 | 2.78 | #6788B1 | >= 3.0 vs plot / NowBar track |
| `charts.net_rx` | #C97710 | 2.81 | #C37208 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #C97710 | 2.81 | #C37208 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #C97710 | 2.81 | #C37208 | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #D08770 | 2.34 | #B9745E | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #C97710 | 2.81 | #C37208 | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #6D8EB8 | 2.78 | #6788B1 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #D8DEE9 | 1.17 | #BEC8DA | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #B0BCC880 | 1.25 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Solarized Dark (`solarized-dark.toml`, dark)
Canonical Solarized values; the base palette is mid-lightness by design.
Effective backgrounds: window #002B36, plot/frame #073642, popup #07343F, legend #073642, selected row #04313C, grid #30525C.
Key ratios: primary/window 4.75, primary/plot 4.11, muted/plot 2.42, disabled/window 2.79, warning/window 4.68, error/popup 2.89, grid 1.54, selected row 1.07, selected tab vs tab 1.15.
- **Strengths:** Recognisable, consistent hue families.
- **Weak combinations / concerns:**
  - `text_primary` #839496 is 4.75 on the window but **4.11** on frames/plots and 4.42 on the selected row -- body text below AA in tables and charts.
  - `text_muted`/`text_disabled` #586E75 2.79 window / 2.42 plot; `text_error` #DC322F 3.25 / 2.89 popup.
  - 13 series between 2.4 and 2.97 on the #073642 plot (red, orange, magenta, violet accents).
  - `charts.gpu.memory` == `charts.gpu.decoder` #D33682; selected row 1.07.
  - CVD: yellow #B58900 vs green #859900 deutan 1.7 (warning/success, running/disk-sleep, progress low/medium, GPU temperature/power, faults/handles).
- **Seen in screenshot:** theme-solarized-dark.png: the whole UI is visibly dimmer than every other dark theme.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_primary` | #839496 | 4.11 | #8B9C9E | >= 4.5 vs plot/frame |
| `semantic.text_muted` | #586E75 | 2.42 | #869CA3 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_disabled` | #586E75 | 2.79 | #5D737A | >= 3.0 vs window (disabled widgets, hints) |
| `semantic.text_error` | #DC322F | 2.89 | #FF6153 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #B58900 | 4.17 | #BC8F10 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #859900 | 4.18 | #8CA011 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #268BD2 | 3.64 | #459CE3 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io` | #DC322F | 2.81 | #E23934 | >= 3.0 vs plot / NowBar track |
| `charts.io_write` | #CB4B16 | 2.82 | #D1511C | >= 3.0 vs plot / NowBar track |
| `charts.net_rx` | #6C71C4 | 2.97 | #6D72C5 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.system` | #DC322F | 2.81 | #E23934 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.utilization` | #6C71C4 | 2.97 | #6D72C5 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #D33682 | 2.86 | #D73B86 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.decoder` | #D33682 | 2.86 | #D73B86 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.fan` | #586E75 | 2.42 | #677D84 | >= 3.0 vs plot / NowBar track |
| `progress.high` | #DC322F | 2.81 | #E23934 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #CB4B16 | 2.82 | #D1511C | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #6C71C4 | 2.97 | #6D72C5 | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #D33682 | 2.86 | #D73B86 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #DC322F | 2.81 | #E23934 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #07364280 | 1.07 | #054859 | >= 1.35 vs window/row |


### Solarized Light (`solarized-light.toml`, light)
Effective backgrounds: window #FDF6E3, plot/frame #EEE8D5, popup #E6E0CE, legend #EEE8D5, selected row #EEE8D5, grid #D9D3C0.
Key ratios: primary/window 4.13, primary/plot 3.64, muted/plot 2.18, disabled/window 2.48, warning/window 2.98, error/popup 3.51, grid 1.22, selected row 1.14, selected tab vs tab 1.14.
- **Strengths:** Strong success green (#4A6600 6.1).
- **Weak combinations / concerns:**
  - `text_primary` #657B83 is **4.13** on the window and 3.64 on the plot: primary text fails AA everywhere.
  - Muted/disabled #93A1A1 2.48 / 2.18 plot; warning #B58900 2.98; error 3.51 popup.
  - `charts.io` #C0291F vs `charts.io_write` #B03B0E dE 7.2; `charts.gpu.memory` == decoder #B02662.
  - Selected row 1.14.
- **Seen in screenshot:** theme-solarized-light.png: low-contrast grey text throughout, axis numbers faint.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_primary` | #657B83 | 3.64 | #576C74 | >= 4.5 vs plot/frame |
| `semantic.text_muted` | #93A1A1 | 2.18 | #5F6B6B | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_disabled` | #93A1A1 | 2.48 | #849191 | >= 3.0 vs window (disabled widgets, hints) |
| `semantic.text_error` | #DC322F | 3.51 | #C41A21 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_warning` | #B58900 | 2.43 | #815E00 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #2075BA | 3.69 | #0067AA | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.gpu.fan` | #93A1A1 | 2.18 | #7B8888 | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #B58900 | 2.62 | #A97F00 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #657B83B3 | 2.33 | #4A5E66B3 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #EEE8D5 | 1.14 | #DBCEA6 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #C5BEAB80 | 1.22 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Tokyo Night (`tokyo-night.toml`, dark)
The user's current theme.
Effective backgrounds: window #1A1B26, plot/frame #292E42, popup #15151D, legend #171720, selected row #222534, grid #292F40.
Key ratios: primary/window 10.59, primary/plot 8.32, muted/plot 6.36, disabled/window 2.76, warning/window 8.55, error/popup 6.84, grid 1.01, selected row 1.12, selected tab vs tab 1.05.
- **Strengths:** Strong text (primary 10.6, muted 8.1); all series but fan above 3:1 on #292E42.
- **Weak combinations / concerns:**
  - Grid lines (`ui.window.border` #29303E80 over #292E42) are **1.01:1** -- invisible.
  - `text_disabled` #565F89 2.76; `charts.gpu.fan` #565F89 2.17; selected row (#292E4280) 1.12; the selected tab is the window colour #1A1B26 and unselected tabs are darker #16161E (1.05) -- and the overline that the theme sets (#7AA2F7) is never drawn (F9).
  - `charts.gpu.utilization` == `charts.gpu.decoder` #BB9AF7.
  - CVD is the weakest of the dark themes: amber #E0AF68 vs green #9ECE6A **deutan 0.9** (warning vs success, disk-sleep vs running, progress medium vs low, GPU temperature vs power, GDI vs Handles); blue #7AA2F7 vs purple #BB9AF7 **protan 0.3** (Threads vs Page Faults, GPU clock vs util); `charts.net_tx` #E0AF68 vs `charts.io_write` #FF9E64 deutan 1.4 on the same tab.
- **Seen in screenshot:** theme-tokyo-night.png, tour/tokyo-0*.png: green CPU Total bar beside a blue Total line and a blue *User* bar; Memory 'Swap' drawn in the error pink #F7768E; Battery, Memory Used and Handles all the same green; in tour/tokyo-23-maximized.png the CPU tooltip lists Total and User in the same blue and Idle in near-invisible grey.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_disabled` | #565F89 | 2.76 | #5C658F | >= 3.0 vs window (disabled widgets, hints) |
| `charts.gpu.fan` | #565F89 | 2.17 | #6D75A0 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #292E4280 | 1.12 | #33374E | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #29303E80 | 1.01 | #3C4351 | >= 1.35 vs plot |


### Ubuntu Dark (`ubuntu-dark.toml`, dark)
Effective backgrounds: window #242424, plot/frame #303030, popup #2B2B2B, legend #2D2D2D, selected row #3D3D3D, grid #373737.
Key ratios: primary/window 14.62, primary/plot 12.43, muted/plot 8.22, disabled/window 8.53, warning/window 7.82, error/popup 3.45, grid 1.1, selected row 1.43, selected tab vs tab 1.22.
- **Strengths:** Clean Yaru-like palette; primary 14.6, muted 9.7.
- **Weak combinations / concerns:**
  - `text_error` #ED333B 3.80 / 3.45 popup.
  - `charts.gpu.utilization` #9141AC 2.23; accent3 #8E44AD 2.25 (Page Faults), accent7 #C7162B 2.25.
  - `charts.net_tx` #E95420 vs `charts.io` #ED333B dE 11.6 on the Network and I/O tab (Ubuntu orange vs red).
- **Seen in screenshot:** theme-ubuntu-dark.png: readable; plot and frame share #303030 so the NowBar track has no edge against the plot frame (taste).
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_error` | #ED333B | 3.45 | #FF5653 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.gpu.utilization` | #9141AC | 2.23 | #A858C2 | >= 3.0 vs plot / NowBar track |
| `progress.high` | #E01B24 | 2.73 | #E92B2B | >= 3.0 vs plot / NowBar track |
| `accents.colors[3]` | #8E44AD | 2.25 | #A35AC1 | >= 3.0 vs plot / NowBar track |
| `accents.colors[7]` | #C7162B | 2.25 | #DF393E | >= 3.0 vs plot / NowBar track |
| `ui.window.border (grid)` | #3D3D3D80 | 1.1 | #444444 | >= 1.35 vs plot |


### Ubuntu Light (`ubuntu-light.toml`, light)
Effective backgrounds: window #FBF8F4, plot/frame #FFF7F0, popup #EFE9E2, legend #F8F1EA, selected row #FAFAFA, grid #EBE3DA.
Key ratios: primary/window 17.84, primary/plot 17.82, muted/plot 3.34, disabled/window 5.03, warning/window 3.27, error/popup 5.06, grid 1.2, selected row 1.01, selected tab vs tab 1.14.
- **Strengths:** Primary 17.8:1.
- **Weak combinations / concerns:**
  - `ui.header.normal` #FAFAFA on window #FBF8F4: the selected process row is **1.01:1** -- invisible.
  - `progress.low` #77DD77 1.60 on the plot: the CPU Total bar at < 50 % nearly disappears; `charts.peak_line` #E5A50AB3 1.65; accent6 #FFC533 1.49.
  - Muted #888888 3.35; warning #E66100 3.27; success #26A269 3.07.
  - Plot #FFF7F0 is lighter than the window (#FBF8F4): frame vs window 1.00.
- **Seen in screenshot:** theme-ubuntu-light.png: the low-load CPU bar is a faint green sliver.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #888888 | 3.34 | #727272 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_warning` | #E66100 | 2.86 | #BA4100 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_success` | #26A269 | 2.7 | #007946 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #1C71D8 | 3.95 | #0067CC | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io_write` | #00BCD4 | 2.17 | #009EB4 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.memory` | #F07F1E | 2.55 | #DF720F | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #E5A50A | 2.04 | #BE8500 | >= 3.0 vs plot / NowBar track |
| `progress.low` | #77DD77 | 1.6 | #43A348 | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #FF9800 | 2.03 | #D57800 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #E5A50AB3 | 1.65 | #8B5C00B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[1]` | #1CA9E8 | 2.51 | #0099D6 | >= 3.0 vs plot / NowBar track |
| `accents.colors[2]` | #6FD86F | 1.69 | #3FA345 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #F07F1E | 2.55 | #DF720F | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #FFC533 | 1.49 | #B78800 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #FAFAFA | 1.01 | #D0D0D0 | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #D7CFC480 | 1.2 | raise to an opaque mid-tone (see text) | >= 1.35 vs plot |


### Windows Dark (`windows-dark.toml`, dark)
Effective backgrounds: window #202020, plot/frame #2B2B2B, popup #292929, legend #2B2B2B, selected row #383838, grid #353535.
Key ratios: primary/window 14.94, primary/plot 12.99, muted/plot 8.82, disabled/window 8.96, warning/window 5.23, error/popup 5.07, grid 1.15, selected row 1.39, selected tab vs tab 1.21.
- **Strengths:** Best-balanced theme: every text role >= 5.2, all series but fan >= 3:1.
- **Weak combinations / concerns:**
  - `charts.net_rx` #00B7C3 vs `charts.io_write` #26C6DA dE 5.0 on the same tab.
  - `charts.gpu.memory` == `charts.gpu.decoder` #E3008C; `charts.gpu.fan` #5F5F5F 2.22; grid 1.15.
- **Seen in screenshot:** theme-windows-dark.png: clean; User/System legend swatches dim.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `charts.gpu.fan` | #5F5F5F | 2.22 | #747474 | >= 3.0 vs plot / NowBar track |
| `ui.window.border (grid)` | #3F3F3F80 | 1.15 | #404040 | >= 1.35 vs plot |


### Windows Light (`windows-light.toml`, light)
Effective backgrounds: window #F5F7FA, plot/frame #F7FAFF, popup #E9ECF1, legend #F1F5FA, selected row #F3F3F3, grid #EAEEF5.
Key ratios: primary/window 17.86, primary/plot 18.32, muted/plot 3.3, disabled/window 5.35, warning/window 4.19, error/popup 5.13, grid 1.11, selected row 1.03, selected tab vs tab 1.11.
- **Strengths:** Primary 17.9; error 5.7; success 5.0.
- **Weak combinations / concerns:**
  - `ui.header.normal` #F3F3F3 on #F5F7FA: selected row **1.03:1**.
  - `cpu_breakdown.iowait`/`progress.medium` #FFB900 1.65 (CPU Total bar at 50-80 %), accent6 #FFC228 1.54.
  - `charts.net_rx` #00B5C8 vs `charts.io_write` #00BCD4 dE **2.5** on the same tab -- receive and disk-write are the same cyan.
  - Muted #8A8A8A 3.22; warning #CA5010 4.19 / 3.81 popup.
- **Seen in screenshot:** theme-windows-light.png: readable; Memory Swap and Used bars clear.
- **Candidate replacements** (hue/chroma kept, lightness moved until the target is met; the review's analysis scripts):

| key | current | ratio now | candidate | target |
|---|---|---|---|---|
| `semantic.text_muted` | #8A8A8A | 3.3 | #737373 | >= 4.5 vs plot/frame (axis labels) |
| `semantic.text_warning` | #CA5010 | 3.81 | #B94401 | >= 4.5 vs popup (darkest/lightest text bg) |
| `semantic.text_info` | #0078D4 | 3.83 | #006CC5 | >= 4.5 vs popup (darkest/lightest text bg) |
| `charts.io_write` | #00BCD4 | 2.2 | #009FB6 | >= 3.0 vs plot / NowBar track |
| `charts.net_tx` | #FA7A1C | 2.55 | #E96D0C | >= 3.0 vs plot / NowBar track |
| `charts.net_rx` | #00B5C8 | 2.38 | #00A0B3 | >= 3.0 vs plot / NowBar track |
| `cpu_breakdown.iowait` | #FFB900 | 1.65 | #C18600 | >= 3.0 vs plot / NowBar track |
| `charts.gpu.temperature` | #F7630C | 2.98 | #F5620A | >= 3.0 vs plot / NowBar track |
| `progress.medium` | #FFB900 | 1.65 | #C18600 | >= 3.0 vs plot / NowBar track |
| `charts.peak_line` | #F7630CB3 | 2.21 | #C84100B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[4]` | #FA7A1C | 2.55 | #E96D0C | >= 3.0 vs plot / NowBar track |
| `accents.colors[5]` | #00B5C8 | 2.38 | #00A0B3 | >= 3.0 vs plot / NowBar track |
| `accents.colors[6]` | #FFC228 | 1.54 | #BA8800 | >= 3.0 vs plot / NowBar track |
| `ui.header.normal` | #F3F3F3 | 1.03 | #CECECE | >= 1.35 vs window/row |
| `ui.window.border (grid)` | #DDE3EB80 | 1.11 | #D3D9E1 | >= 1.35 vs plot |


### Fallback (built-in, `Theme::loadDefaultFallbackTheme`, Theme.cpp:44-206)
Only reached when no theme TOML loads, so impact is low, but it is the safety net:
- All eight accents are the same #4296FA (Theme.cpp:60), so Page Faults, GDI, the network interface icons and the About text are identical to `charts.cpu`; `chartNetTx` == `chartCpu`, `chartNetRx` == `chartMemory` (Theme.cpp:185-186).
- `statusZombie` == `statusStopped` == #FF0000 (Theme.cpp:74/77).
- Progress ramp is blue -> grey (#808080) -> red (Theme.cpp:61-63): no amber step, and 'low' is the CPU blue, so the Total bar is indistinguishable from the Total line only at low load.
- Muted #808080 3.18 on its plot (#22344E -- frame is semi-transparent blue).
- Suggested: give accents the Arctic Fire set, set `progressLow/Medium` to #00E676/#FFB300, `statusStopped` to #FF7043.

## Color System

### Current state

The table gives each theme's hue family for each semantic key. Bold marks the outliers.

| theme | CPU | memory | disk read | disk write | net send | net recv | GPU util | GPU mem | GPU power | GPU temp | progress lo/mid/hi | error | warning |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| arctic-fire | blue | green | red-orange | cyan | yellow | magenta | magenta | pink-red | green | yellow | green/yellow/red | red | yellow |
| arctic-fire-light | blue | green | red | cyan | yellow | purple | purple | pink | green | orange | green/yellow/red | red | orange |
| cyberpunk | cyan | green | pink | pink | orange | purple | magenta | yellow | green | yellow | **purple/pink/yellow** | red | yellow |
| cyberpunk-light | blue | green | red | orange | **purple** | **cyan** | purple | yellow | green | orange | **purple/pink/orange** | red | orange |
| dracula | cyan | green | red | magenta | orange | purple | purple | yellow | green | orange | green/orange/red | red | orange |
| gruvbox | aqua | yellow-green | red | orange | yellow | pink | pink | green | yellow-green | yellow | yellow-green/yellow/red | red | yellow |
| gruvbox-light | teal | olive | dark red | orange | orange | magenta | magenta | green | olive | orange | olive/orange/red | red | orange |
| latte | blue | green | red | orange | **purple** | **teal** | purple | orange | green | orange | green/orange/red | red | orange |
| mocha | blue | green | pink-red | peach | **purple** | **teal** | purple | yellow | green | peach | green/peach/pink | pink-red | peach |
| monochrome | green | green | green | cyan | green | green | green | green | green | lime | green/green/**pale green** | **pale green** | green |
| monochrome-light | green | green | green | cyan | green | green | green | green | green | green | green/green/green | **dark green** | green |
| nord | cyan | green | red | orange | **blue** | **yellow** | purple | yellow | green | orange | green/yellow/red | red | yellow |
| nord-light | blue | green | red | cyan | **blue** | **orange** | purple | orange | green | orange | green/orange/red | red | orange |
| solarized-dark/light | blue | olive | red | orange | **cyan** | **violet** | violet | magenta | olive | yellow | olive/yellow/red | red | yellow |
| tokyo-night | blue | green | pink-red | orange | orange | purple | purple | cyan | green | orange | green/orange/pink | pink-red | orange |
| ubuntu-dark | blue | green | red | cyan | orange | lilac | purple | blue | green | yellow | green/orange/red | red | orange |
| ubuntu-light | blue | green | red | cyan | orange | purple | purple | orange | green | yellow | green/orange/red | red | orange |
| windows-dark | blue | green | red | cyan | yellow | **cyan** | blue | magenta | green | yellow | green/yellow/red | red | orange |
| windows-light | blue | green | red | cyan | orange | **cyan** | purple | magenta | green | orange | green/yellow/red | red | orange |
| fallback | blue | green | orange | red | **blue (=CPU)** | **green (=memory)** | blue | green | yellow | orange | **blue/grey/red** | red | yellow |

**What holds across themes:**
- CPU is blue or cyan in 18 of 20.
- Memory is green in 18.
- Disk read is red in 18.
- Error is red everywhere except Monochrome.

**What doesn't hold:**
- **Network send/receive** has no family; Nord and Nord Light disagree with each other.
- **Disk write** is cyan in 7 themes, orange in 7, pink in 3 and red in 1.
- **Power** is green (equal to memory) on GPU, blue on System and info-blue on Process Details.
- **Battery** uses memory green.
- **Temperature** equals the warning hex in 9 themes.
- **Disk read and Swap** equal the error hex in 13.
- **The load ramp** is not green → amber → red in Cyberpunk, Cyberpunk Light, the fallback and Monochrome.

### Recommended semantic color strategy

1. **Roles, not reuse.** Add `charts.memory_cached`, `swap`, `power`, `battery`, `threads`, `handles`, `page_faults`, `gdi` and a distinct `cpu_total`. Loader fallbacks keep today's values, so user themes still load. Code stops passing `chartCpu`/`chartMemory`/`chartIo`/`textInfo`/`accentColor(n)` for unrelated metrics.
2. **Fixed hue families, with lightness set per theme:**

| Concept | Family | Notes |
|---|---|---|
| CPU | blue | Total = strongest blue; User a lighter tint; System cyan/violet (ΔL* ≥ 20); I/O wait amber-brown |
| Memory | green | Used green; Cached teal/green tint; **Swap violet** (swap isn't an error) |
| GPU | magenta/purple | utilization magenta; memory pink/violet; clock indigo; enc/dec two distinct neutrals; temperature neutral until hot; **power yellow** |
| Network | send amber, receive cyan/teal | "up warm, down cool"; the same in every theme |
| Disk | read orange, write brown/olive | never red |
| Power | yellow | one colour on every screen |
| Battery | its own role (e.g. lime/teal) | not memory green |
| Temperature | neutral line, orange→red ramp only when hot | never the warning hex at normal temperatures |
| Success / Warning / Error / Info | green / amber / red / blue | reserved for state and messages; **never a data series** |
| Selection / accent | theme accent tint | ≥ 1.35:1 against the row, with an accent edge |
| Load ramp | green → amber → red | in every theme; Monochrome expresses it through lightness plus low-chroma hues |

3. **Pairs that appear together** (user/system, send/receive, read/write, warning/success, Threads/Faults) differ by ΔL* ≥ 20, with ΔE2000 ≥ 15 for normal vision and ≥ 8 under protan/deutan simulation.
4. **Contrast floors:**
   - primary text 7:1 (4.5 minimum) on the window, plot and selected row;
   - muted text 4.5:1 on the plot;
   - series and NowBars 3:1 on the plot, and any selection edge or tab overline 3:1 against the tab or row;
   - selection fills: selected row 1.35:1 and selected tab 1.3:1 against their neighbours;
   - grid 1.3–1.6:1.
5. **Enforce it:** the bundled-theme lint in #1199.

**Semantically consistent across themes:** each concept's hue family, the order of the load ramp, and the rule that status colours never encode data. **Free per theme:** exact lightness, chroma and the decorative accents.

## Typography and Text

### Unclear labels and wording

| # | Location | Current | Recommended |
|---|---|---|---|
| 1 | `ProcessColumnConfig.h` | S · RES · VIRT · SHR · PEAK · THR · TIME+ · PF · GPU Dev · MEM % · Net Recv | State · Memory · Virtual · Shared · Peak Mem · Threads · CPU Time · Page Faults · GPU · Mem % · Net Received |
| 2 | `SystemMetricsPanel.cpp` L521, `CpuCoresSection.cpp` L83 | `(16 cores @ 3.60 GHz)` | `(16 logical processors @ 3.60 GHz)` |
| 3 | `SystemMetricsPanel.cpp` L553 | `, 31.9 GB RAM, 8.0 GB VRAM` appended to the CPU model | separate muted line `RAM 31.9 GB · VRAM 8.0 GB` |
| 4 | section headers | `CPU Usage (300 samples)` · `Memory & Swap (300 samples)` · `Threads, Page Faults & Handles (…)` | `CPU` · `Memory & Swap` · `Threads, Handles & Page Faults` |
| 5 | `NetworkSection.cpp` L308 | `Network Throughput - Total (selected: X, history unavailable) (300 samples)` | `All interfaces`, plus a muted note `History isn't available for X` |
| 6 | `NetworkSection.cpp` L470–471 | `TX Rate` / `RX Rate` | `Sent` / `Received` |
| 7 | `NetworkSection.cpp` L183 | `Link: 1000 Mbps [Up]` | `Link 1 Gbps · Up` |
| 8 | `GpuSection.cpp` L163 | `GPU Monitoring (1 GPU)` | remove (the adapter header is enough) |
| 9 | `GpuSection.cpp` L220, `ProcessDetailsPanel.cpp` L1764 | `[Shared Memory]` / `[Discrete]` | `Integrated` / `Discrete` |
| 10 | `GpuSection.cpp` L72 | `Note: This system does not report GPU clock speed or encoder/decoder utilization` | `Not reported by this GPU: clock speed, video encode/decode` |
| 11 | `GpuSection.cpp` L484/L537 | `Clock: unavailable this sample` | `Clock: not available` |
| 12 | `ChartWidgets.h` L179 | `Age: 3.2s` | `3.2 s ago` |
| 13 | x-axis | `Time (s)`, ticks `-300 … 0` | no title; ticks `5m … now` |
| 14 | Faults label | `Page Faults/s: 12.0/s` | `Page faults: 12/s` |
| 15 | Threads NowBar | `1,234 threads` | `1,234` |
| 16 | `ProcessColumnConfig.h` L190 | `(H:MM:SS.cc)` | `(h:mm:ss)` |
| 17 | `ProcessesPanel.cpp` L681 | `List View` shown while the tree is displayed | segmented control `List \| Tree` |
| 18 | `ProcessDetailsPanel.cpp` L719 | `Status` row | `State` |
| 19 | `ProcessDetailsPanel.cpp` L717 (Windows) | `Normal (nice: 0)` | `Normal` |
| 20 | `ProcessDetailsPanel.cpp` L2102 | `Are you sure you want to stop process 'x'?` [Yes] [No] | `Suspend x (PID n)?` [Suspend] [Cancel] |
| 21 | `ProcessDetailsPanel_ActionHelpers.h` L97 | `Success: terminate sent to PID 1234` | `Sent end request to x (PID 1234)` / `Couldn't end x: …` |
| 22 | Details actions | `Pause` | `Suspend` |
| 23 | GPU Usage labels | `GPU Utilization:` (trailing colons) | `GPU utilization` (no colons, like the Identity table) |
| 24 | details tab | `Select a process` | `Details` (process name once one is selected) |
| 25 | System tab | hostname | `System` (hostname in a tooltip) |
| 26 | status bar | `Ready` | live status, e.g. `329 processes · updating every 1 s` |
| 27 | "?" button | opens About | label `About TaskSmack` (CIRCLE_INFO) until #172 |
| 28 | Settings | `APPEARANCE` / `PERFORMANCE` / `ADVANCED` | `Appearance` / `Data` / `Advanced` |
| 29 | Settings | `Metric Refresh Rate` / `Metric History` | `Update interval` / `History length` |
| 30 | Settings | `Apply` (also closes) | `Save` |
| 31 | `SettingsLayerDetail.h` L32 | `Even Huger` | `Largest` |
| 32 | `AboutLayer.cpp` L159 | `TaskSmack: the cross-platform system monitor` | `A cross-platform system monitor` |
| 33 | ellipses | `...` (three periods) | `…` |

### Terminology: one name per concept

| Concept | Today | Use |
|---|---|---|
| Resident memory | RES / Resident Memory / Used | Memory |
| Peak resident | PEAK / Peak Resident / Peak Used | Peak Mem |
| Run state | S / State / Status (row) — plus a different "Status" column | State |
| Network direction | Net Sent / Net Recv / Sent / Received / TX / RX | Sent / Received |
| Suspend | Pause / stop / Suspended | Suspend / Resume |
| Logical CPU | core / Core N | logical processor / CPU N |

### Formatting

- **Units:** a space before every unit everywhere. Today values say "1.2 GB" but axes say "1.2GB".
- **Precision:**
  - % with 0 dp, or 1 dp below 10 % per process;
  - rates with 1 dp;
  - counts as integers;
  - watts as one rule. GPU power is currently 0, 1 and 2 dp on one screen.
- **Locale:** apply it to axes as well as values.
- **Durations:** one compact style and one clock style.
- **Axis ticks:** 1-2-5 steps, capped by plot height.

### Font, size and hierarchy

- **Chart text** is currently the Small preset, about 8 px; floor it at about 10 px (#1194).
- **Headers:** use `largeFont()`, or load `Inter-Bold`, for section headers (#1200).
- **Numbers:** tabular digits for live numbers (#1201).
- **Title-bar font:** Sixtyfour looks crisp at 150 % and 175 %; other fractional scales weren't checked.
- **Preset names:** call them by size ("Largest"), not "Even Huger".

## Icon Opportunities

| Screen/location | Current control/text | Suggested icon | Keep text? | Reason |
| --------------- | -------------------- | -------------- | ---------- | ------ |
| GPU tab, headers, empty states | MICROCHIP (same as CPU) | DISPLAY or CUBES | Yes | Separate GPU from CPU |
| CPU per-core cells | MICROCHIP in every cell | none (keep it on the tab and header) | Yes | Less grid noise |
| System main tab | COMPUTER + hostname | COMPUTER + "System" | Yes | Clear label |
| Settings → Performance | GAUGE_HIGH (also Overview and Priority) | STOPWATCH / CLOCK | Yes | It's about time settings |
| Settings → Advanced | FOLDER_OPEN (repeats a button's icon) | WRENCH | Yes | No duplicate glyph |
| Settings Theme / Font rows | none | PALETTE / FONT | Yes | Scannable rows |
| Actions tab, Process Control | GEARS (also Resources) | SLIDERS | Yes | One glyph per concept |
| Resources headers | GEARS | LAYER_GROUP | Yes | Distinct concept |
| Priority header | GAUGE_HIGH | ARROW_UP_WIDE_SHORT | Yes | Rank metaphor |
| Terminate button | XMARK (reads as close/clear) | POWER_OFF / CIRCLE_STOP | Yes | XMARK already means close |
| Confirm dialog | none | TRIANGLE_EXCLAMATION | Yes | Marks a destructive action |
| Process GPU history headers | CHART_LINE | the GPU icon | Yes | Subject icons elsewhere |
| Processes filter box | hint text | MAGNIFYING_GLASS | Yes | Search affordance |
| View toggle | "Tree View" / "List View" | LIST / SITEMAP segmented | Optional | Shows the current mode |
| Column chooser | none (right-click only) | TABLE_COLUMNS button | Tooltip | Discoverability |
| Tree expander | "+" / "-" buttons | CARET_RIGHT / CARET_DOWN | No | Standard disclosure glyph |
| Row context menu (new) | none | CIRCLE_INFO, COPY, PAUSE, POWER_OFF, SKULL | Yes | Mirrors the Actions tab |
| Command line row | text | COPY button | Tooltip | Common task |
| Network legend | Sent / Received | ARROW_UP / ARROW_DOWN | Yes | Direction not carried by colour alone |
| Disk legend | Read / Write | ARROW_DOWN / ARROW_UP | Yes | Same reason |
| Interface Status | coloured "Up"/"Down" | CIRCLE_CHECK / CIRCLE_XMARK + text | Yes | Not colour-only |
| Interface Type column | inline mapping, icon only | shared `getInterfaceTypeIcon` + tooltip | Tooltip | One mapping |
| Per-disk cell | raw device name | HARD_DRIVE | Yes | Context |
| Battery charging | BOLT (also the section icon) | CHARGING_STATION | Yes | One glyph per meaning |
| "Collecting data…" | text | HOURGLASS_HALF | Yes | Temporary state |
| "Process exited" | TRIANGLE_EXCLAMATION | CIRCLE_INFO | Yes | It isn't an error |
| Title-bar Settings/Help | icon, no tooltip | add tooltips | Tooltip | Parity with the status bar |
| System menu "Move" | ARROW_RIGHT | ARROWS_UP_DOWN_LEFT_RIGHT | Yes | Arrow-right reads as "next" |

**Spacing rule:** one `iconLabel()` helper producing icon + one space + text. Today about 14 sites use one space and about 45 use two. The icon set is consistent (Font Awesome solid, 1 em advance) and stays crisp at 150 % and 175 %. Tracked in #977.

## Chart Review

| Family | Visual / colour | Layout / axis / legend | Tooltip | Issues |
|---|---|---|---|---|
| CPU total + stacked (Overview) | Total bar uses the load ramp, not its line's colour; User = Total blue; band swatches are 35 % fills | legend inside the plot; 1 dp % ticks | User/Total same colour; Idle row near-invisible | #1192, #1193, #1198, #1202 |
| CPU Cores grid | threshold-coloured bars vs blue lines | each of 16 cells repeats "Time (s)" and ticks; "Core N" is a logical CPU | — | #1206, #1203 |
| Memory & Swap | Swap in disk-read red (= error hex in 13 themes); Cached in CPU blue; 3 fills | legend clips "Peak Used" at small sizes | — | #1196, #1198 |
| Power & Battery | estimate labelled "Power" in CPU blue; battery in memory green | battery axis hidden; battery plotted on a Watts axis | "1:05" ambiguous | #1196, #1206, #1202 |
| System Resources | Page Faults = Handles in Cyberpunk and Dracula | unidentified Y2 | "/s" duplicated | #1197, #1206, #1202 |
| GPU Core & Video | memory = decoder in 4 themes; up to 5 series, ≥ 3 fills | maximized: one chart, ~40 % empty; 21 y ticks at 5 % steps | covers the x labels | #1197, #1198, #1207, #1202 |
| GPU Thermal & Power | normalised to an unexplained %; temperature in the warning colour | — | precision 0/1/2 dp | #1205, #1196 |
| Network | send/receive families differ per theme; interface vs total by alpha only | ticks at 1.9 MB/s steps; empty NowBars at idle | — | #1196, #1198, #1202 |
| Disk I/O | read in the error red; write = network send colour | ticks at 95.4 MB/s steps | — | #1196, #1202 |
| Process CPU | User = Total swatch | fixed 0–100 % of the machine (flat line) | integer % | #1195, #1192 |
| Process Memory | Virtual bar is a large error-red block | 0–100 % of RAM; Virtual on an unlabelled Y2 | — | #1195, #1206, #1196 |
| Process Resources | GDI vs Handles collide in Monochrome | 4 NowBar columns vs 3 (misaligned); unexplained 0–450 / 0–250 axes | "/s" duplicated; GDI N/A only in the tooltip | #1206, #1197 |
| Process GPU | — | headers use CHART_LINE; colons; memory axis at 19.1 MB steps | — | #1202, #977 |

**Every chart:**
- NowBars show no value (#1193).
- Chart text is about 8 px (#1194).
- Grid lines are near-invisible (#1191).
- The legend sits over the data (#1198).

**Already tracked chart items:** #1003 (NowBars scaled to the series max), #1012/#1021 (NowBar smoothing), #1013 (no-data states), #1020 (shared tooltip), #1024 (mixed axis), #1039 (ImPlot mouse text), #1023 (GPU memory axis).

## Layout Review

- **Spacing:** the scaled style (#942/#980) is consistent, but vertical rhythm between sections is ad hoc (Separator vs Spacing ×1–4) and dialog footers differ (#1200).
- **Alignment:**
  - Headers are centred over right-aligned numbers (#1209).
  - Settings' two sections use different control columns (Theme starts left of Metric Refresh Rate).
  - Charts with a second axis or a different NowBar column count don't share edges (#1206).
  - Label colons appear in some tables and not others.
- **Hierarchy:** headers, values and notes share one size and weight (#1200); current values are hover-only (#1193).
- **Sizing:**
  - Fill layout floors and caps (8.4375–33.75 em) are sound.
  - Maximized, single-chart views leave large dead areas.
  - Even Huger shows one chart per screen (#1207).
- **Density:**
  - Processes is dense, which is right, but noisy: on Windows, SHR and Status are all "-" and VIRT is about 2 TB everywhere (#1210).
  - Interface Status is dominated by down adapters (#1211).
- **Responsive:**
  - At 200×200 dp, controls overprint; at 900×700, tab labels truncate (#1207).
  - OS-level maximize breaks layout (#1208).
- **DPI and font combinations:**

| Combination | Assessment | How assessed |
|---|---|---|
| 100 %/Small | body text 8 px, chart text smaller still: too small | reasoned |
| 100 %/Medium | chart text ~8 px | reasoned |
| 150 %/Normal | fine; chart text small | observed |
| 175 %/Extra Large | tabs, NowBars and padding scale well; a 1366-wide window keeps a usable plot | observed |
| 200 %/Normal | ImPlot plot/label padding and ticks deliberately unscaled (#1055) — fine | reasoned |

  Line weights scale (#1055); 1 px borders stay hairlines by design.

## Accessibility / Legibility

- **Contrast:**
  - Muted (axis) text is below 4.5:1 on the plot in 12 themes; Solarized Light's primary text is 4.13 (#1167).
  - Status texts fail on most light themes.
  - Series and NowBars are below 3:1 in 18 themes (#1191).
  - Tooltip text in series colours is below 4.5:1 (#1192).
- **Colour-only distinctions:**
  - NowBars have no labels.
  - Interface Up/Down is coloured text only.
  - The CPU Total bar's severity is colour only.
  - Network total vs interface differs only by alpha.
  - Fixes: #1193, #1211, #1198.
- **Colour vision:** red/green (memory vs swap, success vs error) and blue/purple (Threads vs Page Faults, GPU clock vs utilisation) collapse in 11 themes under simulation (#1197).
- **Text size:** chart text is about 8 px at the default size (#1194).
- **Hover and selection clarity:** selected row 1.01–1.27:1 in 14 themes; selected tab under 1.3:1 in 17 (#1190).
- **Chart-series differentiation:** fills, one line weight and in-plot legends (#1198); same-colour pairs in 12 themes (#1197).

## Consistency Issues

1. **Section headers** come in five styles: icon + text, plain text (`GPU Monitoring`), CollapsingHeader, ALL CAPS (Settings), and the warning colour.
2. **Separation** between sections uses a Separator, Spacing, or Spacing ×4.
3. **Label colons** are present in the GPU usage tables and absent in Identity/Runtime.
4. **Missing values** are "-", "N/A", "Unknown", "unavailable this sample" or "not reported this sample".
5. **Precision** differs between the table (0.6 %) and the details pane (0 %).
6. **Unit spacing and precision** differ between values, axes and tooltips.
7. **Durations** use four notations.
8. **Dialog footers** are centred, right-aligned or left-aligned.
9. **Icon spacing** is one space or two.
10. **Empty states** are drawn by `renderEmptyState` in two places and as bare text in eight.
11. **Interface-type icons** have two mappings.
12. **Colour:** the CPU Total and per-core bars differ from their lines, the only bars that still do after #1004/#1005.
13. **Explanations** are hover-only tooltips on plain text with no info cue (Power note, Process Network).
14. **Process counts** read "Processes: 330" on Overview but "330 processes, 329 running" on Processes.
15. **Power** is drawn in three colours on three screens.

## Quick Wins

1. Pass `ImGuiTabBarFlags_DrawSelectedOverline` on the three tab bars (#1190).
2. Make the selected-row colour opaque and visible in the 14 failing themes (#1190).
3. Draw the CPU Total bar in the Total line's colour (#1192).
4. Draw the filter hint in the muted text colour, not the running green (#1196).
5. Drop "(N samples)" from section headers (#1200).
6. Add tooltips to the title-bar Settings and Help buttons (#1200).
7. Rename `FAULTS_LABEL` to "Page faults" so its unit isn't duplicated (#1202).
8. Change the tooltip's "Age: 3.2s" to "3.2 s ago" (#1202).
9. Fix the TIME+ column description to say h:mm:ss (#1203).
10. Rename the Details "Status" row to "State"; use Suspend/Resume consistently (#1203).
11. Settings: Title Case headers, "Update interval", "History length", "Largest" (#1203).
12. Confirm dialogs: [Verb] [Cancel] (#1203).
13. Use "Integrated" instead of "Shared Memory" (#1203).
14. Hide SHR and Status by default on Windows (#1210).
15. Route the 8 bare empty states through `renderEmptyState` (#1210).
16. Use one icon-spacing helper (#977).
17. Credit Font Awesome and Sixtyfour in About (#1212).
18. Fix the Cyberpunk load-ramp order and the fallback accents (#1196).
19. Give Dracula's `charts.gpu.fan` a visible colour (#1191).
20. Use the `…` glyph instead of three periods (#1203).

## Larger UI Improvements

1. **Semantic colour roles and families**, with a bundled-theme lint (#1196, #1199, then #1191/#1197).
2. **Current-value strip or legend values** for every chart, driven by one `SeriesSpec` that also feeds tooltips and NowBars (#1193, #1192).
3. **Number grammar:** one `Format.h` grammar, nice axis ticks and tabular digits (#1202, #1201).
4. **Chart scaling:** auto-scaled process charts and unit-true GPU charts (#1195, #1205).
5. **Process table interaction model:** columns button, view-mode control, row context menu, alignment (#1209), together with #838 and #804.
6. **Typography hierarchy:** a section-header helper and a chart-text floor (#1200, #1194).
7. **Responsive layout:** a content-derived minimum window and use of large windows (#1207), plus the OS-maximize fix (#1208).
8. **Windows priority-class UI** (#1204).
9. **Terminology:** a glossary pass ahead of localisation (#1203, #219).

## Filed Issues

| Finding | Issue | Severity | Scope | Summary |
| ------- | ----- | -------- | ----- | ------- |
| UI-001 | [#1190](https://github.com/mgradwohl/tasksmack/issues/1190) | High Impact | Small | Selected row and selected tab unmistakable in every theme |
| UI-002 | [#1191](https://github.com/mgradwohl/tasksmack/issues/1191) | High Impact | Medium | Series, NowBars and grid at visible contrast |
| UI-003 | [#1192](https://github.com/mgradwohl/tasksmack/issues/1192) | High Impact | Small | NowBar/legend/tooltip colours match their series and stay readable |
| UI-004 | [#1193](https://github.com/mgradwohl/tasksmack/issues/1193) | High Impact | Medium | Current values without hovering |
| UI-005 | [#1194](https://github.com/mgradwohl/tasksmack/issues/1194) | High Impact | Small | Chart text at the Small preset (~8 px) |
| UI-006 | [#1195](https://github.com/mgradwohl/tasksmack/issues/1195) | High Impact | Small–Medium | Process CPU/memory charts flat |
| UI-007 | [#1167](https://github.com/mgradwohl/tasksmack/issues/1167) (existing) | High Impact | Small | Text roles below WCAG AA where drawn |
| UI-008 | [#1196](https://github.com/mgradwohl/tasksmack/issues/1196) | Medium Impact | Medium | Semantic colour roles and families |
| UI-009 | [#1197](https://github.com/mgradwohl/tasksmack/issues/1197) | Medium Impact | Medium | Same-chart collisions; colour-blind collapse |
| UI-010 | [#1198](https://github.com/mgradwohl/tasksmack/issues/1198) | Medium Impact | Small–Medium | Encoding beyond colour; one fill; legend off the data |
| UI-011 | [#1199](https://github.com/mgradwohl/tasksmack/issues/1199) | Medium Impact | Medium | Bundled-theme colour lint |
| UI-012 | [#1200](https://github.com/mgradwohl/tasksmack/issues/1200) | Medium Impact | Small–Medium | Header hierarchy; status/title bar; dialog chrome |
| UI-013 | [#1201](https://github.com/mgradwohl/tasksmack/issues/1201) | Medium Impact | Medium | Tabular digits; decimal alignment |
| UI-014 | [#1202](https://github.com/mgradwohl/tasksmack/issues/1202) | Medium Impact | Medium | Number/unit/duration grammar; round ticks |
| UI-015 | [#1203](https://github.com/mgradwohl/tasksmack/issues/1203) | Medium Impact | Small | One name per concept; headers, Settings and dialog wording |
| UI-016 | [#1204](https://github.com/mgradwohl/tasksmack/issues/1204) | Medium Impact | Small–Medium | Windows priority classes, not nice |
| UI-017 | [#1205](https://github.com/mgradwohl/tasksmack/issues/1205) | Medium Impact | Small–Medium | GPU normalised % axis |
| UI-018 | [#1206](https://github.com/mgradwohl/tasksmack/issues/1206) | Medium Impact | Small–Medium | Secondary axes, stacked alignment, grid-cell chrome |
| UI-019 | [#1207](https://github.com/mgradwohl/tasksmack/issues/1207) | Medium Impact | Small–Medium | Content-derived min size; dead space when large |
| UI-020 | [#1208](https://github.com/mgradwohl/tasksmack/issues/1208) | Medium Impact | Small–Medium | OS maximize leaves a quarter-size, clipped window |
| UI-021 | [#1209](https://github.com/mgradwohl/tasksmack/issues/1209) | Medium Impact | Medium | Process table affordances |
| UI-022 | [#1210](https://github.com/mgradwohl/tasksmack/issues/1210) | Medium Impact | Small | N/A vs zero; unsupported columns; empty states |
| UI-023 | [#977](https://github.com/mgradwohl/tasksmack/issues/977) (existing) | Medium Impact | Small–Medium | Icon vocabulary and spacing |
| UI-024 | [#1211](https://github.com/mgradwohl/tasksmack/issues/1211) | Polish | Small | Hide down/virtual interfaces |
| UI-025 | [#1212](https://github.com/mgradwohl/tasksmack/issues/1212) | Polish | Tiny | About credits (Font Awesome CC BY 4.0, Sixtyfour) |
| UI-026 | [#1130](https://github.com/mgradwohl/tasksmack/issues/1130) (existing) | Polish | Tiny | Priority badge and slider thumb contrast |
| UI-027–035 | see Polish Findings | Polish | Tiny–Small | Folded into the issues listed there |

## Top 15 UI Actions

| # | Finding | Issue | Component / file | Change | Reason | Scope |
|---|---|---|---|---|---|---|
| 1 | UI-001 | #1190 | `ProcessesPanel.cpp`, tab bars, theme `ui.header.*` | Visible selected row; draw the tab overline | Users can't see what they're acting on | Small |
| 2 | UI-004 | #1193 | `ChartWidgets.h` `renderHistoryWithNowBars` | Show current values without hovering | A monitor's primary question | Medium |
| 3 | UI-006 | #1195 | `ProcessDetailsPanel.cpp` | Auto-scale process CPU and memory charts | Typical processes draw flat lines | Small–Medium |
| 4 | UI-003 | #1192 | `SystemMetricsPanel.cpp`, `CpuCoresSection.cpp`, `ChartWidgets.h` | Bars, legends and tooltips match their series | The colour key is self-contradictory | Small |
| 5 | UI-005 | #1194 | `ChartWidgets.h` `PlotFontGuard`, `Theme.cpp` | Chart-text floor of about 10 px | Axis text is 8 px | Small |
| 6 | UI-002 | #1191 | theme TOMLs, `Theme.cpp:491` | Series, NowBars and grid ≥ 3:1 | Lines and bars vanish | Medium |
| 7 | UI-007 | #1167 | theme TOMLs | Text ≥ 4.5:1 where drawn | Axis and status text unreadable | Small |
| 8 | UI-008 | #1196 | `ColorScheme`, `ThemeLoader`, panels | Semantic colour roles and families | Colour doesn't identify a metric | Medium |
| 9 | UI-011 | #1199 | `tests/UI` | Bundled-theme lint | Prevents regressions across 20 themes | Medium |
| 10 | UI-014 | #1202 | `Format.h`, axis formatters | One number grammar; round ticks | The same quantity reads differently in three places | Medium |
| 11 | UI-013 | #1201 | `ProcessesPanel.cpp`, `ProcessRowFormat.h` | Tabular digits; decimal alignment | Live numbers jitter and misalign | Medium |
| 12 | UI-015 | #1203 | `ProcessColumnConfig.h`, Details, Settings | One name per concept; plain headers | Jargon and conflicting names | Small |
| 13 | UI-020 | #1208 | `TitleBarLayer`, `Core/Window` | Handle OS maximize | Win+Up and snap break the window | Small–Medium |
| 14 | UI-010 | #1198 | `plotLineWithFill`, `setupLegendDefault` | One fill; legend off the data; non-colour cues | Muddy, colour-only charts | Small–Medium |
| 15 | UI-021 | #1209 | `ProcessesPanel.cpp` | Columns button, view-mode control, row context menu | Main-view interactions are hidden | Medium |
