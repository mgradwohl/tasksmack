#pragma once

// ITaskManagerReplacement over the platform's calls (#3072): Windows' factory hands it the registry read,
// the helper and, for an elevated TaskSmack, the registry itself, which it turns on only for a copy no one
// but an administrator can change (Platform::makeTaskManagerReplacement()). Platform-neutral, so the tests run it everywhere with a fake
// registry and a fake broker (tests/Platform/test_TaskManagerReplacement.cpp).

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform
{

/// What TaskManagerReplacementService is made of: the platform's calls, or a test's.
struct TaskManagerReplacementSetup
{
    std::string selfPath;
    /// The Debugger value: nullopt when there is none.
    std::function<std::expected<std::optional<std::string>, std::string>()> readDebugger;
    /// Whether a program the value names exists.
    std::function<bool(std::string_view)> fileExists;
    /// Writes go through this helper when it is set ...
    std::shared_ptr<IPrivilegedBroker> broker;
    /// ... or straight to this registry, for an elevated TaskSmack.
    std::shared_ptr<TaskManagerReplacement::IIfeoRegistry> direct;
    /// Whether @c direct may point Task Manager at this copy (#3280): run on the worker, by each read and
    /// again before each Set, never at construction. A Clear never needs it: removing a TaskSmack's
    /// Debugger is always allowed. Unset, a Set through @c direct needs nothing more.
    std::function<std::expected<void, std::string>()> checkDirectSet;
    /// What read() reports, and a Set answers, while checkDirectSet fails.
    std::string directSetRefusal;
    /// Why neither is set.
    std::string unavailableReason;
    bool installedCopy = false;
};

/// ITaskManagerReplacement over TaskManagerReplacementSetup's calls. A Set always names this copy
/// (@c selfPath), never a path from anywhere else.
class TaskManagerReplacementService final : public ITaskManagerReplacement
{
  public:
    explicit TaskManagerReplacementService(TaskManagerReplacementSetup setup);

    [[nodiscard]] TaskManagerReplacementInfo read() override;
    [[nodiscard]] ProcessActionResult
    change(Broker::TaskManagerChange change, bool replaceOther, const std::stop_token& stopToken) override;

  private:
    /// Why a Set through @c direct is refused now (checkDirectSet failed), or nullopt when it isn't.
    [[nodiscard]] std::optional<std::string> directSetRefused() const;

    /// @p request through the helper: started first (its UAC prompt) when it isn't running.
    [[nodiscard]] ProcessActionResult changeThroughHelper(const Broker::TaskManagerReplacementRequest& request,
                                                          const std::stop_token& stopToken) const;

    TaskManagerReplacementSetup m_Setup;
};

} // namespace Platform
