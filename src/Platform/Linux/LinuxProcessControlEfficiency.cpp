// Efficiency mode on Linux (#3071, #3330, #3331, #3332): LinuxProcessControl::setEfficiencyMode() and the memory of what
// it changed, split from LinuxProcessControl.cpp (#3772). The nice and cgroup-weight rules are LinuxEfficiencyMode.h's.

#include "LinuxEfficiencyMode.h"
#include "LinuxProcessActionsDetail.h"
#include "LinuxProcessControl.h"
#include "Platform/IProcessActions.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform
{

using LinuxProcessDetail::verifyIdentity;

struct LinuxProcessControl::EfficiencyMemory
{
    /// What on found before it changed a process: its nice, and its cgroup's weight when it lowered it.
    struct Before
    {
        std::int32_t nice = 0;
        std::optional<int> cpuWeight;
        /// The cgroup whose weight it lowered (#3331): off puts the weight back there, even if the process has
        /// moved since, and never writes it into the cgroup the process is in now.
        std::optional<std::string> cgroup;
        /// That cgroup's identity (its directory's inode) when on lowered it: off puts nothing into another cgroup
        /// made at the same path since (#3331).
        std::optional<std::uint64_t> cgroupId;
        /// Off ran, put the nice back and left only the weight to put back (#3332): the next on records the nice it
        /// finds, keeping the weight still owed.
        bool turnedOff = false;
    };
    using Key = std::pair<std::int32_t, std::uint64_t>; ///< PID and start time
    std::mutex mutex;                                   ///< Held only to read or change the two below, never over a call
    std::map<Key, Before> byProcess;
    /// The processes a setEfficiencyMode() is changing now (#3330): an on and an off from two objects sharing this
    /// memory would otherwise interleave their read, change and record, and lose what on found. A second one for the
    /// same process meanwhile is refused rather than waited for.
    std::set<Key> busy;
};

namespace
{

/// setEfficiencyMode()'s claim on one process in a memory's busy set, given back when it returns (#3330).
class EfficiencyClaim
{
  public:
    EfficiencyClaim(LinuxProcessControl::EfficiencyMemory& memory, LinuxProcessControl::EfficiencyMemory::Key key)
        : m_Memory(memory), m_Key(key)
    {
        const std::scoped_lock lock(m_Memory.mutex);
        m_Held = m_Memory.busy.insert(m_Key).second;
    }
    ~EfficiencyClaim()
    {
        if (m_Held)
        {
            const std::scoped_lock lock(m_Memory.mutex);
            m_Memory.busy.erase(m_Key);
        }
    }
    EfficiencyClaim(const EfficiencyClaim&) = delete;
    EfficiencyClaim& operator=(const EfficiencyClaim&) = delete;
    EfficiencyClaim(EfficiencyClaim&&) = delete;
    EfficiencyClaim& operator=(EfficiencyClaim&&) = delete;

    /// False when another on or off for the process holds it.
    [[nodiscard]] bool held() const noexcept
    {
        return m_Held;
    }

  private:
    LinuxProcessControl::EfficiencyMemory& m_Memory;
    LinuxProcessControl::EfficiencyMemory::Key m_Key;
    bool m_Held = false;
};

} // namespace

std::shared_ptr<LinuxProcessControl::EfficiencyMemory> LinuxProcessControl::sharedEfficiencyMemory()
{
    static const std::shared_ptr<EfficiencyMemory> shared = makeEfficiencyMemory();
    return shared;
}

std::shared_ptr<LinuxProcessControl::EfficiencyMemory> LinuxProcessControl::makeEfficiencyMemory()
{
    return std::make_shared<EfficiencyMemory>();
}

namespace
{

/// Why Efficiency mode left a process's cgroup weight as it was, for the success line; empty when it lowered it.
[[nodiscard]] std::string lowerCgroupWeight(const LinuxProcessSyscalls& syscalls,
                                            std::int32_t pid,
                                            const std::optional<std::string>& cgroup,
                                            std::optional<std::uint64_t> identity,
                                            std::optional<int>& weightBefore)
{
    namespace Efficiency = LinuxEfficiency;
    if (!cgroup.has_value() || cgroup->empty())
    {
        return "only its priority was lowered: it isn't in a cgroup of its own";
    }
    const std::optional<std::string> procs = syscalls.readCgroupFile(*cgroup, Efficiency::CGROUP_PROCS_FILE);
    if (!procs.has_value() || !Efficiency::onlyProcessIn(*procs, pid))
    {
        return "only its priority was lowered: it shares its cgroup with other processes, which a lower CPU weight would slow too";
    }
    const std::optional<std::string> weightText = syscalls.readCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE);
    const std::optional<int> weight = weightText.has_value() ? Efficiency::parseCpuWeight(*weightText) : std::nullopt;
    if (!weight.has_value())
    {
        return "only its priority was lowered: its cgroup has no CPU controller";
    }
    if (*weight == Efficiency::MIN_CPU_WEIGHT)
    {
        return {};
    }
    const std::string minimum = std::to_string(Efficiency::MIN_CPU_WEIGHT);
    // Into the cgroup whose identity on read, when it read one: one replaced since gets nothing (#3331).
    const int written = identity.has_value() ? syscalls.writeCgroupFileOf(*cgroup, *identity, Efficiency::CPU_WEIGHT_FILE, minimum)
                                             : syscalls.writeCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE, minimum);
    if (written == ESTALE) // NOLINT(misc-include-cleaner) - <cerrno> has it; include-cleaner maps it to a kernel header
    {
        return "only its priority was lowered: its cgroup was replaced";
    }
    if (const int error = written; error != 0)
    {
        return std::format("only its priority was lowered: its cgroup's CPU weight can't be changed ({})",
                           std::system_category().message(error));
    }
    weightBefore = *weight;
    return {};
}

