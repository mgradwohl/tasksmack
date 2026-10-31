#pragma once

// ITaskManagerReplacement over the platform's calls (#3072): Windows' factory hands it the registry read,
// the helper and, for an elevated TaskSmack installed in Program Files, the registry itself
// (Platform::makeTaskManagerReplacement()). Platform-neutral, so the tests run it everywhere with a fake
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
    /// ... or straight to this registry, for an elevated TaskSmack installed in Program Files.
    std::shared_ptr<TaskManagerReplacement::IIfeoRegistry> direct;
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
    /// @p request through the helper: started first (its UAC prompt) when it isn't running.
    [[nodiscard]] ProcessActionResult changeThroughHelper(const Broker::TaskManagerReplacementRequest& request,
                                                          const std::stop_token& stopToken) const;

    TaskManagerReplacementSetup m_Setup;
};

} // namespace Platform
