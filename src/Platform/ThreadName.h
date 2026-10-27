#pragma once

// OS thread names, so per-thread tools (pidstat -t, top -H, perf, gdb, WPA, Visual Studio's
// Threads window) show which TaskSmack thread is which (#843 measurement kit). In Platform, the
// lowest layer, so Core and Domain can both name their threads without a layering violation.

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform
{

/// Linux's limit for a thread name (TASK_COMM_LEN 16, including the terminating NUL);
/// pthread_setname_np() fails with ERANGE on anything longer.
inline constexpr std::size_t MAX_LINUX_THREAD_NAME_LENGTH = 15;

/// Thread names TaskSmack gives its own threads. Each fits MAX_LINUX_THREAD_NAME_LENGTH, so the
/// name a tool shows is the whole name on every platform.
inline constexpr std::string_view UI_THREAD_NAME = "tasksmack-ui";
inline constexpr std::string_view SAMPLER_THREAD_NAME = "ts-sampler";
inline constexpr std::string_view PROCESS_SAMPLER_THREAD_NAME = "ts-sampler-proc";
/// The System panel's three samplers (#2731): one per data domain, so a stalled source never delays
/// another's samples. CPU, memory, network and power on the system one; the disk counters and their PDH
/// rescans on the storage one; DXGI, NVML and their rescans on the GPU/NPU one.
inline constexpr std::string_view SYSTEM_SAMPLER_THREAD_NAME = "ts-sampler-sys";
inline constexpr std::string_view STORAGE_SAMPLER_THREAD_NAME = "ts-sampler-disk";
inline constexpr std::string_view GPU_SAMPLER_THREAD_NAME = "ts-sampler-gpu";
inline constexpr std::string_view SERVICE_SAMPLER_THREAD_NAME = "ts-sampler-svc";
/// The sensors' own sampler (#2737): temperatures, fans and voltages, at the Settings interval.
inline constexpr std::string_view SENSOR_SAMPLER_THREAD_NAME = "ts-sampler-sens";
/// The Connections tab's sampler (#3066), running only while the tab is shown.
inline constexpr std::string_view CONNECTIONS_SAMPLER_THREAD_NAME = "ts-sampler-conn";
/// The Startup tab's sampler (#801), running only while the tab is shown.
inline constexpr std::string_view STARTUP_SAMPLER_THREAD_NAME = "ts-sampler-auto";
/// The worker that runs one Process Details Connections read at a time (#799).
inline constexpr std::string_view CONNECTIONS_READ_THREAD_NAME = "ts-conn-read";
/// The worker that runs one Process Details Modules read at a time (#802).
inline constexpr std::string_view MODULES_READ_THREAD_NAME = "ts-mod-read";
/// The worker that runs one Process Details Threads read at a time (#3069).
inline constexpr std::string_view THREADS_READ_THREAD_NAME = "ts-thr-read";
/// The worker that runs one Process Details Open files read at a time (#183).
inline constexpr std::string_view OPEN_FILES_READ_THREAD_NAME = "ts-files-read";
inline constexpr std::string_view SECURITY_READ_THREAD_NAME = "ts-sec-read";
/// The worker that runs one Process Details I/O priority read at a time (#2734).
inline constexpr std::string_view IO_PRIORITY_READ_THREAD_NAME = "ts-ioprio-read";
/// The worker that runs one Process Details Environment read at a time (#2116).
inline constexpr std::string_view ENVIRONMENT_READ_THREAD_NAME = "ts-env-read";
/// The thread that types and names a Windows process's File handles for one read (#183).
inline constexpr std::string_view OPEN_FILES_NAMER_THREAD_NAME = "ts-files-name";
/// The worker that reads the System Information page's static facts (#1399), once or on Refresh.
inline constexpr std::string_view SYSTEM_INFO_READ_THREAD_NAME = "ts-sysinfo-read";
/// The worker that writes the System Information page's "Save report..." file (#2986), one report at a
/// time, so the free-name checks and the write never run on the UI thread.
inline constexpr std::string_view REPORT_SAVE_THREAD_NAME = "ts-report-save";
/// The worker that reads the crash history Process Details shows (#1675), at most once a minute.
inline constexpr std::string_view CRASH_HISTORY_READ_THREAD_NAME = "ts-crash-read";
/// The one-shot worker that reads the battery device's static facts on Windows (#1523, #2731): at most
/// every BATTERY_INFO_REFRESH_SECONDS, so a slow ACPI call never stretches a system sample.
inline constexpr std::string_view BATTERY_READ_THREAD_NAME = "ts-battery-read";
/// The one-shot worker that enumerates and opens the EMI energy meters on Windows (#2587), so the
/// SetupAPI enumeration, which a slow ACPI call can stall, never runs on the UI thread or in a sample.
inline constexpr std::string_view ENERGY_METER_OPEN_THREAD_NAME = "ts-emi-open";
/// The worker that runs one Services tab action (start, stop, ...) at a time (#1577).
inline constexpr std::string_view SERVICE_ACTION_THREAD_NAME = "ts-svc-action";
/// The worker that runs one Startup tab action (enable, disable) at a time (#801).
inline constexpr std::string_view STARTUP_ACTION_THREAD_NAME = "ts-auto-action";
/// The worker that writes one "Create dump file" dump at a time (#807).
inline constexpr std::string_view DUMP_THREAD_NAME = "ts-dump";
/// The one-shot worker that writes config.toml for a settings change made on the UI thread (#2916),
/// coalescing rapid changes into one write of the last state (App/CoalescingWriter.h).
inline constexpr std::string_view SETTINGS_SAVE_THREAD_NAME = "ts-config-save";
/// The worker that runs a confirmed batch action or batch priority change over the Processes table's
/// selection, one batch at a time (#2142).
inline constexpr std::string_view BATCH_ACTION_THREAD_NAME = "ts-batch-action";
/// The worker that runs one single-process action at a time (terminate, kill, suspend, resume,
/// affinity, a trace's launch), so a slow platform call or a password prompt never blocks the UI (#2305).
inline constexpr std::string_view PROCESS_ACTION_THREAD_NAME = "ts-proc-action";
/// The one-shot worker that reads the CPU layout Process Details' Set affinity dialog groups its boxes by
/// (#3182): GetLogicalProcessorInformationEx on Windows, sysfs on Linux, read once and kept.
inline constexpr std::string_view AFFINITY_LAYOUT_READ_THREAD_NAME = "ts-cpu-layout";
/// The worker that shows the dump confirm's "Save dump to..." folder dialog while it is up (#2001).
inline constexpr std::string_view FOLDER_DIALOG_THREAD_NAME = "ts-folder-pick";
/// The worker that reads the dump confirm's folder and free space when it opens or its folder changes
/// (#3155), so a folder on an unplugged drive or an offline share never stalls the UI thread.
inline constexpr std::string_view DUMP_CHECK_THREAD_NAME = "ts-dump-check";
/// The worker that opens one link, file or folder with the system's handler at a time (#3155):
/// ShellExecuteW on Windows, xdg-open on Linux, either of which can wait on a shell extension, COM or
/// the network.
inline constexpr std::string_view SYSTEM_OPEN_THREAD_NAME = "ts-sys-open";
/// The thread that starts TaskSmack's Windows administrator helper, and sits behind its UAC prompt for
/// as long as that is up (PipeBroker::launch(), #2549; named in #2768).
inline constexpr std::string_view HELPER_LAUNCH_THREAD_NAME = "ts-uac-launch";
/// The worker that reads process icons for the Processes table and Process Details (#1800).
inline constexpr std::string_view ICON_READ_THREAD_NAME = "ts-icon-read";
/// The thread that consumes the opt-in UDP event trace on Windows (#1492) while it runs.
inline constexpr std::string_view UDP_TRACE_THREAD_NAME = "ts-udp-trace";
/// The worker that starts or stops the UDP event trace's session when its Settings toggle changes
/// (#3155): the stale-session sweep, the session's start, and the stop's join of the consumer thread.
inline constexpr std::string_view UDP_TRACE_SWITCH_THREAD_NAME = "ts-udp-switch";

/// `name` cut to at most `maxLength` bytes, never in the middle of a UTF-8 sequence.
[[nodiscard]] inline std::string truncateThreadName(std::string_view name, std::size_t maxLength = MAX_LINUX_THREAD_NAME_LENGTH)
{
    if (name.size() <= maxLength)
    {
        return std::string(name);
    }
    std::size_t length = maxLength;
    // Back up over continuation bytes (10xxxxxx) so the cut lands on a code point boundary.
    while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0U) == 0x80U)
    {
        --length;
    }
    return std::string(name.substr(0, length));
}

/// Names the calling thread (Linux: pthread_setname_np, truncated to
/// MAX_LINUX_THREAD_NAME_LENGTH; Windows: SetThreadDescription). Best effort: returns false if the
/// OS refused, and never throws.
bool setCurrentThreadName(std::string_view name) noexcept;

/// Names the main (UI) thread. On Linux this is deliberately a no-op that returns true: the main
/// thread's name IS the process name (/proc/<pid>/comm), which ps, top, pgrep and TaskSmack's own
/// process list show, so renaming it would rename the process. Its TID equals the PID, which is
/// how per-thread tools tell it apart there. On Windows it sets the thread description, which
/// leaves the process name alone.
bool setMainThreadName(std::string_view name) noexcept;

} // namespace Platform
