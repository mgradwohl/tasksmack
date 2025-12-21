#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

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
    Shared,
    CpuTime,
    State,
    Name,
    PPID,
    Nice,
    Threads,
    Command,
    // Future columns (data not yet available):
    // IoRead,
    // IoWrite,
    Count // Must be last
};

[[nodiscard]] constexpr auto allProcessColumns() -> std::array<ProcessColumn, 14>
{
    // Keep in sync with ProcessColumn enum (excluding Count).
    return {
        ProcessColumn::PID,
        ProcessColumn::User,
        ProcessColumn::CpuPercent,
        ProcessColumn::MemPercent,
        ProcessColumn::Virtual,
        ProcessColumn::Resident,
        ProcessColumn::Shared,
        ProcessColumn::CpuTime,
        ProcessColumn::State,
        ProcessColumn::Name,
        ProcessColumn::PPID,
        ProcessColumn::Nice,
        ProcessColumn::Threads,
        ProcessColumn::Command,
    };
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
    std::string_view name;        // Display name in header
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
        {.name="PID", .configKey="pid", .defaultWidth=60.0F, .defaultVisible=true, .canHide=false, .description="Process ID"},
        // User
        {.name="User", .configKey="user", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Process owner"},
        // CPU%
        {.name="CPU %", .configKey="cpu_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="CPU usage percentage"},
        // MEM%
        {.name="MEM %", .configKey="mem_percent", .defaultWidth=55.0F, .defaultVisible=true, .canHide=true, .description="Memory usage as percentage of total RAM"},
        // VIRT
        {.name="VIRT", .configKey="virtual", .defaultWidth=80.0F, .defaultVisible=false, .canHide=true, .description="Virtual memory size"},
        // RES
        {.name="RES", .configKey="resident", .defaultWidth=80.0F, .defaultVisible=true, .canHide=true, .description="Resident memory (physical RAM used)"},
        // SHR
        {.name="SHR", .configKey="shared", .defaultWidth=70.0F, .defaultVisible=false, .canHide=true, .description="Shared memory size"},
        // TIME+
        {.name="TIME+", .configKey="cpu_time", .defaultWidth=85.0F, .defaultVisible=true, .canHide=true, .description="Cumulative CPU time (H:MM:SS.cc)"},
        // State
        {.name="S", .configKey="state", .defaultWidth=25.0F, .defaultVisible=true, .canHide=true, .description="Process state (R=Running, S=Sleeping, etc.)"},
        // Name
        {.name="Name", .configKey="name", .defaultWidth=120.0F, .defaultVisible=true, .canHide=false, .description="Process name"},
        // PPID
        {.name="PPID", .configKey="ppid", .defaultWidth=60.0F, .defaultVisible=false, .canHide=true, .description="Parent process ID"},
        // Nice
        {.name="NI", .configKey="nice", .defaultWidth=35.0F, .defaultVisible=false, .canHide=true, .description="Nice value (priority, -20 to 19)"},
        // Threads
        {.name="THR", .configKey="threads", .defaultWidth=45.0F, .defaultVisible=false, .canHide=true, .description="Thread count"},
        // Command
        {.name="Command", .configKey="command", .defaultWidth=0.0F, .defaultVisible=true, .canHide=true, .description="Full command line (0 = stretch)"},
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
        for (ProcessColumn col : allProcessColumns())
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
