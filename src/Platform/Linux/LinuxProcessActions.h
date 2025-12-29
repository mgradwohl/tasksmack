#pragma once

#include "Platform/IProcessActions.h"

#include <string_view>

namespace Platform
{

/// Linux implementation of IProcessActions.
/// Uses POSIX signals via kill(2).
class LinuxProcessActions : public IProcessActions
{
  public:
    LinuxProcessActions() = default;
    ~LinuxProcessActions() override = default;

    LinuxProcessActions(const LinuxProcessActions&) = delete;
    LinuxProcessActions& operator=(const LinuxProcessActions&) = delete;
    LinuxProcessActions(LinuxProcessActions&&) = default;
    LinuxProcessActions& operator=(LinuxProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(int32_t pid) override;
    [[nodiscard]] ProcessActionResult kill(int32_t pid) override;
    [[nodiscard]] ProcessActionResult stop(int32_t pid) override;
    [[nodiscard]] ProcessActionResult resume(int32_t pid) override;

  private:
    [[nodiscard]] static ProcessActionResult sendSignal(int32_t pid, int signal, std::string_view signalName);
};

} // namespace Platform
