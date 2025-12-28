#pragma once

#include "Platform/IPathProvider.h"

#include <filesystem>

namespace Platform
{

class LinuxPathProvider : public IPathProvider
{
  public:
    LinuxPathProvider() = default;
    ~LinuxPathProvider() override = default;

    LinuxPathProvider(const LinuxPathProvider&) = delete;
    LinuxPathProvider& operator=(const LinuxPathProvider&) = delete;
    LinuxPathProvider(LinuxPathProvider&&) = delete;
    LinuxPathProvider& operator=(LinuxPathProvider&&) = delete;

    [[nodiscard]] std::filesystem::path getExecutableDir() const override;
    [[nodiscard]] std::filesystem::path getUserConfigDir() const override;
};

} // namespace Platform
