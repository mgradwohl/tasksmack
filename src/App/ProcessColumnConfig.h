#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace App
{

/// All available columns for the process table.
/// Order here defines the default column order.
/// Grouped by category: Identity, State, Resources, Scheduling, Time, I/O, Network, Power, GPU, Command
enum class ProcessColumn : std::uint8_t
{
    // Identity - who is this process?
    PID = 0,
    Name,
    User,
    PPID,
    // State - what is it doing?
    State,
    Status,
    // Resource usage - how much is it consuming?
    CpuPercent,
    MemPercent,
    Resident,
    Virtual,
    Shared,
    PeakResident,
    // Scheduling - how is it scheduled?
    Nice,
    Affinity,
    Threads,
    Handles,
    // Time metrics
    CpuTime,
    StartTime,
    // I/O - disk activity
    IoRead,
    IoWrite,
    PageFaults,
    // Network
    NetSent,
    NetReceived,
    // Power
    Power,
    // GPU
    GpuPercent,
    GpuMemory,
    GpuEngine,
    GpuDevice,
    // Command line (typically last, stretches to fill)
    Command,
    Count
};

[[nodiscard]] constexpr auto allProcessColumns() -> std::array<ProcessColumn, static_cast<std::size_t>(ProcessColumn::Count)>
{
    // Order matches enum definition: Identity, State, Resources, Scheduling, Time, I/O, Network, Power, GPU, Command
    return {// Identity
            ProcessColumn::PID,
            ProcessColumn::Name,
            ProcessColumn::User,
            ProcessColumn::PPID,
            // State
            ProcessColumn::State,
            ProcessColumn::Status,
            // Resources
            ProcessColumn::CpuPercent,
            ProcessColumn::MemPercent,
            ProcessColumn::Resident,
            ProcessColumn::Virtual,
            ProcessColumn::Shared,
            ProcessColumn::PeakResident,
            // Scheduling
            ProcessColumn::Nice,
            ProcessColumn::Affinity,
            ProcessColumn::Threads,
            ProcessColumn::Handles,
            // Time
            ProcessColumn::CpuTime,
            ProcessColumn::StartTime,
            // I/O
            ProcessColumn::IoRead,
            ProcessColumn::IoWrite,
            ProcessColumn::PageFaults,
            // Network
            ProcessColumn::NetSent,
            ProcessColumn::NetReceived,
            // Power
            ProcessColumn::Power,
            // GPU
            ProcessColumn::GpuPercent,
            ProcessColumn::GpuMemory,
            ProcessColumn::GpuEngine,
            ProcessColumn::GpuDevice,
            // Command (last)
            ProcessColumn::Command};
}

[[nodiscard]] constexpr auto processColumnCount() -> std::size_t
{
    return allProcessColumns().size();
}

[[nodiscard]] constexpr auto toIndex(ProcessColumn col) -> std::size_t
{
    // Explicit: ProcessColumn is a small, contiguous enum used as an index.
    return static_cast<std::size_t>(std::to_underlying(col));
}

/// Column metadata for display and configuration
struct ProcessColumnInfo
{
    std::string_view name;        // Display name in header (can be short like "S")
    std::string_view menuName;    // Display name in context menu (full name like "State")
    std::string_view configKey;   // Key used in config file
    float defaultWidth;           // Default column width
    bool defaultVisible;          // Visible by default
    bool canHide;                 // Whether user can hide this column
    std::string_view description; // Tooltip description
};

/// Get metadata for a column
constexpr auto getColumnInfo(ProcessColumn col) -> ProcessColumnInfo
{
    // clang-format off
    // Array order MUST match ProcessColumn enum order
    constexpr std::array<ProcessColumnInfo, processColumnCount()> infos = {{
        // === Identity ===
        // PID - always visible
        {.name="PID", .menuName="PID", .configKey="pid", .defaultWidth=60.0F, .defaultVisible=true, .canHide=false, .description="Process ID"},
        // Name - always visible
        {.name="Name", .menuName="Name", .configKey="name", .defaultWidth=120.0F, .defaultVisible=true, .canHide=false, .description="Process name"},
        // User
        {.name="User", .menuName="User", .configKey="user", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Process owner"},
        // PPID
        {.name="PPID", .menuName="Parent PID", .configKey="ppid", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Parent process ID"},

        // === State ===
        // State
        {.name="S", .menuName="State", .configKey="state", .defaultWidth=25.0F, .defaultVisible=true, .canHide=true, .description="Process state (R=Running, S=Sleeping, etc.)"},
        // Status
        {.name="Status", .menuName="Status", .configKey="status", .defaultWidth=110.0F, .defaultVisible=false, .canHide=true, .description="Process status (Suspended, Efficiency Mode)"},

        // === Resources ===
        // CPU%
        {.name="CPU %", .menuName="CPU %", .configKey="cpu_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="CPU usage percentage"},
        // MEM%
        {.name="MEM %", .menuName="MEM %", .configKey="mem_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Memory usage as percentage of total RAM"},
        // RES
        {.name="RES", .menuName="Resident Memory", .configKey="resident", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Resident memory (physical RAM used)"},
        // VIRT
        {.name="VIRT", .menuName="Virtual Memory", .configKey="virtual", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Virtual memory size"},
        // SHR
        {.name="SHR", .menuName="Shared Memory", .configKey="shared", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="Shared memory size"},
        // PEAK RES
        {.name="PEAK", .menuName="Peak Resident", .configKey="peak_resident", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Peak resident memory (historical maximum)"},

        // === Scheduling ===
        // Nice
        {.name="NI", .menuName="Nice", .configKey="nice", .defaultWidth=35.0F, .defaultVisible=false, .canHide=true, .description="Nice value (priority, -20 to 19)"},
        // Affinity
        {.name="Affinity", .menuName="CPU Affinity", .configKey="affinity", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="CPU cores this process can run on"},
        // Threads
        {.name="THR", .menuName="Threads", .configKey="threads", .defaultWidth=45.0F, .defaultVisible=false, .canHide=true, .description="Thread count"},
        // Handles (Windows) / File Descriptors (Linux)
        {.name="Handles", .menuName="Handles/FDs", .configKey="handles", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Handle count (Windows) / File descriptor count (Linux)"},

        // === Time ===
        // TIME+
        {.name="TIME+", .menuName="CPU Time", .configKey="cpu_time", .defaultWidth=85.0F, .defaultVisible=true, .canHide=true, .description="Cumulative CPU time (H:MM:SS.cc)"},
        // Start Time
        {.name="Started", .menuName="Start Time", .configKey="start_time", .defaultWidth=140.0F, .defaultVisible=false, .canHide=true, .description="Process start time"},

        // === I/O ===
        // I/O Read
        {.name="I/O Read", .menuName="I/O Read", .configKey="io_read", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk read rate (bytes/sec)"},
        // I/O Write
        {.name="I/O Write", .menuName="I/O Write", .configKey="io_write", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk write rate (bytes/sec)"},
        // Page Faults
        {.name="PF", .menuName="Page Faults", .configKey="page_faults", .defaultWidth=75.0F, .defaultVisible=false, .canHide=true, .description="Total page faults (cumulative)"},

        // === Network ===
        // Net Sent
        {.name="Net Sent", .menuName="Net Sent", .configKey="net_sent", .defaultWidth=90.0F, .defaultVisible=true, .canHide=true, .description="Network send rate (bytes/sec)"},
        // Net Received
        {.name="Net Recv", .menuName="Net Received", .configKey="net_recv", .defaultWidth=90.0F, .defaultVisible=true, .canHide=true, .description="Network receive rate (bytes/sec)"},

        // === Power ===
        // Power
        {.name="Power", .menuName="Power", .configKey="power", .defaultWidth=100.0F, .defaultVisible=true, .canHide=true, .description="Power consumption in watts (platform-dependent)"},

        // === GPU ===
        // GPU Percent
        {.name="GPU %", .menuName="GPU %", .configKey="gpu_percent", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="GPU utilization percentage (aggregated across all GPUs)"},
        // GPU Memory
        {.name="GPU Mem", .menuName="GPU Memory", .configKey="gpu_memory", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="GPU memory allocated (VRAM)"},
        // GPU Engine
        {.name="GPU Engine", .menuName="GPU Engine", .configKey="gpu_engine", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="Active GPU engines (3D, Compute, Video, etc.)"},
        // GPU Device
        {.name="GPU Dev", .menuName="GPU Device", .configKey="gpu_device", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Which GPU(s) the process is using"},

        // === Command (last, stretches) ===
        // Command
        {.name="Command", .menuName="Command Line", .configKey="command", .defaultWidth=0.0F, .defaultVisible=true, .canHide=true, .description="Full command line (0 = stretch)"},
    }};
    // clang-format on

    return infos[toIndex(col)];
}

/// Column visibility settings for persistence
struct ProcessColumnSettings
{
    std::array<bool, processColumnCount()> visible{};

    ProcessColumnSettings()
    {
        // Initialize with defaults
        for (const auto col : allProcessColumns())
        {
            visible[toIndex(col)] = getColumnInfo(col).defaultVisible;
        }
    }

    [[nodiscard]] bool isVisible(ProcessColumn col) const
    {
        return visible[toIndex(col)];
    }

    void setVisible(ProcessColumn col, bool vis)
    {
        visible[toIndex(col)] = vis;
    }

    void toggleVisible(ProcessColumn col)
    {
        const std::size_t idx = toIndex(col);
        visible[idx] = !visible[idx];
    }
};

} // namespace App
