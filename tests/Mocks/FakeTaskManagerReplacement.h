/// @file FakeTaskManagerReplacement.h
/// @brief Replace Task Manager in memory (#3072), for the App side's controller, Settings rows and notice:
/// read() reports @c state, change() records what it was asked and, when it succeeds, moves the state.

#pragma once

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Mocks
{

class FakeTaskManagerReplacement final : public Platform::ITaskManagerReplacement
{
  public:
    using State = Platform::TaskManagerReplacement::State;

    [[nodiscard]] Platform::TaskManagerReplacementInfo read() override
    {
        const std::scoped_lock lock(m_Mutex);
        ++reads;
        Platform::TaskManagerReplacementInfo info;
        info.status.state = state;
        info.status.program = program;
        info.selfPath = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
        info.otherProgramExists = otherProgramExists;
        info.installedCopy = installedCopy;
        info.unavailableReason = unavailableReason;
        return info;
    }

    [[nodiscard]] Platform::ProcessActionResult
    change(Platform::Broker::TaskManagerChange change, bool replaceOther, const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        changes.emplace_back(change, replaceOther);
        if (answer.success)
        {
            state = (change == Platform::Broker::TaskManagerChange::Set) ? State::ThisCopy : State::Off;
        }
        return answer;
    }

    // Set before the controller reads; read after its work is waited for.
    State state = State::Off;
    std::string program;
    bool otherProgramExists = true;
    bool installedCopy = true;
    std::string unavailableReason;
    Platform::ProcessActionResult answer = Platform::ProcessActionResult::ok("done");
    int reads = 0;
    std::vector<std::pair<Platform::Broker::TaskManagerChange, bool>> changes;

  private:
    std::mutex m_Mutex;
};

} // namespace Mocks
