#pragma once

#include "App/DialogGeometry.h"

#include <algorithm>
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
/// Grouped by category: Identity, State, Resources, Scheduling, Time, I/O, Network, Power, GPU, NPU, Command
enum class ProcessColumn : std::uint8_t
{
    // Identity - who is this process?
    PID = 0,
    Name,
    User,
    PPID,
    Publisher, // Software publisher/vendor (Windows-only, empty on Linux)
    Container, // Container from cgroups: "docker:1a2b3c4d5e6f" (Linux-only)
    Services,  // Services the process hosts: "Dhcp, EventLog" (#1880)
               // State - what is it doing?
    State,
    Status,
    Type,         // Process type: App / Background Process / Windows Process (Windows-only)
    Architecture, // The architecture it runs as: x64, ARM64, "x64 (emulated)", "x86 (WOW64)" (#2394)
                  // Resource usage - how much is it consuming?
    CpuPercent,
    MemPercent,
    Resident,
    Virtual,
    Shared,
    PeakResident,
    // Private and Swap: RssAnon and VmSwap on Linux (#1885); neither on Windows (#1879)
    Private,
    Swap,
    // Scheduling - how is it scheduled?
    Priority,
    Policy, // The CPU scheduling policy, "Normal" or "FIFO 50" (Linux, #3086)
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
    // NPU: the NPUs and other compute accelerators, never counted in the GPU columns (#1799)
    NpuPercent,
    NpuMemory,
    NpuEngine,
    // Command line (typically last, fixed width by default for stable horizontal scroll extent)
    Command,
    Count
};

