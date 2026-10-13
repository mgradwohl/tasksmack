#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "WindowsAffinityMath.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <cstdint>
#include <vector>

namespace Platform
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The Win32 calls WindowsProcessActions makes to open and identify a process and to set its CPU
/// affinity, as a table so tests can make each fail as Windows would (#1881): access denied, a PID
/// reused, a refused mask. realWindowsProcessFunctions() is the real one. The calls return and set
/// GetLastError() as the APIs do.
struct WindowsProcessFunctions
{
    HANDLE(WINAPI* openProcess)(DWORD access, BOOL inherit, DWORD pid) = nullptr;
    BOOL(WINAPI* getProcessTimes)(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) = nullptr;
    BOOL(WINAPI* setProcessAffinityMask)(HANDLE process, DWORD_PTR mask) = nullptr;
    /// SetProcessDefaultCpuSets(): ids null and count 0 clear the process's default CPU sets.
    BOOL(WINAPI* setProcessDefaultCpuSets)(HANDLE process, const ULONG* ids, ULONG count) = nullptr;
    /// Each processor group's maximum processor count (GetMaximumProcessorCount), in group order;
    /// empty when it can't be read. Fixed for the boot session, so read once, at construction.
    std::vector<std::uint32_t> (*readGroupSizes)() = nullptr;
    /// The physical cores (GetLogicalProcessorInformationEx, RelationProcessorCore); empty on failure.
    std::vector<ProcessorCoreRecord> (*readCores)() = nullptr;
    /// Every CPU set (GetSystemCpuSetInformation); empty on failure.
    std::vector<CpuSetRecord> (*readCpuSets)() = nullptr;
};

/// The real Win32 calls.
[[nodiscard]] WindowsProcessFunctions realWindowsProcessFunctions() noexcept;

// NOLINTEND(misc-include-cleaner)

/// Windows implementation of IProcessActions.
/// Uses TerminateProcess and related APIs. Each action opens a handle to the PID, confirms the
/// process's creation time matches the target's, and acts through that handle. The handle pins the
/// process object, so once the check passes a later reuse of the PID cannot redirect the action.
class WindowsProcessActions : public IProcessActions
{
  public:
    WindowsProcessActions();
    /// As above, making its Win32 calls through @p functions (tests: calls that fail on demand).
    explicit WindowsProcessActions(WindowsProcessFunctions functions);
    ~WindowsProcessActions() override = default;

    WindowsProcessActions(const WindowsProcessActions&) = delete;
    WindowsProcessActions& operator=(const WindowsProcessActions&) = delete;
    WindowsProcessActions(WindowsProcessActions&&) = default;
    WindowsProcessActions& operator=(WindowsProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    /// Windows has no ionice equivalent (#803): refused, and canSetIoPriority is false.
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    /// Refused, as setIoPriority() is.
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    /// One processor group: SetProcessAffinityMask(), which every thread, and every thread started
    /// later, follows. Several: SetProcessDefaultCpuSets(), since one affinity mask can't span groups
    /// (#1881); every CPU chosen clears the default CPU sets instead.
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;

  private:
    /// Helper to terminate a process with given exit code
    [[nodiscard]] ProcessActionResult terminateProcess(const ProcessTarget& target, uint32_t exitCode) const;

    WindowsProcessFunctions m_Fns;
    std::vector<std::uint32_t> m_GroupSizes; // Each processor group's maximum processor count, read at construction
};

} // namespace Platform
