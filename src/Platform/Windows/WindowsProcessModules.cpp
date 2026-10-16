#include "WindowsProcessModules.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcessIdentity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// The most file versions kept: past it the cache starts over, so browsing many processes cannot grow
/// it without bound. A desktop's distinct module paths number in the low thousands.
constexpr std::size_t MAX_CACHED_VERSIONS = 4096;

/// The longest path GetModuleFileNameExW can return (an extended-length path), in characters.
constexpr DWORD MAX_MODULE_PATH = 32768;

/// Tries at listing the modules before taking what fits: the list grows as the process loads more.
constexpr int MAX_ENUM_ATTEMPTS = 3;

/// VS_FIXEDFILEINFO::dwSignature, telling a real fixed-info block from garbage.
constexpr DWORD FIXED_FILE_INFO_SIGNATURE = 0xFEEF04BD;

[[nodiscard]] ModulesReadResult failure(ModulesReadStatus status, std::string detail = {})
{
    return {.status = status, .modules = {}, .detail = std::move(detail)};
}

/// The system's message for a Win32 error code ("Only part of a ReadProcessMemory ... was completed").
[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

} // namespace

bool WindowsProcessModulesReader::hasModules() const
{
    return true;
}

ModulesReadResult WindowsProcessModulesReader::readModules(const ProcessTarget& target)
{
    // PROCESS_VM_READ: the module list lives in the process's own memory (its PEB loader data).
    // Protected processes, and elevated ones from an unelevated TaskSmack, refuse it.
    auto opened =
        openProcessVerified({.openProcess = m_Api.openProcess, .getProcessTimes = m_Api.getProcessTimes, .closeHandle = m_Api.closeHandle},
                            target,
                            PROCESS_VM_READ);
    if (!opened)
    {
        auto refused = toReadFailure<ModulesReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    std::vector<HMODULE> handles(256);
    for (int attempt = 1;; ++attempt)
    {
        const auto bytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
        DWORD needed = 0;
        if (m_Api.enumProcessModulesEx(process.get(), handles.data(), bytes, &needed, LIST_MODULES_ALL) == 0)
        {
            const DWORD error = GetLastError();
            DWORD exitCode = 0;
            if (m_Api.getExitCodeProcess(process.get(), &exitCode) != 0 && exitCode != STILL_ACTIVE)
            {
                return failure(ModulesReadStatus::ProcessExited);
            }
            if (error == ERROR_ACCESS_DENIED)
            {
                return failure(ModulesReadStatus::PermissionDenied);
            }
            // ERROR_PARTIAL_COPY: a process still starting has no loader data yet; retried at the next refresh.
            return failure(ModulesReadStatus::Failed, errorText(error));
        }
        if (needed <= bytes || attempt == MAX_ENUM_ATTEMPTS)
        {
            handles.resize(std::min(needed, bytes) / sizeof(HMODULE));
            break;
        }
        handles.resize((needed / sizeof(HMODULE)) + 32); // room for a few more loaded in between
    }

    ModulesReadResult result{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    result.modules.reserve(handles.size());
    std::wstring buffer(MAX_MODULE_PATH, L'\0');
    for (HMODULE module : handles)
    {
        const DWORD length = m_Api.getModuleFileNameEx(process.get(), module, buffer.data(), MAX_MODULE_PATH);
        MODULEINFO info{};
        if (length == 0 || m_Api.getModuleInformation(process.get(), module, &info, sizeof(info)) == 0)
        {
            continue; // unloaded since the listing
        }
        const std::wstring path(buffer.data(), length);
        result.modules.push_back({.path = WinString::wideToUtf8(path),
                                  .baseAddress = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll),
                                  .sizeBytes = info.SizeOfImage,
                                  .version = fileVersion(path),
                                  .deleted = false});
    }
    return result;
}

std::optional<ModuleVersion> WindowsProcessModulesReader::fileVersion(const std::wstring& path)
{
    if (const auto cached = m_VersionCache.find(path); cached != m_VersionCache.end())
    {
        return cached->second;
    }
    std::optional<ModuleVersion> version;
    DWORD ignored = 0;
    if (const DWORD size = m_Api.getFileVersionInfoSize(path.c_str(), &ignored); size > 0)
    {
        std::vector<std::byte> block(size);
        void* data = nullptr;
        UINT dataBytes = 0;
        if (m_Api.getFileVersionInfo(path.c_str(), 0, size, block.data()) != 0 &&
            m_Api.verQueryValue(block.data(), L"\\", &data, &dataBytes) != 0 && data != nullptr && dataBytes >= sizeof(VS_FIXEDFILEINFO))
        {
            // Points into block, which is alive here.
            const auto* fixed = static_cast<const VS_FIXEDFILEINFO*>(data);
            if (fixed->dwSignature == FIXED_FILE_INFO_SIGNATURE)
            {
                version = ModuleVersion{.major = HIWORD(fixed->dwFileVersionMS),
                                        .minor = LOWORD(fixed->dwFileVersionMS),
                                        .build = HIWORD(fixed->dwFileVersionLS),
                                        .revision = LOWORD(fixed->dwFileVersionLS)};
            }
        }
    }
    if (m_VersionCache.size() >= MAX_CACHED_VERSIONS)
    {
        m_VersionCache.clear();
    }
    m_VersionCache.emplace(path, version);
    return version;
}

} // namespace Platform::Windows