[[nodiscard]] constexpr auto allProcessColumns() -> std::array<ProcessColumn, static_cast<std::size_t>(ProcessColumn::Count)>
{
    // Order matches enum definition: Identity, State, Resources, Scheduling, Time, I/O, Network, Power, GPU, NPU, Command
    return {// Identity
            ProcessColumn::PID,
            ProcessColumn::Name,
            ProcessColumn::User,
            ProcessColumn::PPID,
            ProcessColumn::Publisher,
            ProcessColumn::Container,
            ProcessColumn::Services,
            // State
            ProcessColumn::State,
            ProcessColumn::Status,
            ProcessColumn::Type,
            ProcessColumn::Architecture,
            // Resources
            ProcessColumn::CpuPercent,
            ProcessColumn::MemPercent,
            ProcessColumn::Resident,
            ProcessColumn::Virtual,
            ProcessColumn::Shared,
            ProcessColumn::PeakResident,
            ProcessColumn::Private,
            ProcessColumn::Swap,
            // Scheduling
            ProcessColumn::Priority,
            ProcessColumn::Policy,
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
            // NPU
            ProcessColumn::NpuPercent,
            ProcessColumn::NpuMemory,
            ProcessColumn::NpuEngine,
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
        // Container (Linux-only)
        {.name="Container", .menuName="Container", .configKey="container", .defaultWidth=150.0F, .defaultVisible=false, .canHide=true, .description="Container the process runs in, from its cgroup: runtime and short ID (docker, podman, containerd, cri-o, k8s) or name (lxc, nspawn, machine) (Linux only)"},
        // Services (#1880): off by default; while shown, the service list is kept current
        {.name="Services", .menuName="Services", .configKey="services", .defaultWidth=160.0F, .defaultVisible=false, .canHide=true, .description="Services the process hosts: on Windows those the service manager runs in it (several share an svchost.exe), on Linux the systemd service its cgroup is in. Hover a cell for the full list"},

        // === State ===
        // State
        {.name="State", .menuName="State", .configKey="state", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Process state (R=Running, S=Sleeping, etc.)"},
        // Status
        {.name="Status", .menuName="Status", .configKey="status", .defaultWidth=110.0F, .defaultVisible=false, .canHide=true, .description="Process status (Suspended, Efficiency Mode)"},
        // Type (Windows-only)
        {.name="Type", .menuName="Type", .configKey="type", .defaultWidth=120.0F, .defaultVisible=false, .canHide=true, .description="Process type: App (has UI), Background Process, or Windows Process (Windows only)"},
        // Architecture (#2394)
        {.name="Architecture", .menuName="Architecture", .configKey="architecture", .defaultWidth=110.0F, .defaultVisible=false, .canHide=true, .description="The architecture the process runs as: x64, x86, ARM64, ARM64EC or ARM32. (emulated) marks code translated to this machine's own as it runs (x64 on ARM64), which costs performance; (WOW64) on Windows and (32-bit) on Linux mark a 32-bit program its 64-bit OS runs natively"},

        // === Resources ===
        // CPU%
        {.name="CPU %", .menuName="CPU %", .configKey="cpu_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="CPU usage percentage"},
        // MEM%
        {.name="Mem %", .menuName="Mem %", .configKey="mem_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Memory usage as percentage of total RAM"},
        // RES
        {.name="Memory", .menuName="Memory (Resident)", .configKey="resident", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Physical RAM in use: the private working set on Windows (as Task Manager's Memory column), the resident set size (the resident field of /proc/[pid]/statm, shared pages included) on Linux"},
        // VIRT
        {.name="Virtual", .menuName="Virtual Memory", .configKey="virtual", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Virtual memory: the commit size on Windows (memory committed for the process, as Task Manager's Commit size), the whole address space (vsize from /proc/[pid]/stat) on Linux"},
        // SHR
        {.name="Shared", .menuName="Shared Memory", .configKey="shared", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="Shared memory: the shareable working set on Windows (the pages of its working set that can be shared with other processes, such as DLLs and mapped files: the working set less its private working set), the resident shared pages (the shared field of /proc/[pid]/statm) on Linux"},
        // PEAK RES
        {.name="Peak Mem", .menuName="Peak Memory", .configKey="peak_resident", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Peak resident memory (historical maximum): the peak working set on Windows, shared pages included; VmHWM on Linux"},
        // Private (#1885, #1879)
        {.name="Private", .menuName="Private Memory", .configKey="private", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Memory no other process shares: the resident anonymous pages (RssAnon: heap, stacks, private mappings) on Linux"},
        // Swap (#1885, #1879)
        {.name="Swap", .menuName="Swap", .configKey="swap", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="The part of the process swapped out to disk or zram (VmSwap) on Linux"},

        // === Scheduling ===
        // Priority (human-readable label derived from nice value)
        // Note: configKey remains "nice" for backward compatibility with user config files
        {.name="Priority", .menuName="Priority", .configKey="nice", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="Process priority: Windows priority class (Realtime, High, Above Normal, Normal, Below Normal, Idle), or the nice value's level elsewhere"},
        // Scheduling policy (Linux, #3086)
        {.name="Policy", .menuName="Scheduling Policy", .configKey="policy", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="The CPU scheduling policy (Linux): Normal (SCHED_OTHER), Batch, Idle, Deadline, or real time with its priority, FIFO 50 or RR 10"},
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
        {.name="CPU Time", .menuName="CPU Time", .configKey="cpu_time", .defaultWidth=85.0F, .defaultVisible=true, .canHide=true, .description="Cumulative CPU time, in its two largest units (45s, 2m 05s, 1h 02m, 3d 04h)"},
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

        // === NPU (#1799): NPUs and other compute accelerators, kept out of the GPU columns ===
        // NPU Percent: also shown by default once an accelerator is known (ProcessColumnAvailability::defaultVisible(), #1870)
        {.name="NPU %", .menuName="NPU %", .configKey="npu_percent", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="NPU utilization on the busiest NPU or compute accelerator the process uses (0-100%, as the NPU tab reports it)"},
        // NPU Memory
        {.name="NPU Mem", .menuName="NPU Memory", .configKey="npu_memory", .defaultWidth=85.0F, .defaultVisible=false, .canHide=true, .description="NPU memory in use, counted as the NPU tab counts it: the shared memory an NPU maps from system RAM, or its dedicated memory"},
        // NPU Engine
        {.name="NPU Engine", .menuName="NPU Engine", .configKey="npu_engine", .defaultWidth=100.0F, .defaultVisible=false, .canHide=true, .description="Active NPU engines (Compute, etc.)"},

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
    case ProcessColumn::Container:
    case ProcessColumn::Services:
    case ProcessColumn::Status:
    case ProcessColumn::Type:
    case ProcessColumn::Architecture:
    case ProcessColumn::Policy:
    case ProcessColumn::GpuEngine:
    case ProcessColumn::GpuDevice:
    case ProcessColumn::NpuEngine:
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
/// 0 B/s (#1101). With UDP counted (Windows' opt-in UDP event trace, #1492), they say that instead.
/// @param hasUdpNetworkCounters  ProcessCapabilities::hasUdpNetworkCounters.
[[nodiscard]] constexpr auto columnCapabilityNote(ProcessColumn col, bool hasUdpNetworkCounters) -> std::string_view
{
    if (col != ProcessColumn::NetSent && col != ProcessColumn::NetReceived)
    {
        return {};
    }
    if (hasUdpNetworkCounters)
    {
        return "TCP and UDP: UDP is counted by a Windows network event trace (Settings)";
    }
    return "TCP only: UDP traffic (QUIC/HTTP3, video calls, games, DNS) is not counted";
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

/// Room a column's widest value is given beyond its own text, in ems: half an em clear of the
/// column's edge on each side (#1280). ImGui's CellPadding is added outside a column's width, but it
/// is only a few pixels, and a label filling the rest reads as touching the column border.
inline constexpr float COLUMN_CONTENT_MARGIN_EM = 1.0F;

/// Default width of a column whose values are a fixed, known set of labels (Priority's): the scaled
/// authored width, or the widest label plus COLUMN_CONTENT_MARGIN_EM if that is wider, so the
/// longest label ("Above Normal", "Below Normal") fits in whatever font is active (#1280).
/// @param scaledDefault    scaledDefaultWidth() for the column.
/// @param widestContentPx  The widest label's width in the current font (0 if not measured yet).
/// @param emPx             One em, i.e. ImGui::GetFontSize().
[[nodiscard]] inline auto contentFittedWidth(float scaledDefault, float widestContentPx, float emPx) noexcept -> float
{
    if (!std::isfinite(emPx) || emPx <= 0.0F || !std::isfinite(widestContentPx) || widestContentPx <= 0.0F)
    {
        return scaledDefault;
    }
    return std::max(scaledDefault, widestContentPx + (COLUMN_CONTENT_MARGIN_EM * emPx));
}

/// Column visibility settings for persistence
struct ProcessColumnSettings
{
    std::array<bool, processColumnCount()> visible{};
    /// Columns whose visibility was chosen -- by the user, or loaded from the config file -- rather
    /// than left at a default. Only the others follow the system's capabilities (#1210, see
    /// ProcessColumnAvailability::applyCapabilityDefaults()).
    std::array<bool, processColumnCount()> chosen{};

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

    /// Shows or hides `col` as chosen (by the user or the config file), marking it chosen.
    void setVisible(ProcessColumn col, bool vis)
    {
        visible[toIndex(col)] = vis;
        chosen[toIndex(col)] = true;
    }

    void toggleVisible(ProcessColumn col)
    {
        setVisible(col, !isVisible(col));
    }

    /// Takes `col`'s visibility from the table (ImGui's state) when it differs, and returns whether it
    /// did. Only `userChange` -- a toggle in ImGui's header menu -- counts as a choice; state ImGui
    /// produced itself, such as a column layout it restored, is taken without marking the column
    /// chosen, so it keeps following this system's defaults (#1210).
    bool adoptTableVisibility(ProcessColumn col, bool enabled, bool userChange)
    {
        if (isVisible(col) == enabled)
        {
            return false;
        }
        if (userChange)
        {
            setVisible(col, enabled);
        }
        else
        {
            visible[toIndex(col)] = enabled;
        }
        return true;
    }

    /// Whether `col`'s visibility was chosen rather than left at a default.
    [[nodiscard]] bool isChosen(ProcessColumn col) const
    {
        return chosen[toIndex(col)];
    }

    /// Whether `col` saved as `vis` in a config written before the sparse [process_columns] format
    /// (UserConfig's CONFIG_FORMAT_VERSION, #1376) was the user's choice. Those configs listed every
    /// column, chosen or not, so only a value other than the column's default can be told apart as a
    /// choice; one equal to it is taken for a column the user never touched. The default compared
    /// against is getColumnInfo()'s, as those configs were written with, not this system's
    /// capability-aware one (ProcessColumnAvailability::defaultColumns()). A column the user had
    /// deliberately set back to its default is indistinguishable, and follows the defaults again.
    [[nodiscard]] static bool isLegacyChoice(ProcessColumn col, bool vis)
    {
        return vis != getColumnInfo(col).defaultVisible;
    }

    /// Sets the default visibility of a column whose visibility was not chosen; one that was is left alone.
    void setDefaultVisible(ProcessColumn col, bool vis)
    {
        if (!isChosen(col))
        {
            visible[toIndex(col)] = vis;
        }
    }

    /// Shows or hides `col` as the Columns menu asks (#1209); a column that cannot be hidden (PID,
    /// Name) stays shown.
    void requestVisible(ProcessColumn col, bool vis)
    {
        setVisible(col, vis || !getColumnInfo(col).canHide);
    }

    /// Shows every column that cannot be hidden (PID, Name: getColumnInfo().canHide), whatever was
    /// asked for (#1209). A config file may say "pid = false", and the table's
    /// TableSetColumnEnabled(false) ignores ImGui's NoHide flag, so loaded and requested visibility
    /// is passed through this before it reaches the table.
    void keepUnhideableColumnsVisible()
    {
        for (const auto col : allProcessColumns())
        {
            if (!getColumnInfo(col).canHide)
            {
                visible[toIndex(col)] = true;
            }
        }
    }

    /// The default column set regardless of what the system can fill; see
    /// ProcessColumnAvailability::defaultColumns() for what "Reset columns" restores (#1209, #1210).
    [[nodiscard]] static ProcessColumnSettings defaults()
    {
        return {};
    }

    friend bool operator==(const ProcessColumnSettings&, const ProcessColumnSettings&) = default;
};

} // namespace App
