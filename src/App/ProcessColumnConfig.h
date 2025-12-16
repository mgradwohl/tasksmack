#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace App
{

/// All available columns for the process table.
/// Order here defines the default column order.
enum class ProcessColumn : uint8_t
{
    PID = 0,
    User,
    CpuPercent,
    MemPercent,
    Virtual,
    Resident,
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
    constexpr std::array<ProcessColumnInfo, static_cast<size_t>(ProcessColumn::Count)> infos = {{
        // PID - always visible
        {"PID", "pid", 60.0F, true, false, "Process ID"},
        // User
        {"User", "user", 80.0F, true, true, "Process owner"},
        // CPU%
        {"CPU %", "cpu_percent", 55.0F, true, true, "CPU usage percentage"},
        // MEM%
        {"MEM %", "mem_percent", 55.0F, true, true, "Memory usage as percentage of total RAM"},
        // VIRT
        {"VIRT", "virtual", 80.0F, false, true, "Virtual memory size"},
        // RES
        {"RES", "resident", 80.0F, true, true, "Resident memory (physical RAM used)"},
        // TIME+
        {"TIME+", "cpu_time", 85.0F, true, true, "Cumulative CPU time (H:MM:SS.cc)"},
        // State
        {"S", "state", 25.0F, true, true, "Process state (R=Running, S=Sleeping, etc.)"},
        // Name
        {"Name", "name", 120.0F, true, false, "Process name"},
        // PPID
        {"PPID", "ppid", 60.0F, false, true, "Parent process ID"},
        // Nice
        {"NI", "nice", 35.0F, false, true, "Nice value (priority, -20 to 19)"},
        // Threads
        {"THR", "threads", 45.0F, false, true, "Thread count"},
        // Command
        {"Command", "command", 0.0F, true, true, "Full command line (0 = stretch)"},
    }};
    // clang-format on

    return infos[static_cast<size_t>(col)];
}

/// Column visibility settings for persistence
struct ProcessColumnSettings
{
    std::array<bool, static_cast<size_t>(ProcessColumn::Count)> visible{};

    ProcessColumnSettings()
    {
        // Initialize with defaults
        for (size_t i = 0; i < static_cast<size_t>(ProcessColumn::Count); ++i)
        {
            visible[i] = getColumnInfo(static_cast<ProcessColumn>(i)).defaultVisible;
        }
    }

    [[nodiscard]] bool isVisible(ProcessColumn col) const
    {
        return visible[static_cast<size_t>(col)];
    }

    void setVisible(ProcessColumn col, bool vis)
    {
        visible[static_cast<size_t>(col)] = vis;
    }

    void toggleVisible(ProcessColumn col)
    {
        auto idx = static_cast<size_t>(col);
        visible[idx] = !visible[idx];
    }
};

} // namespace App
