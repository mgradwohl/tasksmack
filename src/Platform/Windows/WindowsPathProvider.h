#pragma once

#include "Platform/IPathProvider.h"

#include <filesystem>

namespace Platform
{

class WindowsPathProvider : public IPathProvider
{
  public:
    WindowsPathProvider() = default;
    ~WindowsPathProvider() override = default;

    WindowsPathProvider(const WindowsPathProvider&) = delete;
    WindowsPathProvider& operator=(const WindowsPathProvider&) = delete;
    WindowsPathProvider(WindowsPathProvider&&) = delete;
    WindowsPathProvider& operator=(WindowsPathProvider&&) = delete;

    [[nodiscard]] std::filesystem::path getExecutableDir() const override;
    [[nodiscard]] std::filesystem::path getUserConfigDir() const override;
};

} // namespace Platform
