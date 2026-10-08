#pragma once

// Platform probe implementations over a synthetic Workload (#1413). Each reads the workload at the
// current steady_clock time; probes made from one shared Workload agree on its clock, so the process,
// system and disk readings describe the same machine at the same moment.
//
// Constructed only by the App composition root, and only when TASKSMACK_SYNTHETIC is set: see
// App/SyntheticScenario.h. Nothing in a normal run creates or calls them.

#include "Platform/IDiskProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"
#include "SyntheticWorkload.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Platform::Synthetic
{

class SyntheticProcessProbe : public IProcessProbe
{
  public:
    explicit SyntheticProcessProbe(std::shared_ptr<const Workload> workload);

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    /// The total at the time of the latest enumerate(), so both cover the same interval (#1119).
    [[nodiscard]] std::uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] std::uint64_t systemTotalMemory() const override;

  private:
    std::shared_ptr<const Workload> m_Workload;
    double m_LastUptime = 0.0;
};

class SyntheticSystemProbe : public ISystemProbe
{
  public:
    explicit SyntheticSystemProbe(std::shared_ptr<const Workload> workload);

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    std::shared_ptr<const Workload> m_Workload;
};

class SyntheticDiskProbe : public IDiskProbe
{
  public:
    explicit SyntheticDiskProbe(std::shared_ptr<const Workload> workload);

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

  private:
    std::shared_ptr<const Workload> m_Workload;
};

/// Synthetic PIDs are not real processes -- some may well be the PIDs of real ones -- so every action
/// is refused and none is offered.
class SyntheticProcessActions : public IProcessActions
{
  public:
    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, std::int32_t nice) override;
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, std::int32_t level) override;
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
};

} // namespace Platform::Synthetic
