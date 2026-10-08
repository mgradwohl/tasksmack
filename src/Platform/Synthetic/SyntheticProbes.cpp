#include "SyntheticProbes.h"

#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"
#include "SyntheticWorkload.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace Platform::Synthetic
{

namespace
{

[[nodiscard]] ProcessActionResult refuse(const ProcessTarget& target)
{
    return ProcessActionResult::error(std::format("Process {} is synthetic (TASKSMACK_SYNTHETIC); action not sent", target.pid));
}

} // namespace

SyntheticProcessProbe::SyntheticProcessProbe(std::shared_ptr<const Workload> workload) : m_Workload(std::move(workload))
{}

std::vector<ProcessCounters> SyntheticProcessProbe::enumerate()
{
    m_LastUptime = m_Workload->uptimeAt(std::chrono::steady_clock::now());
    std::vector<ProcessCounters> processes;
    m_Workload->processesAt(m_LastUptime, processes);
    return processes;
}

ProcessCapabilities SyntheticProcessProbe::capabilities() const
{
    return Workload::processCapabilities();
}

std::uint64_t SyntheticProcessProbe::totalCpuTime() const
{
    return m_Workload->totalCpuTicksAt(m_LastUptime);
}

long SyntheticProcessProbe::ticksPerSecond() const
{
    return Workload::TICKS_PER_SECOND;
}

std::uint64_t SyntheticProcessProbe::systemTotalMemory() const
{
    return m_Workload->totalMemoryBytes();
}

SyntheticSystemProbe::SyntheticSystemProbe(std::shared_ptr<const Workload> workload) : m_Workload(std::move(workload))
{}

SystemCounters SyntheticSystemProbe::read()
{
    SystemCounters counters;
    m_Workload->systemCountersAt(m_Workload->uptimeAt(std::chrono::steady_clock::now()), counters);
    return counters;
}

SystemCapabilities SyntheticSystemProbe::capabilities() const
{
    return Workload::systemCapabilities();
}

long SyntheticSystemProbe::ticksPerSecond() const
{
    return Workload::TICKS_PER_SECOND;
}

SyntheticDiskProbe::SyntheticDiskProbe(std::shared_ptr<const Workload> workload) : m_Workload(std::move(workload))
{}

SystemDiskCounters SyntheticDiskProbe::read()
{
    SystemDiskCounters counters;
    m_Workload->diskCountersAt(m_Workload->uptimeAt(std::chrono::steady_clock::now()), counters);
    return counters;
}

DiskCapabilities SyntheticDiskProbe::capabilities() const
{
    return Workload::diskCapabilities();
}

ProcessActionCapabilities SyntheticProcessActions::actionCapabilities() const
{
    return {};
}

ProcessActionResult SyntheticProcessActions::terminate(const ProcessTarget& target)
{
    return refuse(target);
}

ProcessActionResult SyntheticProcessActions::kill(const ProcessTarget& target)
{
    return refuse(target);
}

ProcessActionResult SyntheticProcessActions::stop(const ProcessTarget& target)
{
    return refuse(target);
}

ProcessActionResult SyntheticProcessActions::resume(const ProcessTarget& target)
{
    return refuse(target);
}

ProcessActionResult SyntheticProcessActions::setPriority(const ProcessTarget& target, [[maybe_unused]] std::int32_t nice)
{
    return refuse(target);
}

ProcessActionResult SyntheticProcessActions::setIoPriority(const ProcessTarget& target,
                                                           [[maybe_unused]] IoPriorityClass ioClass,
                                                           [[maybe_unused]] std::int32_t level)
{
    return refuse(target);
}

IoPriorityReadResult SyntheticProcessActions::getIoPriority(const ProcessTarget& target)
{
    return std::unexpected(refuse(target).errorMessage);
}

} // namespace Platform::Synthetic