/// What restoreCgroupWeight() did: why it couldn't put the weight back, for the success line (empty when it did, or
/// had nothing to do), and whether a weight on lowered is still to be put back (#3332), so off keeps its memory.
struct WeightRestore
{
    std::string note;
    bool unfinished = false;
};

/// Puts back the cgroup weight Efficiency mode lowered: @p rememberedWeight when it lowered it, else the default when
/// the process is alone in a cgroup at the minimum. A weight on lowered that can't be found, read or written is said,
/// and left unfinished (#3332).
[[nodiscard]] WeightRestore restoreCgroupWeight(const LinuxProcessSyscalls& syscalls,
                                                std::int32_t pid,
                                                const std::optional<std::string>& cgroup,
                                                bool rememberedProcess,
                                                std::optional<int> rememberedWeight,
                                                std::optional<std::uint64_t> identity)
{
    namespace Efficiency = LinuxEfficiency;
    const bool lowered = rememberedProcess && rememberedWeight.has_value(); // on lowered it, so it is ours to put back
    if (rememberedProcess && !rememberedWeight.has_value())
    {
        return {};
    }
    if (!cgroup.has_value() || cgroup->empty())
    {
        if (lowered)
        {
            return {.note = "its cgroup's CPU weight wasn't put back: its cgroup can't be read", .unfinished = true};
        }
        return {};
    }
    if (!rememberedProcess)
    {
        const std::optional<std::string> procs = syscalls.readCgroupFile(*cgroup, Efficiency::CGROUP_PROCS_FILE);
        if (!procs.has_value() || !Efficiency::onlyProcessIn(*procs, pid))
        {
            return {}; // a shared cgroup's weight isn't this process's to put back
        }
    }
    const std::optional<std::string> weightText = syscalls.readCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE);
    const std::optional<int> current = weightText.has_value() ? Efficiency::parseCpuWeight(*weightText) : std::nullopt;
    if (!current.has_value() && lowered)
    {
        return {.note = "its cgroup's CPU weight wasn't put back: it can't be read", .unfinished = true};
    }
    const std::optional<int> restore = Efficiency::weightToRestore(current, rememberedWeight);
    if (!restore.has_value())
    {
        return {}; // not the minimum on set: someone changed it since, and it is theirs
    }
    const std::string value = std::to_string(*restore);
    // Into the cgroup on lowered, checked on the directory written through: one replaced since gets nothing (#3331).
    const int written = identity.has_value() ? syscalls.writeCgroupFileOf(*cgroup, *identity, Efficiency::CPU_WEIGHT_FILE, value)
                                             : syscalls.writeCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE, value);
    if (written == ESTALE) // NOLINT(misc-include-cleaner) - <cerrno> has it; include-cleaner maps it to a kernel header
    {
        return {.note = "its old cgroup's CPU weight wasn't put back: that cgroup is gone", .unfinished = false};
    }
    if (const int error = written; error != 0)
    {
        return {
            .note = std::format("its cgroup's CPU weight is still the minimum: it can't be changed ({})",
                                std::system_category().message(error)),
            .unfinished = lowered,
        };
    }
    return {};
}

} // namespace

