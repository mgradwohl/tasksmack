#pragma once

// Replace Task Manager on Windows (#3072): the registry behind TaskManagerReplacement.h's logic, and what
// TaskSmackHelper.exe does with a TaskManagerReplacementRequest.
//
// WindowsIfeoRegistry reads and writes taskmgr.exe's Image File Execution Options key in the registry's
// 64-bit view. TaskSmack reads it unelevated (reading HKLM needs no rights), on a worker; only the helper,
// or an elevated TaskSmack installed in Program Files, writes it. The key is a constructor argument so
// the tests point it at a scratch key under HKEY_CURRENT_USER and never touch the real one.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/BrokerProtocol.h"
#include "Platform/TaskManagerReplacement.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

/// taskmgr.exe's IFEO key, or a test's key, in the registry's 64-bit view.
class WindowsIfeoRegistry final : public TaskManagerReplacement::IIfeoRegistry
{
  public:
    /// HKEY_LOCAL_MACHINE's taskmgr.exe key (TaskManagerReplacement::IFEO_KEY).
    WindowsIfeoRegistry();
    /// @p subKey under @p root: the tests' scratch key.
    WindowsIfeoRegistry(HKEY root, std::wstring subKey);

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> readDebugger() override;
    [[nodiscard]] std::expected<bool, std::string> createKey() override;
    [[nodiscard]] std::expected<void, std::string> writeDebugger(std::string_view value) override;
    [[nodiscard]] std::expected<void, std::string> deleteDebugger() override;
    [[nodiscard]] std::expected<bool, std::string> readCreatedMarker() override;
    [[nodiscard]] std::expected<void, std::string> writeCreatedMarker() override;
    [[nodiscard]] std::expected<void, std::string> deleteCreatedMarker() override;
    [[nodiscard]] std::expected<bool, std::string> deleteKeyIfEmpty() override;

  private:
    HKEY m_Root;
    std::wstring m_SubKey;
};

/// Whether two paths name the same file (BrokerHelper::sameFile()), or a test's stand-in.
using SameFileFn = bool (*)(const std::wstring& first, const std::wstring& second);

/// TaskSmackHelper.exe's answer to @p request (#3072), writing @p registry. A Set is taken only for the
/// TaskSmack.exe the helper serves, @p besideTaskSmack (the one beside the helper, in an install only an
/// administrator can change): the path must pass TaskManagerReplacement::validateTaskSmackPath(), be that
/// path as text (letters ignoring case), and, through @p sameFile, that very file, so a link elsewhere
/// that resolves to it isn't written either. A Clear removes only a TaskSmack's Debugger.
[[nodiscard]] Broker::ActionResult handleHelperRequest(const Broker::TaskManagerReplacementRequest& request,
                                                       const std::wstring& besideTaskSmack,
                                                       TaskManagerReplacement::IIfeoRegistry& registry,
                                                       SameFileFn sameFile);

/// The folders an installed TaskSmack is under: %ProgramFiles%, %ProgramW6432% and %ProgramFiles(x86)%,
/// as set, in UTF-8.
[[nodiscard]] std::vector<std::string> installFolders();

/// Whether @p path (UTF-8) names an existing file.
[[nodiscard]] bool fileExists(std::string_view path);

} // namespace Platform::Windows

#endif // _WIN32
