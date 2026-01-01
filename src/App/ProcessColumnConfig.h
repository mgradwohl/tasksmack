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
enum class ProcessColumn : std::uint8_t
{
    PID = 0,
    User,
    CpuPercent,
    MemPercent,
    Virtual,
    Resident,
    PeakResident,
    Shared,
    CpuTime,
    State,
    Status,
    Name,
    PPID,
    Nice,
    Threads,
    PageFaults,
    Affinity,
    Command,
    IoRead,
    IoWrite,
    Power,
    NetSent,
    NetReceived,
    GpuPercent,
    GpuMemory,
    GpuEngine,
    GpuDevice,
    Count
};

[[nodiscard]] constexpr auto allProcessColumns() -> std::array<ProcessColumn, static_cast<std::size_t>(ProcessColumn::Count)>
{
    return {ProcessColumn::PID,       ProcessColumn::User,        ProcessColumn::CpuPercent,   ProcessColumn::MemPercent,
            ProcessColumn::Virtual,   ProcessColumn::Resident,    ProcessColumn::PeakResident, ProcessColumn::Shared,
            ProcessColumn::CpuTime,   ProcessColumn::State,       ProcessColumn::Status,       ProcessColumn::Name,
            ProcessColumn::PPID,      ProcessColumn::Nice,        ProcessColumn::Threads,      ProcessColumn::PageFaults,
            ProcessColumn::Affinity,  ProcessColumn::Command,     ProcessColumn::IoRead,       ProcessColumn::IoWrite,
            ProcessColumn::NetSent,   ProcessColumn::NetReceived, ProcessColumn::Power,        ProcessColumn::GpuPercent,
            ProcessColumn::GpuMemory, ProcessColumn::GpuEngine,   ProcessColumn::GpuDevice};
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
    constexpr std::array<ProcessColumnInfo, processColumnCount()> infos = {{
        // PID - always visible
        {.name="PID", .menuName="PID", .configKey="pid", .defaultWidth=60.0F, .defaultVisible=true, .canHide=false, .description="Process ID"},
        // User
        {.name="User", .menuName="User", .configKey="user", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Process owner"},
        // CPU%
        {.name="CPU %", .menuName="CPU %", .configKey="cpu_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="CPU usage percentage"},
        // MEM%
        {.name="MEM %", .menuName="MEM %", .configKey="mem_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Memory usage as percentage of total RAM"},
        // VIRT
        {.name="VIRT", .menuName="Virtual Memory", .configKey="virtual", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Virtual memory size"},
        // RES
        {.name="RES", .menuName="Resident Memory", .configKey="resident", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Resident memory (physical RAM used)"},
        // PEAK RES
        {.name="PEAK", .menuName="Peak Resident", .configKey="peak_resident", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Peak resident memory (historical maximum)"},
        // SHR
        {.name="SHR", .menuName="Shared Memory", .configKey="shared", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="Shared memory size"},
        // TIME+
        {.name="TIME+", .menuName="CPU Time", .configKey="cpu_time", .defaultWidth=85.0F, .defaultVisible=true, .canHide=true, .description="Cumulative CPU time (H:MM:SS.cc)"},
        // State
        {.name="S", .menuName="State", .configKey="state", .defaultWidth=25.0F, .defaultVisible=true, .canHide=true, .description="Process state (R=Running, S=Sleeping, etc.)"},
        // Status
        {.name="Status", .menuName="Status", .configKey="status", .defaultWidth=110.0F, .defaultVisible=false, .canHide=true, .description="Process status (Suspended, Efficiency Mode)"},
        // Name
        {.name="Name", .menuName="Name", .configKey="name", .defaultWidth=120.0F, .defaultVisible=true, .canHide=false, .description="Process name"},
        // PPID
        {.name="PPID", .menuName="Parent PID", .configKey="ppid", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Parent process ID"},
        // Nice
        {.name="NI", .menuName="Nice", .configKey="nice", .defaultWidth=35.0F, .defaultVisible=false, .canHide=true, .description="Nice value (priority, -20 to 19)"},
        // Threads
        {.name="THR", .menuName="Threads", .configKey="threads", .defaultWidth=45.0F, .defaultVisible=false, .canHide=true, .description="Thread count"},
        // Page Faults
        {.name="PF", .menuName="Page Faults", .configKey="page_faults", .defaultWidth=75.0F, .defaultVisible=false, .canHide=true, .description="Total page faults (cumulative)"},
        // Affinity
        {.name="Affinity", .menuName="CPU Affinity", .configKey="affinity", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="CPU cores this process can run on"},
        // Command
        {.name="Command", .menuName="Command Line", .configKey="command", .defaultWidth=0.0F, .defaultVisible=true, .canHide=true, .description="Full command line (0 = stretch)"},
        // I/O Read
        {.name="I/O Read", .menuName="I/O Read", .configKey="io_read", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk read rate (bytes/sec)"},
        // I/O Write
        {.name="I/O Write", .menuName="I/O Write", .configKey="io_write", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk write rate (bytes/sec)"},
        // Power
        {.name="Power", .menuName="Power", .configKey="power", .defaultWidth=100.0F, .defaultVisible=true, .canHide=true, .description="Power consumption in watts (platform-dependent)"},
        // Net Sent
        {.name="Net Sent", .menuName="Net Sent", .configKey="net_sent", .defaultWidth=90.0F, .defaultVisible=true, .canHide=true, .description="Network send rate (bytes/sec)"},
        // Net Received
        {.name="Net Recv", .menuName="Net Received", .configKey="net_recv", .defaultWidth=90.0F, .defaultVisible=true, .canHide=true, .description="Network receive rate (bytes/sec)"},
        // GPU Percent
        {.name="GPU %", .menuName="GPU %", .configKey="gpu_percent", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="GPU utilization percentage (aggregated across all GPUs)"},
        // GPU Memory
        {.name="GPU Mem", .menuName="GPU Memory", .configKey="gpu_memory", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="GPU memory allocated (VRAM)"},
        // GPU Engine
        {.name="GPU Engine", .menuName="GPU Engine", .configKey="gpu_engine", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="Active GPU engines (3D, Compute, Video, etc.)"},
        // GPU Device
        {.name="GPU Dev", .menuName="GPU Device", .configKey="gpu_device", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Which GPU(s) the process is using"},
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
