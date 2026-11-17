#pragma once

#include "Platform/IPathProvider.h"

#include <filesystem>

struct _GUID;

namespace Platform
{

/// SHGetKnownFolderPath, or a test's: the path is allocated with CoTaskMemAlloc, and freed with CoTaskMemFree.
/// Spelled out in primitive types (HRESULT, REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR) to keep windows.h out of this header.
using PathProviderKnownFolderFn = long(__stdcall*)(const _GUID&, unsigned long, void*, wchar_t**);

class WindowsPathProvider : public IPathProvider
{
  public:
    /// Reads the user's config folder through SHGetKnownFolderPath.
    WindowsPathProvider();
    /// Reads it through @p knownFolderPath instead (tests).
    explicit WindowsPathProvider(PathProviderKnownFolderFn knownFolderPath);
    ~WindowsPathProvider() override = default;

    WindowsPathProvider(const WindowsPathProvider&) = delete;
    WindowsPathProvider& operator=(const WindowsPathProvider&) = delete;
    WindowsPathProvider(WindowsPathProvider&&) = delete;
    WindowsPathProvider& operator=(WindowsPathProvider&&) = delete;

    [[nodiscard]] std::filesystem::path getExecutableDir() const override;
    [[nodiscard]] std::filesystem::path getUserConfigDir() const override;

  private:
    PathProviderKnownFolderFn m_KnownFolderPath;
};

} // namespace Platform
