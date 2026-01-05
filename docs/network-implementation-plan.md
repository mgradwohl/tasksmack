# Network Monitoring Implementation Plan

**Epic Issue:** #308  
**Created:** 2026-01-04  
**Status:** Planning

## Overview

This plan covers the implementation of the complete network monitoring feature set for TaskSmack, following the recommended order from Epic #308.

## Branching Strategy

All phases will branch from and PR into `dev/network-epic`. After all phases are complete, a single PR merges `dev/network-epic` into `main`.

```
main
  │
  └─► dev/network-epic (integration branch)
        │
        ├─► feature/per-interface-network (#295 + #294)
        │     └─► PR → dev/network-epic
        │
        ├─► feature/network-panel (#167)
        │     └─► PR → dev/network-epic
        │
        └─► Final PR → main (after all phases complete)
```

**Benefits:**
- Reduces risk of partial features on `main`
- Allows incremental review without blocking releases
- Single integration point for testing all network features together

## Current State

| Layer | Linux | Windows | Issue |
|-------|-------|---------|-------|
| **System-wide network** (System Overview tab) | ✅ Done | ❌ Not implemented | #294 |
| **Per-interface breakdown** | ❌ Not implemented | ❌ Not implemented | #295 |
| **Dedicated Network Panel** | ❌ Not started | ❌ Not started | #167 |

## Dependencies & Prerequisites

**No FetchContent or external dependencies required.** All APIs are system-provided:

| Platform | API | Provided By |
|----------|-----|-------------|
| Windows | `GetIfTable()` | `iphlpapi.lib` (Windows SDK, always available) |
| Linux | `/proc/net/dev` | Linux kernel (always available) |

Only CMake change needed: link `iphlpapi` on Windows.

## Implementation Phases

### Phase 1: Foundation - Per-Interface Network (#295 + #294)

**Branch:** `feature/per-interface-network`  
**Effort:** 3-4 days  
**Dependencies:** None  

#### Scope

Build the foundational per-interface network infrastructure for both platforms. This phase combines the original #295 (per-interface) with #294 (Windows system-wide) since Windows implementation naturally provides per-interface data.

#### Interface Filtering

The interface dropdown shows **TCP/IP network adapters only**:
- ✅ Ethernet adapters
- ✅ Wi-Fi adapters
- ✅ Virtual adapters (VPN, Docker, WSL)
- ❌ Loopback (filtered out)
- ❌ Bluetooth (filtered by interface type)
- ❌ NFC, wireless mice, etc. (not network adapters)

**Filtering logic:**
- Linux: Skip interfaces with no RX/TX activity and loopback (`lo`)
- Windows: Filter by `IF_TYPE` (include `IF_TYPE_ETHERNET_CSMACD`, `IF_TYPE_IEEE80211`; exclude `IF_TYPE_SOFTWARE_LOOPBACK`, etc.)

#### Implementation Tasks

1. Add `InterfaceCounters` struct to `SystemTypes.h`:
   ```cpp
   struct InterfaceCounters {
       std::string name;        // "eth0", "Ethernet", etc.
       std::string displayName; // Friendly name for UI
       uint64_t rxBytes = 0;
       uint64_t txBytes = 0;
       bool isUp = false;
       uint64_t linkSpeedMbps = 0; // 0 if unknown
   };
   ```
2. Add `std::vector<InterfaceCounters> interfaces` to `NetworkCounters`
3. **Linux:** Update probe to store per-interface data (already parsing `/proc/net/dev`)
4. **Windows:** Implement `readNetworkCounters()` using `GetIfTable()`:
   - Enumerate interfaces, store per-interface counters
   - Filter by interface type (exclude loopback, non-network)
   - Set `.hasNetworkCounters = true` in capabilities
   - Note: Uses older API for SDK compatibility; see issue #349 for GetIfTable2 upgrade
5. Add per-interface rates to `SystemSnapshot`
6. Update `SystemModel` to compute per-interface rates
7. Add interface selector dropdown to System Overview network section
8. Enable history charts and "now bars" for selected interface

#### UI Features

- Interface selector dropdown with "All Interfaces" default
- History chart shows selected interface (or aggregate)
- "Now bar" shows current RX/TX rates
- Rates auto-scale (B/s → KB/s → MB/s → GB/s)

#### Files to Modify

- `src/Platform/SystemTypes.h`
- `src/Platform/Linux/LinuxSystemProbe.cpp`
- `src/Platform/Windows/WindowsSystemProbe.cpp`
- `src/Domain/SystemSnapshot.h`
- `src/Domain/SystemModel.cpp`
- `src/App/Panels/SystemMetricsPanel.cpp`
- `CMakeLists.txt` (add `iphlpapi` link for Windows)

