#include "WindowsProcessMemoryMap.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "WinString.h"
#include "WindowsDevicePaths.h"
#include "WindowsHandles.h"
#include "WindowsProcessIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// The longest name GetMappedFileNameW returns (an NT path), in characters.
constexpr DWORD MAX_MAPPED_NAME = 32768;

[[nodiscard]] MemoryMapReadResult failure(MemoryMapReadStatus status, std::string detail = {})
{
    return {.status = status, .mappings = {}, .detail = std::move(detail), .truncated = false};
}

/// Whether @p process has exited: its exit code is one other than STILL_ACTIVE (a failed query reads as running).
[[nodiscard]] bool hasExited(const ProcessMemoryMapFunctions& api, HANDLE process)
{
    DWORD exitCode = 0;
    return api.getExitCodeProcess(process, &exitCode) != 0 && exitCode != STILL_ACTIVE;
}

/// Names the files behind the regions of one read: GetMappedFileNameW once per allocation (an image's or a
/// view's regions share their allocation base), as a drive path. The drive letters' devices are read on
/// the first name that needs them.
class FileNamer
{
  public:
    FileNamer(const ProcessMemoryMapFunctions& api, HANDLE process) : m_Api(&api), m_Process(process)
    {}

    /// The file behind @p info's region, UTF-8, or empty when there is none (a pagefile-backed section) or
    /// it can't be named.
    [[nodiscard]] std::string name(const MEMORY_BASIC_INFORMATION& info)
    {
        const auto allocation = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
        if (const auto known = m_Names.find(allocation); known != m_Names.end())
        {
            return known->second;
        }
        if (m_Buffer.empty())
        {
            m_Buffer.resize(MAX_MAPPED_NAME);
        }
        // The region's own address, not its allocation's: a view reserved in part may have no page at its base.
        const DWORD length = m_Api->getMappedFileName(m_Process, info.BaseAddress, m_Buffer.data(), MAX_MAPPED_NAME);
        if (length == 0 || length >= MAX_MAPPED_NAME)
        {
            // A committed page that names no file means the allocation has none (a pagefile-backed
            // section): asked once. A reserved one says nothing: another region of it may still name it.
            if (info.State == MEM_COMMIT)
            {
                m_Names.emplace(allocation, std::string{});
            }
            return {};
        }
        if (!m_Drives.has_value())
        {
            m_Drives = WindowsDevicePaths::driveDevices(m_Api->getLogicalDrives, m_Api->queryDosDevice);
        }
        std::string path =
            WinString::wideToUtf8(WindowsDevicePaths::ntPathToDosPath(std::wstring_view(m_Buffer.data(), length), *m_Drives));
        m_Names.emplace(allocation, path);
        return path;
    }

  private:
    const ProcessMemoryMapFunctions* m_Api; // the reader's, which outlives this read
    HANDLE m_Process;
    std::wstring m_Buffer;
    std::optional<std::vector<WindowsDevicePaths::DriveDevice>> m_Drives;
    std::unordered_map<std::uintptr_t, std::string> m_Names;
};

} // namespace

bool WindowsProcessMemoryMapReader::hasMemoryMap() const
{
    return true;
}

MemoryMapReadResult WindowsProcessMemoryMapReader::readMemoryMap(const ProcessTarget& target)
{
    const ProcessIdentityFunctions identity{
        .openProcess = m_Api.openProcess,
        .getProcessTimes = m_Api.getProcessTimes,
        .closeHandle = m_Api.closeHandle,
        .getExitCodeProcess = m_Api.getExitCodeProcess,
    };
    // VirtualQueryEx and GetMappedFileNameW are documented to need PROCESS_QUERY_INFORMATION. A process that
    // refuses it (a protected one, or an elevated one from an unelevated TaskSmack) may still grant the
    // limited right, which recent Windows accepts for both; if it doesn't, the first query says so.
    auto opened = openProcessVerified(identity, target, PROCESS_QUERY_INFORMATION);
    if (!opened && opened.error().kind == IdentityFailureKind::AccessDenied)
    {
        opened = openProcessVerified(identity, target, 0);
    }
    if (!opened)
    {
        auto refused = toReadFailure<MemoryMapReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    MemoryMapReadResult result{.status = MemoryMapReadStatus::Ok, .mappings = {}, .detail = {}, .truncated = false};
    FileNamer files(m_Api, process.get());
    std::uintptr_t address = 0;
    for (;;)
    {
        MEMORY_BASIC_INFORMATION info{};
        // NOLINTNEXTLINE(performance-no-int-to-ptr) - an address in the target, never dereferenced here
        if (m_Api.virtualQueryEx(process.get(), reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER && address != 0)
            {
                break; // past the top of the process's address space: the walk is done
            }
            if (hasExited(m_Api, process.get()))
            {
                return failure(MemoryMapReadStatus::ProcessExited);
            }
            if (error == ERROR_ACCESS_DENIED)
            {
                return failure(MemoryMapReadStatus::PermissionDenied);
            }
            return failure(MemoryMapReadStatus::Failed, std::system_category().message(static_cast<int>(error)));
        }
        if (info.State != MEM_FREE)
        {
            if (result.mappings.size() >= m_MaxRegions)
            {
                result.truncated = true;
                break;
            }
            result.mappings.push_back(toMapping(info, regionMayHaveFile(info.Type) ? files.name(info) : std::string{}));
        }
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address)
        {
            break; // the last region ends at the top of the address space (or a broken answer would loop)
        }
        address = next;
    }
    // A process that exited during the walk leaves an emptied address space, not its map.
    if (hasExited(m_Api, process.get()))
    {
        return failure(MemoryMapReadStatus::ProcessExited);
    }
    return result;
}

} // namespace Platform::Windows