ProcessActionResult LinuxProcessControl::setEfficiencyMode(const ProcessTarget& target, bool on)
{
    namespace Efficiency = LinuxEfficiency;
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    const std::pair<std::int32_t, std::uint64_t> key{target.pid, target.startTimeTicks};
    const EfficiencyClaim claim(*m_Efficiency, key); // one on or off at a time per process over the shared memory (#3330)
    if (!claim.held())
    {
        return ProcessActionResult::error("Efficiency mode is already being changed for this process");
    }
    std::optional<EfficiencyMemory::Before> remembered;
    {
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
        {
            remembered = found->second;
        }
    }
    const std::optional<std::int32_t> niceBefore = m_Syscalls.currentNice(target.pid);

    // The nice: nice 19 on, the remembered one (or 0) off unless someone changed it since. setPriority() makes
    // every check a priority change makes (PID 1 and kernel threads refused, the start time confirmed, every
    // thread), and a permission refusal from it needs an Administrator session (#3580).
    std::optional<std::int32_t> nice = Efficiency::EFFICIENCY_NICE;
    if (!on)
    {
        if (remembered.has_value() && remembered->turnedOff)
        {
            // An earlier off already put the nice back (#3332): only the weight is still owed, so the priority is left
            // as it is (a process that was at nice 19 before on is still at its own nice) and only the identity checked.
            nice = std::nullopt;
        }
        else
        {
            const std::optional<std::int32_t> rememberedNice = remembered.has_value() ? std::optional(remembered->nice) : std::nullopt;
            nice = niceBefore.has_value() ? Efficiency::niceToRestore(*niceBefore, rememberedNice) : rememberedNice.value_or(0);
        }
    }
    ProcessActionResult changed = nice.has_value() ? setPriority(target, *nice) : verifyIdentity(target);
    if (!changed.success)
    {
        return changed;
    }

    const std::optional<std::string> cgroupText = m_Syscalls.procCgroup(target.pid);
    const std::optional<std::string> cgroup = cgroupText.has_value() ? Efficiency::unifiedCgroupPath(*cgroupText) : std::nullopt;
    if (on)
    {
        std::optional<int> weightBefore;
        // A weight an earlier off still owes another cgroup (#3331): this on lowers no second one, which would leave
        // one of the two unrecorded.
        const bool owedElsewhere =
            remembered.has_value() && remembered->cpuWeight.has_value() && remembered->cgroup.has_value() && remembered->cgroup != cgroup;
        // Which cgroup the path names now, read before anything is lowered (#3331): a weight is only ever lowered
        // with its cgroup's identity recorded, and a record for a cgroup since replaced at the same path is stale.
        std::optional<std::uint64_t> cgroupId;
        bool identityUnknown = false;
        if (!owedElsewhere && cgroup.has_value() && !cgroup->empty())
        {
            const std::expected<std::uint64_t, int> id = m_Syscalls.cgroupIdentity(*cgroup);
            cgroupId = id.has_value() ? std::optional(*id) : std::nullopt;
            identityUnknown = !id.has_value();
        }
        const bool staleRecord = remembered.has_value() && remembered->cgroup == cgroup && remembered->cgroupId.has_value() &&
                                 cgroupId.has_value() && remembered->cgroupId != cgroupId;
        std::string note;
        if (owedElsewhere)
        {
            note = "only its priority was lowered: its earlier cgroup's CPU weight is still to be put back";
        }
        else if (identityUnknown)
        {
            note = "only its priority was lowered: its cgroup can't be checked";
        }
        else
        {
            note = lowerCgroupWeight(m_Syscalls, target.pid, cgroup, cgroupId, weightBefore);
        }
        {
            // Turned on twice, the first time's "before" stays what off puts back. Processes that have exited since
            // their on are forgotten here, so the shared memory doesn't grow for TaskSmack's lifetime (#3330): one
            // whose PID now holds another process (its start time differs) counts as exited. Read outside the lock.
            std::vector<EfficiencyMemory::Key> entries;
            {
                const std::scoped_lock lock(m_Efficiency->mutex);
                for (const auto& entry : m_Efficiency->byProcess)
                {
                    entries.push_back(entry.first);
                }
            }
            // Kept: this process, and any whose exit isn't certain (a /proc read that failed for another reason).
            std::erase_if(entries,
                          [&key](const EfficiencyMemory::Key& entry)
                          {
                              if (entry == key)
                              {
                                  return true;
                              }
                              const std::optional<std::uint64_t> startNow = LinuxProcessDetail::processStartTicks(entry.first);
                              const bool gone = !startNow.has_value() && LinuxProcessDetail::processGone(entry.first);
                              return !Efficiency::rememberedProcessExited(startNow, gone, entry.second);
                          });
            const std::scoped_lock lock(m_Efficiency->mutex);
            for (const EfficiencyMemory::Key& exited : entries)
            {
                m_Efficiency->byProcess.erase(exited);
            }

            const auto [entry, added] = m_Efficiency->byProcess.try_emplace(key,
                                                                            EfficiencyMemory::Before{
                                                                                .nice = niceBefore.value_or(0),
                                                                                .cpuWeight = weightBefore,
                                                                                .cgroup = weightBefore.has_value() ? cgroup : std::nullopt,
                                                                                .cgroupId = cgroupId,
                                                                            });
            if (!added && staleRecord && !entry->second.turnedOff)
            {
                // Its cgroup was replaced at the same path (#3331): that cgroup's weight is nothing to put back, the
                // replacement's (if this on lowered it) is; the nice from before the first on is still owed.
                entry->second.cpuWeight = weightBefore;
                entry->second.cgroup = weightBefore.has_value() ? cgroup : std::nullopt;
                entry->second.cgroupId = weightBefore.has_value() ? cgroupId : std::nullopt;
            }
            else if (!added && entry->second.turnedOff)
            {
                // After an off that couldn't put the weight back: the nice is the one found now, and the weight
                // still owed (and its cgroup) is kept, unless this on lowered one itself.
                entry->second.nice = niceBefore.value_or(0);
                if (weightBefore.has_value() || staleRecord)
                {
                    entry->second.cpuWeight = weightBefore;
                    entry->second.cgroup = cgroup;
                    entry->second.cgroupId = cgroupId;
                }
                entry->second.turnedOff = false;
            }
            else if (!added && weightBefore.has_value() && !entry->second.cpuWeight.has_value())
            {
                // An earlier on lowered no weight (its cgroup shared then, or its identity unread) and this one did:
                // recorded now, or off would leave it at the minimum (#3331).
                entry->second.cpuWeight = weightBefore;
                entry->second.cgroup = cgroup;
                entry->second.cgroupId = cgroupId;
            }
        }
        spdlog::info("Efficiency mode on for PID {}{}", target.pid, note.empty() ? "" : ": " + note);
        return ProcessActionResult::ok(std::move(note));
    }
    // The cgroup on lowered, where it lowered one (#3331): a process that has moved since has its old cgroup's weight
    // put back, and its new cgroup's left alone. Also when the process's cgroup can't be read now.
    const bool loweredOne = remembered.has_value() && remembered->cgroup.has_value();
    const bool moved = loweredOne && cgroup.has_value() && *cgroup != *remembered->cgroup;
    if (loweredOne && remembered->cgroupId.has_value())
    {
        const std::expected<std::uint64_t, int> id = m_Syscalls.cgroupIdentity(*remembered->cgroup);
        const bool gone = id.has_value() ? *id != *remembered->cgroupId : id.error() == ENOENT;
        if (gone || !id.has_value())
        {
            // Gone, or another cgroup made at its path since: nothing of on's to put back, so forgotten (#3331). One
            // that can't be checked (no permission now, say) may still be it: kept for another off, its nice spent.
            {
                const std::scoped_lock lock(m_Efficiency->mutex);
                if (gone)
                {
                    m_Efficiency->byProcess.erase(key);
                }
                else if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
                {
                    found->second.turnedOff = true;
                }
            }
            std::string why = gone ? "its old cgroup's CPU weight wasn't put back: that cgroup is gone"
                                   : std::format("its cgroup's CPU weight wasn't put back: its cgroup can't be checked ({})",
                                                 std::system_category().message(id.error()));
            spdlog::info("Efficiency mode off for PID {}: {}", target.pid, why);
            return ProcessActionResult::ok(std::move(why));
        }
    }
    WeightRestore weight = restoreCgroupWeight(m_Syscalls,
                                               target.pid,
                                               loweredOne ? remembered->cgroup : cgroup,
                                               remembered.has_value(),
                                               remembered.has_value() ? remembered->cpuWeight : std::nullopt,
                                               loweredOne ? remembered->cgroupId : std::nullopt);
    if (moved && weight.unfinished)
    {
        // The old cgroup is gone or unreadable: a process that left it won't bring it back, so nothing to retry.
        weight.unfinished = false;
        weight.note = "its old cgroup's CPU weight wasn't put back: it has moved to another cgroup, and the old one can't be changed";
    }
    {
        // A weight still to put back keeps the memory, so turning it off again tries once more (#3332); its nice is
        // spent, which the next on sees.
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (!weight.unfinished)
        {
            m_Efficiency->byProcess.erase(key);
        }
        else if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
        {
            found->second.turnedOff = true;
        }
    }
    spdlog::info("Efficiency mode off for PID {}{}", target.pid, weight.note.empty() ? "" : ": " + weight.note);
    return ProcessActionResult::ok(std::move(weight.note));
}

std::size_t LinuxProcessControl::rememberedEfficiencyCount() const
{
    const std::scoped_lock lock(m_Efficiency->mutex);
    return m_Efficiency->byProcess.size();
}

std::optional<std::int32_t> LinuxProcessControl::efficiencyNiceToRestore(const ProcessTarget& target) const
{
    std::optional<std::int32_t> remembered;
    {
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (const auto found = m_Efficiency->byProcess.find({target.pid, target.startTimeTicks});
            found != m_Efficiency->byProcess.end() && !found->second.turnedOff) // a spent nice isn't one to put back (#3332)
        {
            remembered = found->second.nice;
        }
    }
    const std::optional<std::int32_t> current = remembered.has_value() ? m_Syscalls.currentNice(target.pid) : std::nullopt;
    return current.has_value() ? LinuxEfficiency::niceToRestore(*current, remembered) : std::nullopt;
}

} // namespace Platform