#### Testing Strategy

**Platform Tests (`tests/Platform/`):**
- `NetworkCapabilityEnabled` - verify `hasNetworkCounters == true`
- `NetworkCountersAreValid` - counters ≥ 0
- `NetworkCountersMonotonicallyIncrease` - counters don't decrease between samples
- `InterfaceListNotEmpty` - at least one interface returned
- `InterfaceFilteringCorrect` - loopback excluded, network adapters included

**Domain Tests (`tests/Domain/`):**
- `NetworkRateCalculation` - verify bytes/sec from counter deltas
- `PerInterfaceRates` - each interface has independent rate calculation
- `InterfaceAddRemove` - handle hot-plug scenarios gracefully
- `ZeroTimeDelta` - no division by zero

**Integration Tests:**
- UI shows network section when capability enabled
- Interface dropdown populates correctly
- Graph updates when interface selected

#### Acceptance Criteria

- [ ] `NetworkCounters` includes per-interface data on both platforms
- [ ] `capabilities().hasNetworkCounters == true` on Windows
- [ ] `SystemSnapshot` includes per-interface rates
- [ ] UI shows interface selector dropdown in System Overview
- [ ] "All Interfaces" shows aggregated view (current Linux behavior)
- [ ] Selected interface shows individual throughput
- [ ] History charts and "now bars" work for network
- [ ] All tests pass

#### PR Strategy

PR to `dev/network-epic` when both platforms complete with tests.

---

### Phase 2: Dedicated Network Panel (#167)

**Branch:** `feature/network-panel`  
**Effort:** 5-7 days  
**Dependencies:** Phase 1 (per-interface data required)  

#### Scope

Create a dedicated Network Panel similar to btop++, providing comprehensive network monitoring in its own dockable panel.

#### Relationship to System Overview

- **System Overview:** Keeps a compact network summary (current rates, small chart)
- **Network Panel:** Provides detailed view with larger charts, more metrics, interface details
- **No charts move** - System Overview retains its network section; Network Panel is additive

#### Panel Layout (v1)

```
┌─────────────────────────────────────────────────┐
│ Network                                    [≡]  │
├─────────────────────────────────────────────────┤
│ Interface: [eth0 ▼]                             │
│                                                 │
│ ▼ Download: 1.2 MB/s    ▲ Upload: 156 KB/s     │
│ ┌─────────────────────────────────────────────┐ │
│ │ [========== throughput graph ===========]   │ │
│ └─────────────────────────────────────────────┘ │
│                                                 │
│ Total Downloaded: 4.2 GB   Total Uploaded: 1.1 GB │
│ Interface Status: Up       Speed: 1 Gbps       │
└─────────────────────────────────────────────────┘
```

#### Implementation Tasks

1. Create `NetworkPanel.h/.cpp` in `src/App/Panels/`
2. Register panel in `ShellLayer`
3. Implement v1 features:
   - Interface selector dropdown (reuse component from Phase 1)
   - Real-time throughput graph (RX/TX, using ImPlot)
   - Current rates with auto-scaling units
   - Cumulative transfer totals (since app start)
   - Interface status and link speed
4. Add panel to View menu
5. Persist panel visibility and selected interface

#### Files to Create

- `src/App/Panels/NetworkPanel.h`
- `src/App/Panels/NetworkPanel.cpp`

#### Files to Modify

- `src/App/ShellLayer.cpp` (register panel)
- `CMakeLists.txt` (add new sources)

#### Preferences Saved

Via ImGui's `imgui.ini` (automatic):
- Panel visibility (open/closed)
- Panel position and size
- Docking location

Via TaskSmack config (if needed):
- Selected interface preference
- Graph time scale preference

#### v1 Features

- [ ] Interface selector dropdown
- [ ] Throughput graph (RX/TX lines, larger than System Overview)
- [ ] Current rates (auto-scaling: B/s → KB/s → MB/s → GB/s)
- [ ] Cumulative totals (since app start)
- [ ] Interface status (Up/Down)
- [ ] Link speed (if available from OS)

#### Testing Strategy

**Unit Tests:**
- Panel construction/destruction (no leaks)
- Interface selector updates when interfaces change

**Integration Tests:**
- Panel appears in View menu
- Panel opens/closes correctly
- Panel persists in layout after restart
- Interface selection updates graph
- Rates display correctly

**Manual Testing:**
- Docking behavior (dock to various locations)
- Multi-monitor support
- Theme switching (colors update)

#### Acceptance Criteria

