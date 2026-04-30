# Code Review 0426

**Date:** 2026-04-30
**Scope:** All non-test source files under `src/`
**Reviewer:** Copilot Agent

---

## Review Categories

1. Problematic code that could lead to bugs
2. Use of raw pointers that should be modernized
3. Anti-patterns that should be modernized
4. Refactoring opportunities
5. Use of colors in the UI that are not pulled from Themes
6. Heap allocations that are unnecessary or could leak
7. Use of `char*` that should be modernized to `std::string` or `std::string_view`
8. Unnecessary casts or bad casting patterns
9. Other problems

---

## Issue 1 — Hardcoded close button colors bypass Theme system

**Category:** Hardcoded UI Colors (item 5)
**File:** `src/App/TitleBarLayer.cpp`, lines 448–449
**Severity:** High

### Problem

The close (×) button uses hardcoded red `ImVec4` literals for its hover and active
states, rather than pulling from the active `ColorScheme`:

```cpp
ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8F, 0.1F, 0.1F, 1.0F));
ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9F, 0.2F, 0.2F, 1.0F));
```

All other button colors in `TitleBarLayer` use scheme fields (`buttonHovered`,
`buttonActive`). These hardcoded values mean the close button is always the same red
regardless of the active theme.

### Fix

Add `closeButtonHovered` and `closeButtonActive` fields to `ColorScheme` in
`UI/Theme.h`. Set appropriate defaults in `loadDefaultFallbackTheme()` and in all
theme TOML files, then replace the literals:

```cpp
ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.closeButtonHovered);
ImGui::PushStyleColor(ImGuiCol_ButtonActive,  scheme.closeButtonActive);
```

---

## Issue 2 — `ImVec4` components manually decomposed to apply alpha

**Category:** Hardcoded UI Colors / Anti-pattern (items 3, 5)
**File:** `src/App/Panels/NetworkSection.cpp`, lines 298–299
**Severity:** Medium

### Problem

Interface-specific chart line colors are built by extracting `.x`, `.y`, `.z`
components from existing theme colors and applying a hardcoded `0.7F` alpha:

```cpp
const auto ifaceSentColor = ImVec4(theme.scheme().chartCpu.x,
                                   theme.scheme().chartCpu.y,
                                   theme.scheme().chartCpu.z, 0.7F);
const auto ifaceRecvColor = ImVec4(theme.accentColor(2).x,
                                   theme.accentColor(2).y,
                                   theme.accentColor(2).z, 0.7F);
```

This is verbose, fragile (must be updated if the color type changes), and duplicates
an alpha-modification pattern that appears multiple times in the UI layer.

### Fix

Add a small free function to `UI/Theme.h` (or `UI/Widgets.h`):

```cpp
[[nodiscard]] inline ImVec4 withAlpha(const ImVec4& color, float alpha) noexcept
{
    return {color.x, color.y, color.z, alpha};
}
```

Then replace the two construction lines:

```cpp
const auto ifaceSentColor = UI::withAlpha(theme.scheme().chartCpu, 0.7F);
const auto ifaceRecvColor = UI::withAlpha(theme.accentColor(2), 0.7F);
```

---

## Issue 3 — Priority gradient uses hardcoded RGB formulas, bypassing Theme

**Category:** Hardcoded UI Colors (item 5)
**Files:** `src/App/Panels/ProcessDetailsPanel_PriorityHelpers.h`, lines 82–101;
`src/App/Panels/ProcessDetailsPanel.cpp`, lines 1999–2012
**Severity:** Low

### Problem

The priority slider gradient is computed from raw RGB arithmetic keyed off the nice
value:

```cpp
// high priority (nice < 0) → blue→white
r = t; g = t; b = 1.0F;

// low priority (nice > 0) → white→red
r = 1.0F; g = 1.0F - t; b = 1.0F - t;
```

These colors are fully hardcoded and cannot be themed.

### Fix

Add gradient endpoint colors to `ColorScheme`:

```cpp
ImVec4 priorityHighColor;    // nice < 0 end (default: blue)
ImVec4 priorityNormalColor;  // nice == 0 mid (default: white)
ImVec4 priorityLowColor;     // nice > 0 end (default: red)
```

Interpolate between them in `priorityNiceToColor()` using those scheme values.

---

## Issue 4 — `const char*` label members in option structs should be `std::string_view`

**Category:** `char*` modernization (item 7)
**File:** `src/App/SettingsLayerDetail.h`, lines 21, 39, 57
**Severity:** Low

### Problem

`FontSizeOption`, `RefreshRateOption`, and `HistoryOption` all store display labels
as `const char*`:

```cpp
struct FontSizeOption    { const char* label; UI::FontSize value; };
struct RefreshRateOption { const char* label; int valueMs; };
struct HistoryOption     { const char* label; int valueSeconds; };
```

These are `constexpr` structs holding string literals. `std::string_view` is the
C++23-idiomatic type for read-only string data.

### Fix

Change each member to `std::string_view label;`. The `constexpr` arrays still work
because string literals implicitly construct `string_view`. Pass `.data()` to ImGui
at the call site where a null-terminated pointer is required (e.g.,
`ImGui::BeginCombo(opt.label.data(), ...)`).

