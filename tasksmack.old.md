# Cross-Platform Task Manager Clone (C++23)

A modern system monitor and process manager inspired by Windows Task Manager, htop, and advanced tools like Process Explorer, System Informer, Glances, btop++, and iStat Menus. Built with C++23, targeting Windows and Linux, with a GUI powered by ImGui (optionally raylib/GLFW/SDL).

---

## Inspirations and References

- [Process Explorer (Sysinternals / Microsoft Learn)](https://learn.microsoft.com/en-us/sysinternals/downloads/process-explorer)  
- [System Informer](https://www.systeminformer.com/)  
- [System Informer GitHub](https://github.com/winsiderss/systeminformer)  
- [System Explorer (TechSpot archive)](https://www.techspot.com/downloads/5015-system-explorer.html)  
- [Glances (GitHub)](https://github.com/nicolargo/glances)  
- [Glances Documentation](https://glances.readthedocs.io/en/latest/quickstart.html)  
- [btop++ (GitHub)](https://github.com/aristocratos/btop)  
- [iStat Menus (Bjango)](https://bjango.com/mac/istatmenus/)

---

## Feature Roadmap

### Core Monitoring
- CPU: per-core usage, load averages, frequency, scheduling states
- Memory: usage, swap, pressure, top consumers
- Disk: per-device and per-process I/O, throughput, queue depth
- Network: per-interface and per-process bandwidth, connections
- GPU: usage, VRAM, temperature (NVML/DRM/WDDM)
- Processes: hierarchical tree, customizable columns, fast sort/filter

### Advanced Controls
- Kill, suspend, renice, set affinity/priority
- Service and driver management (Windows)
- Startup program manager (Windows/Linux autostart)

### Security & Debugging
- Handle/DLL inspection
- Hash lookups (VirusTotal integration optional)
- Change logging (process/service/network events)

### UI/UX Enhancements
- Zoomable graphs and timelines
- Compact/tray overlay view (like iStat Menus)
- Customizable layouts, themes, saved profiles

### Remote Monitoring & Extensibility
- Optional web dashboard/API (Glances-style)
- Plugin system for custom metrics/actions

---

## Architecture Overview

### Components
- **Core Runtime:** Event-driven loop using C++23 coroutines
- **Platform Adapters:** Windows/Linux backends for metrics and actions
- **Data Model:** Immutable snapshots with versioned schemas
- **Aggregation:** Ring buffers per metric, decimation for long timelines
- **Actions Engine:** Safe execution of process/service controls
- **Plugin Framework:** Loadable modules for metrics/actions
- **UI Layer:** ImGui-based GUI, optional raylib renderer
- **Remote/API Layer:** Embedded HTTP server (optional)

---

## Skeleton Directory Structure

```plaintext
taskman-clone/
├── CMakeLists.txt
├── docs/
│   └── ARCHITECTURE.md        # This file
├── src/
│   ├── core/
│   │   ├── runtime.cpp        # Event loop, coroutines
│   │   ├── snapshot.cpp       # Immutable data snapshots
│   │   └── event_bus.cpp      # Pub/sub system
│   ├── platform/
│   │   ├── windows/
│   │   │   ├── process.cpp
│   │   │   ├── services.cpp
│   │   │   └── gpu.cpp
│   │   └── linux/
│   │       ├── process.cpp
│   │       ├── systemd.cpp
│   │       └── gpu.cpp
│   ├── ui/
│   │   ├── main_window.cpp    # ImGui setup
│   │   ├── dashboard.cpp      # CPU/mem/net/disk panels
│   │   ├── process_view.cpp   # Tree/table hybrid
│   │   └── compact_view.cpp   # Tray/overlay mode
│   ├── plugins/
│   │   └── sample_plugin.cpp
│   └── remote/
│       ├── http_server.cpp
│       └── api_endpoints.cpp
├── include/
│   ├── core/
│   ├── platform/
│   ├── ui/
│   ├── plugins/
│   └── remote/
└── tests/
    ├── core_tests.cpp
    ├── platform_tests.cpp
    └── ui_tests.cpp

## Milestones

1. **MVP Monitoring**  
   - CPU/memory/network/disk panels  
   - Process tree view  
   - Smooth real-time graphs  

2. **Process Control**  
   - Kill/suspend processes  
   - Adjust priority/nice/affinity  

3. **Advanced Introspection (Windows)**  
   - Handles/DLLs inspection  
   - Services and driver management  

4. **Sensors & GPU**  
   - lm-sensors integration (Linux)  
   - NVML/DRM/WDDM GPU monitoring  
   - S.M.A.R.T. disk health checks  

5. **Compact Mode**  
   - Tray/overlay quick stats  
   - Theme presets  

6. **Remote/API**  
   - REST/WebSocket endpoints  
   - Multi-host overview dashboard  

7. **Plugins**  
   - Public plugin API  
   - Sample plugins for metrics/actions  

---

## Security & Privacy

- **Least privilege**  
  - Elevation only when necessary  
  - Clear prompts and audit logs  

- **Sensitive operations**  
  - Confirmations for handle closing, driver/service edits  
  - Warnings for potentially disruptive actions  

- **Data handling**  
  - No telemetry by default  
  - Remote/API disabled unless explicitly enabled  
  - Explicit user consent required for VirusTotal or external lookups  

- **Hardening**  
  - Sandboxed plugin execution  
  - Deny-by-default for unsafe actions  
  - Signed update channel (future roadmap)  