- [ ] Network Panel appears in View menu
- [ ] Panel is dockable (follows ImGui docking pattern)
- [ ] Interface selector works (same filtering as Phase 1)
- [ ] Graph updates in real-time
- [ ] Rates display with appropriate units
- [ ] Panel persists in layout (imgui.ini)
- [ ] All tests pass

#### PR Strategy

PR to `dev/network-epic` when panel complete with all v1 features.

---

## Future Enhancements (Post-Epic)

These features are out of scope for this epic but tracked for future work:

### Network Panel v2 Enhancements

- **Active connections list** - Show open TCP/UDP connections
- **Per-connection bandwidth** - Traffic per connection
- **Protocol breakdown** - TCP vs UDP pie chart
- **Remote IP information** - Hostname, geolocation
- **Connection filtering** - Filter by process, port, protocol

### Per-Process Network (#293, #353) — IN PROGRESS

**Status:** Implementation started (2026-01-04)  
**Branch:** `feature/per-process-network`

**Value:** Shows which processes are using network bandwidth. Available in:
- Windows Task Manager (Network column)
- Windows Resource Monitor (detailed per-process)
- btop++ (per-process network columns)
- System Informer (detailed per-process)

**Complexity:** Significantly harder than system-wide monitoring due to:
- Socket-to-process mapping (OS doesn't directly expose this)
- High-frequency updates needed (network traffic is bursty)
- Performance impact (scanning all processes/sockets is expensive)
- Privilege requirements (some approaches need root/admin)

#### Implementation Approach (Linux): Netlink INET_DIAG

After evaluating eBPF vs Netlink INET_DIAG, we chose **Netlink INET_DIAG** for:
- No elevated privileges required (unlike eBPF which needs root/CAP_BPF)
- No build toolchain changes (eBPF requires libbpf, clang for BPF bytecode)
- Simpler implementation - kernel directly provides TCP_INFO with byte counters
- Good accuracy for TCP sockets (most applications)

| Approach | Privileges | Build Complexity | Accuracy | Decision |
|----------|-----------|------------------|----------|----------|
| eBPF | Root/CAP_BPF | High (libbpf, BPF compiler) | Excellent | ❌ Too complex |
| Netlink INET_DIAG | None | None | Good (TCP only) | ✅ Chosen |
| /proc/net/tcp parsing | None | None | Incomplete (no byte counters) | ❌ Missing data |

**Implementation:**
1. `NetlinkSocketStats` class - Query TCP/UDP sockets via INET_DIAG
2. `buildInodeToPidMap()` - Map socket inodes to PIDs via `/proc/[pid]/fd/*`
3. Integrate into `LinuxProcessProbe::enumerate()` - Aggregate per-PID

**Files:**
- `src/Platform/Linux/NetlinkSocketStats.h` - Header
- `src/Platform/Linux/NetlinkSocketStats.cpp` - Implementation
- `src/Platform/Linux/LinuxProcessProbe.cpp` - Integration

**Limitations:**
- UDP sockets may have limited byte counter support (kernel-dependent)
- Unix domain sockets not tracked (not relevant for network monitoring)
- Scanning `/proc/[pid]/fd/*` adds some overhead (mitigated by caching)

#### Implementation Approach (Windows): Future Work

Windows options:
- `GetPerTcpConnectionEStats` - Per-connection TCP stats
- ETW (Event Tracing for Windows) - Real-time kernel events

**Note:** UI already exists and waits for probe data. Once probes populate `ProcessCounters::netSentBytes` and `netReceivedBytes`, the UI will automatically work.

---

## Timeline Estimate

| Phase | Estimated Duration | Cumulative |
|-------|-------------------|------------|
| Phase 1 (Foundation) | 3-4 days | 4 days |
| Phase 2 (Network Panel) | 5-7 days | 11 days |
| Final integration & testing | 1-2 days | 13 days |

**Total:** ~2-3 weeks for complete network monitoring epic.

---

## Decisions Made

| Question | Decision |
|----------|----------|
| Branching strategy | `dev/network-epic` integration branch; phases PR into it; final PR to `main` |
| Phase order | Foundation first (per-interface), then panel |
| Interface filtering | TCP/IP network adapters only (no Bluetooth, NFC, etc.) |
| System Overview changes | Keeps network section; Network Panel is additive |
| Per-process network (Linux) | Netlink INET_DIAG + inode-to-PID mapping (no privileges required) |
| Per-process network (Windows) | Future work: GetPerTcpConnectionEStats or ETW |
| Future enhancements scope | Keep tight v1 scope; future enhancements tracked separately |

---

*Document will be deleted after implementation is complete.*