---

## Issue 5 — `const char*` section/key parameters in `UserConfigHelpers.h` should be `std::string_view`

**Category:** `char*` modernization (item 7)
**File:** `src/App/UserConfigHelpers.h`, lines 24, 44, 63
**Severity:** Low

### Problem

The `loadAndClamp`, `loadAndNarrowInt64`, and `loadAndNarrowIntWithClamp` template
helpers accept `section` and `key` as `const char*`:

```cpp
inline void loadAndClamp(const toml::table& config,
                         const char* section, const char* key, ...);
```

The toml++ library's `operator[]` accepts `std::string_view` natively.

### Fix

Change both parameters from `const char*` to `std::string_view`. All call sites
pass string literals, so no changes are needed there.

---

## Issue 6 — `const char* suffix` in `ByteUnit` should be `std::string_view`

**Category:** `char*` modernization (item 7)
**File:** `src/UI/Format.h`, line 274
**Severity:** Low

### Problem

`ByteUnit` stores the unit suffix as `const char*`:

```cpp
struct ByteUnit {
    const char* suffix = "B";
    double scale = 1.0;
    int decimals = 0;
};
```

At line 465 it is immediately wrapped into a `string_view`:

```cpp
const std::string_view suffix{unit.suffix};
```

The intermediate `const char*` member is unnecessary.

### Fix

Change the member type directly:

```cpp
struct ByteUnit {
    std::string_view suffix = "B";
    double scale = 1.0;
    int decimals = 0;
};
```

Remove the redundant wrapping on line 465.

---

## Issue 7 — `int m_FrameCount` should be `uint32_t`

**Category:** Anti-pattern / type mismatch (item 3)
**File:** `src/App/ShellLayer.h`, line 57
**Severity:** Low

### Problem

```cpp
int m_FrameCount = 0;
```

Frame counters are never negative. Using a signed `int` gives undefined behaviour on
overflow. At 240 Hz it would take ~2.9 years to overflow, but the semantic type is
wrong.

### Fix

```cpp
uint32_t m_FrameCount = 0U;
```

The cast in `ShellLayer.cpp:109` (`static_cast<float>(m_FrameCount)`) remains valid;
`uint32_t → float` is well-defined.

---

## Issue 8 — Magic number `0.5F` for FPS accumulation window should be a named constant

**Category:** Refactoring / magic numbers (items 3, 4)
**File:** `src/App/ShellLayer.cpp`, line 107
**Severity:** Low

### Problem

```cpp
if (m_FrameTimeAccumulator >= 0.5F)
```

The 0.5-second FPS averaging window is a bare magic number with no explanation.

### Fix

Add to `ShellLayer.h`:

```cpp
static constexpr float FPS_AVERAGE_WINDOW_SECONDS = 0.5F;
```

Replace the literal:

```cpp
if (m_FrameTimeAccumulator >= FPS_AVERAGE_WINDOW_SECONDS)
```

---

## Issue 9 — Defaulted move operations on Platform probe classes are missing `noexcept`

**Category:** Anti-pattern / missing `noexcept` (items 3, 8)
**Files:**
- `src/Platform/Windows/WindowsSystemProbe.h`, lines 21–22
- `src/Platform/Windows/WindowsProcessProbe.h`, lines 45–46
- `src/Platform/Windows/WindowsPowerProbe.h`, lines 18–19
- `src/Platform/Linux/LinuxPowerProbe.h`, lines 23–24
- `src/Platform/Linux/LinuxDiskProbe.h`, lines 18–19

**Severity:** Medium

### Problem

Each of these classes declares move constructor and move assignment as `= default`
without `noexcept`:

```cpp
WindowsSystemProbe(WindowsSystemProbe&&) = default;
WindowsSystemProbe& operator=(WindowsSystemProbe&&) = default;
```

Without `noexcept`, `std::vector` and other containers fall back to the copy path
when growing, and the `performance-noexcept-move-constructor` clang-tidy check will
fire.

### Fix

Add `noexcept` to all five pairs:

```cpp
WindowsSystemProbe(WindowsSystemProbe&&) noexcept = default;
WindowsSystemProbe& operator=(WindowsSystemProbe&&) noexcept = default;
```

---

## Issue 10 — Floating-point equality check via `static_cast<int>` is fragile

**Category:** Bad casting pattern / potential bug (items 1, 8)
**File:** `src/App/Panels/NetworkSection.cpp`, line 461
**Severity:** Medium

### Problem

```cpp
const auto gbps = static_cast<double>(iface.linkSpeedMbps) / 1000.0;
if (gbps == static_cast<int>(gbps))   // check whether value is a whole number
{
    ImGui::Text("%d Gbps", static_cast<int>(gbps));
}
```

Comparing a `double` to its truncated-`int` form with `==` is an anti-pattern.
Although it happens to be exact for small multiples of 1000, it is non-idiomatic and
will mislead future maintainers.

### Fix

Stay in the integer domain throughout:

```cpp
if (iface.linkSpeedMbps % 1000 == 0)
{
    ImGui::Text("%d Gbps", iface.linkSpeedMbps / 1000);
}
else
{
    ImGui::Text("%.1f Gbps", static_cast<double>(iface.linkSpeedMbps) / 1000.0);
}
```

---

## Issue 11 — Redundant `static_cast<double>` applied to a variable already `double`

**Category:** Unnecessary cast (item 8)
**File:** `src/App/Panels/SystemMetricsPanel.cpp`, lines 1094 and 1109
**Severity:** Low

### Problem

```cpp
// Line 1094 – correct cast: timeData is std::vector<float>
const double timeSec = static_cast<double>(timeData[*idxVal]);

// Line 1109 – redundant: timeSec is already double
const auto ageText = formatAgeSeconds(static_cast<double>(timeSec));
```

The cast on line 1109 adds noise with no benefit.

### Fix

```cpp
const auto ageText = formatAgeSeconds(timeSec);
```

---

## Issue 12 — `mutable uint64_t m_SyntheticEnergy` is not thread-safe

**Category:** Potential bug / thread safety (item 1)
**File:** `src/Platform/Windows/WindowsProcessProbe.h`, line 57
**Severity:** Medium

### Problem

```cpp
mutable uint64_t m_SyntheticEnergy = 0;
```

This `mutable` member is read and written inside a `const`-qualified method
(`computeSyntheticEnergy`). It is neither `atomic` nor guarded by a mutex. If the
probe is ever invoked concurrently (e.g., a UI-triggered immediate refresh overlaps
with the background sampler), there is a data race on this variable.

### Fix

```cpp
mutable std::atomic<uint64_t> m_SyntheticEnergy{0};
```

Use `.load()` / `.store()` or `.fetch_add()` as appropriate in the implementation.
Include `<atomic>` if not already present (it is already included in
`LinuxProcessProbe.h`, so the pattern is established).

---

## Issue 13 — `LoadLibraryW` handle for `iphlpapi.dll` is never freed

**Category:** Resource leak (item 6)
**File:** `src/Platform/Windows/WindowsProcessProbe.cpp`, lines 675–682
**Severity:** Medium

### Problem

```cpp
HMODULE iphlp = GetModuleHandleW(L"iphlpapi.dll");
if (iphlp == nullptr)
{
    iphlp = LoadLibraryW(L"iphlpapi.dll");  // increments ref-count
    if (iphlp == nullptr)
    {
        return false;
    }
}
// iphlp used to resolve function pointers...
// HMODULE is a local variable – FreeLibrary is never called
```

When `GetModuleHandleW` returns `nullptr`, `LoadLibraryW` increments `iphlpapi.dll`'s
reference count. The `HMODULE` is a local that is discarded at function exit and
`FreeLibrary` is never called, constituting a formal RAII violation and a reference
count leak.

### Fix

Store the handle as a member variable and free it in the destructor if it was loaded
by this class:

```cpp
// In WindowsProcessProbe.h
HMODULE m_IphlpModule = nullptr;

// In constructor / detectNetworkCounters()
if (GetModuleHandleW(L"iphlpapi.dll") == nullptr)
{
    m_IphlpModule = LoadLibraryW(L"iphlpapi.dll");
}

// In ~WindowsProcessProbe() (or a stop/cleanup method)
if (m_IphlpModule != nullptr)
{
    FreeLibrary(m_IphlpModule);
    m_IphlpModule = nullptr;
}
```

Alternatively, document explicitly (with a `// NOLINT` and comment) that
`iphlpapi.dll` is a core system DLL that lives for the entire process lifetime and
the intentional "leak" is acceptable.

---

## Summary Table

| # | Category | File | Lines | Severity |
|---|----------|------|-------|----------|
| 1 | Hardcoded color | `TitleBarLayer.cpp` | 448–449 | **High** |
| 2 | Hardcoded color / anti-pattern | `NetworkSection.cpp` | 298–299 | **Medium** |
| 3 | Hardcoded color | `ProcessDetailsPanel_PriorityHelpers.h` | 82–101 | Low |
| 4 | `char*` → `string_view` | `SettingsLayerDetail.h` | 21, 39, 57 | Low |
| 5 | `char*` → `string_view` | `UserConfigHelpers.h` | 24, 44, 63 | Low |
| 6 | `char*` → `string_view` | `Format.h` | 274 | Low |
| 7 | Type mismatch / overflow | `ShellLayer.h` | 57 | Low |
| 8 | Magic number | `ShellLayer.cpp` | 107 | Low |
| 9 | Missing `noexcept` | 5 Platform probe headers | various | **Medium** |
| 10 | Float equality via cast | `NetworkSection.cpp` | 461 | **Medium** |
| 11 | Redundant cast | `SystemMetricsPanel.cpp` | 1109 | Low |
| 12 | Thread safety (non-atomic mutable) | `WindowsProcessProbe.h` | 57 | **Medium** |
| 13 | Resource leak (`HMODULE`) | `WindowsProcessProbe.cpp` | 675–682 | **Medium** |
