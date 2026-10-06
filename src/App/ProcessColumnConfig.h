#pragma once

#include "App/DialogGeometry.h"

#include <array>
#include <cmath>
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
    Publisher, // Software publisher/vendor (Windows-only, empty on Linux)
               // State - what is it doing?
    State,
    Status,
    Type, // Process type: App / Background Process / Windows Process (Windows-only)
          // Resource usage - how much is it consuming?
    CpuPercent,
    MemPercent,
    Resident,
    Virtual,
    Shared,
    PeakResident,
    // Scheduling - how is it scheduled?
    Priority,
    Affinity,
    Threads,
    Handles,
    GdiObjects, // GDI object count (Windows-only)
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
    // Command line (typically last, fixed width by default for stable horizontal scroll extent)
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
            ProcessColumn::Publisher,
            // State
            ProcessColumn::State,
            ProcessColumn::Status,
            ProcessColumn::Type,
            // Resources
            ProcessColumn::CpuPercent,
            ProcessColumn::MemPercent,
            ProcessColumn::Resident,
            ProcessColumn::Virtual,
            ProcessColumn::Shared,
            ProcessColumn::PeakResident,
            // Scheduling
            ProcessColumn::Priority,
            ProcessColumn::Affinity,
            ProcessColumn::Threads,
            ProcessColumn::Handles,
            ProcessColumn::GdiObjects,
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
    std::string_view name;        // Display name in header: a plain word, never a single letter or htop jargon like "TIME+" (#1203)
    std::string_view menuName;    // Display name in context menu (may be longer, like "Memory (Resident)")
    std::string_view configKey;   // Key used in config file
    float defaultWidth;           // Default column width in px at REFERENCE_EM_PX; see scaledDefaultWidth()
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
        // Publisher (Windows-only)
        {.name="Publisher", .menuName="Publisher", .configKey="publisher", .defaultWidth=140.0F, .defaultVisible=false, .canHide=true, .description="Software publisher or vendor name from the process executable (Windows only)"},

        // === State ===
        // State
        {.name="State", .menuName="State", .configKey="state", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Process state (R=Running, S=Sleeping, etc.)"},
        // Status
        {.name="Status", .menuName="Status", .configKey="status", .defaultWidth=110.0F, .defaultVisible=false, .canHide=true, .description="Process status (Suspended, Efficiency Mode)"},
        // Type (Windows-only)
        {.name="Type", .menuName="Type", .configKey="type", .defaultWidth=120.0F, .defaultVisible=false, .canHide=true, .description="Process type: App (has UI), Background Process, or Windows Process (Windows only)"},

        // === Resources ===
        // CPU%
        {.name="CPU %", .menuName="CPU %", .configKey="cpu_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="CPU usage percentage"},
        // MEM%
        {.name="Mem %", .menuName="Mem %", .configKey="mem_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Memory usage as percentage of total RAM"},
        // RES
        {.name="Memory", .menuName="Memory (Resident)", .configKey="resident", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Resident memory (physical RAM used)"},
        // VIRT
        {.name="Virtual", .menuName="Virtual Memory", .configKey="virtual", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Virtual memory size"},
        // SHR
        {.name="Shared", .menuName="Shared Memory", .configKey="shared", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="Shared memory size"},
        // PEAK RES
        {.name="Peak Mem", .menuName="Peak Memory", .configKey="peak_resident", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Peak resident memory (historical maximum)"},

        // === Scheduling ===
        // Priority (human-readable label derived from nice value)
        // Note: configKey remains "nice" for backward compatibility with user config files
        {.name="Priority", .menuName="Priority", .configKey="nice", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Process priority (High, Above Normal, Normal, Below Normal, Idle)"},
        // Affinity
        {.name="Affinity", .menuName="CPU Affinity", .configKey="affinity", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="CPU cores this process can run on"},
        // Threads
        {.name="Threads", .menuName="Threads", .configKey="threads", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Thread count"},
        // Handles (Windows) / File Descriptors (Linux)
        {.name="Handles", .menuName="Handles/FDs", .configKey="handles", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Handle count (Windows) / File descriptor count (Linux)"},
        // GDI Objects (Windows-only)
        {.name="GDI", .menuName="GDI Objects", .configKey="gdi_objects", .defaultWidth=50.0F, .defaultVisible=false, .canHide=true, .description="GDI object count (Windows only) — number of GDI handles used by the process"},

        // === Time ===
        // TIME+
        {.name="CPU Time", .menuName="CPU Time", .configKey="cpu_time", .defaultWidth=85.0F, .defaultVisible=true, .canHide=true, .description="Cumulative CPU time (H:MM:SS, or M:SS under an hour)"},
        // Start Time
        {.name="Started", .menuName="Start Time", .configKey="start_time", .defaultWidth=140.0F, .defaultVisible=false, .canHide=true, .description="Process start time"},

        // === I/O ===
        // I/O Read
        {.name="I/O Read", .menuName="I/O Read", .configKey="io_read", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk read rate (bytes/sec)"},
        // I/O Write
        {.name="I/O Write", .menuName="I/O Write", .configKey="io_write", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Disk write rate (bytes/sec)"},
        // Page Faults
        {.name="Page Faults", .menuName="Page Faults", .configKey="page_faults", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Total page faults (cumulative)"},

        // === Network ===
        // Net Sent
        {.name="Net Sent", .menuName="Net Sent", .configKey="net_sent", .defaultWidth=90.0F, .defaultVisible=true, .canHide=true, .description="Network send rate (bytes/sec)"},
        // Net Received
        {.name="Net Received", .menuName="Net Received", .configKey="net_recv", .defaultWidth=100.0F, .defaultVisible=true, .canHide=true, .description="Network receive rate (bytes/sec)"},

        // === Power ===
        // Power
        {.name="Power", .menuName="Power", .configKey="power", .defaultWidth=100.0F, .defaultVisible=true, .canHide=true, .description="Power consumption in watts (platform-dependent)"},

        // === GPU ===
        // GPU Percent
        {.name="GPU %", .menuName="GPU %", .configKey="gpu_percent", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="GPU utilization on the busiest GPU the process uses (0-100%, as the GPU tab reports it)"},
        // GPU Memory
        {.name="GPU Mem", .menuName="GPU Memory", .configKey="gpu_memory", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="GPU memory in use, counted as the GPU tab counts it: VRAM on a discrete GPU, shared memory on an integrated one (Windows)"},
        // GPU Engine
        {.name="GPU Engine", .menuName="GPU Engine", .configKey="gpu_engine", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="Active GPU engines (3D, Compute, Video, etc.)"},
        // GPU Device
        {.name="GPU", .menuName="GPU Device", .configKey="gpu_device", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Which GPU(s) the process is using"},

        // === Command (last, fixed-width by default) ===
        // Command
        {.name="Command", .menuName="Command Line", .configKey="command", .defaultWidth=420.0F, .defaultVisible=true, .canHide=true, .description="Full command line"},
    }};
    // clang-format on

    return infos[toIndex(col)];
}

/// How a column's cells are aligned, which its header follows (#1209).
enum class ColumnAlign : std::uint8_t
{
    Left,
    Center,
    Right,
};

/// The alignment of a column's cells: numbers, sizes, rates, times and counts right, the one-letter
/// State code centred, free text left. ProcessesPanel::renderProcessRow() draws each column this way,
/// and the header is aligned to match, so a numeric header sits over its numbers (#1209).
[[nodiscard]] constexpr auto columnAlignment(ProcessColumn col) -> ColumnAlign
{
    switch (col)
    {
    case ProcessColumn::Name:
    case ProcessColumn::User:
    case ProcessColumn::Publisher:
    case ProcessColumn::Status:
    case ProcessColumn::Type:
    case ProcessColumn::GpuEngine:
    case ProcessColumn::GpuDevice:
    case ProcessColumn::Command:
        return ColumnAlign::Left;
    case ProcessColumn::State:
        return ColumnAlign::Center;
    default:
        return ColumnAlign::Right;
    }
}

/// What a column's header tooltip adds to its description about what the platform leaves out, or
/// empty. The network columns say when they count TCP only: UDP traffic -- QUIC/HTTP3, video calls,
/// games, DNS -- isn't attributed per process, and a browser streaming over HTTP/3 reads about
/// 0 B/s (#1101).
/// @param hasUdpNetworkCounters  ProcessCapabilities::hasUdpNetworkCounters.
[[nodiscard]] constexpr auto columnCapabilityNote(ProcessColumn col, bool hasUdpNetworkCounters) -> std::string_view
{
    if ((col == ProcessColumn::NetSent || col == ProcessColumn::NetReceived) && !hasUdpNetworkCounters)
    {
        return "TCP only: UDP traffic (QUIC/HTTP3, video calls, games, DNS) is not counted";
    }
    return {};
}

/// Default width of a column in pixels at the current font.
///
/// The widths in getColumnInfo() are authored in pixels at the reference em (the Medium preset on a
/// 1.0 display scale), where they are sized to their content -- Name's 120px holds about 24
/// characters there. Handing those pixels to ImGui unscaled pinned them to that one font: the table
/// settings are not persisted (ImGui's ini is disabled), so every launch laid the columns out from
/// the raw pixel values whatever font was active. At Even Huger the same 120px held 14 characters
/// and clipped names the kernel had already capped at 15; at Small every column was a third wider
/// than its content needed (#913).
///
/// ImGui rescales a live table's widths itself when the font changes (ImGuiTable::RefScale), so
/// this only has to make the *starting* widths font-relative.
///
/// @param info  Column metadata from getColumnInfo().
/// @param emPx  One em, i.e. ImGui::GetFontSize().
/// @return Width in pixels; the authored width unchanged if emPx is not a usable size.
[[nodiscard]] inline auto scaledDefaultWidth(const ProcessColumnInfo& info, float emPx) noexcept -> float
{
    if (!std::isfinite(emPx) || emPx <= 0.0F)
    {
        return info.defaultWidth;
    }
    return info.defaultWidth * (emPx / REFERENCE_EM_PX);
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

    /// Shows or hides `col` as the Columns menu asks (#1209); a column that cannot be hidden (PID,
    /// Name) stays shown.
    void requestVisible(ProcessColumn col, bool vis)
    {
        setVisible(col, vis || !getColumnInfo(col).canHide);
    }

    /// The default column set, what the Columns menu's "Reset columns" restores (#1209).
    [[nodiscard]] static ProcessColumnSettings defaults()
    {
        return {};
    }

    /// Whether every column is shown or hidden as it is by default.
    [[nodiscard]] bool isDefault() const
    {
        return visible == defaults().visible;
    }

    friend bool operator==(const ProcessColumnSettings&, const ProcessColumnSettings&) = default;
};

} // namespace App
