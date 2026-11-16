#include "WindowsProcessMemoryMap.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "WinString.h"
#include "WindowsDevicePaths.h"
#include "WindowsHandles.h"
#include "WindowsProcessIdentity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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

/// Counts the resident pages of one read's committed regions with QueryWorkingSetEx (#4031), within its
/// limits: at most limits.pagesPerQuery pages a call, no region past limits.maxRegionBytes, and no more than
/// limits.maxPagesPerRead pages over the read. A region it doesn't ask about, or whose query fails, keeps no
/// sizes; a refused query (the process granted only the limited right after all) stops the rest.
class WorkingSetCounter
{
  public:
    WorkingSetCounter(const ProcessMemoryMapFunctions& api, HANDLE process, const WorkingSetLimits& limits, bool allowed)
        : m_Api(&api), m_Process(process), m_Limits(limits), m_Remaining(limits.maxPagesPerRead), m_Allowed(allowed)
    {
        // A call's buffer size is a DWORD of bytes.
        constexpr std::size_t MOST_PER_CALL = std::numeric_limits<DWORD>::max() / sizeof(PSAPI_WORKING_SET_EX_INFORMATION);
        m_Limits.pagesPerQuery = std::clamp<std::size_t>(m_Limits.pagesPerQuery, 1, MOST_PER_CALL);
    }

    /// @p info's region's resident pages, or none when it isn't committed or isn't asked about.
    [[nodiscard]] std::optional<WorkingSetPages> count(const MEMORY_BASIC_INFORMATION& info)
    {
        const auto size = static_cast<std::uint64_t>(info.RegionSize);
        if (!m_Allowed || info.State != MEM_COMMIT || size > m_Limits.maxRegionBytes)
        {
            return std::nullopt;
        }
        const std::uint64_t pages = (size + WORKING_SET_PAGE_BYTES - 1) / WORKING_SET_PAGE_BYTES; // a region is whole pages
        if (pages > m_Remaining)
        {
            return std::nullopt;
        }
        m_Remaining -= pages;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        WorkingSetPages counted;
        for (std::uint64_t done = 0; done < pages;)
        {
            const auto batch = static_cast<std::size_t>(std::min<std::uint64_t>(pages - done, m_Limits.pagesPerQuery));
            m_Buffer.resize(batch);
            for (std::size_t i = 0; i < batch; ++i)
            {
                const std::uintptr_t page = base + static_cast<std::uintptr_t>((done + i) * WORKING_SET_PAGE_BYTES);
                // NOLINTNEXTLINE(performance-no-int-to-ptr) - an address in the target, never dereferenced here
                m_Buffer[i].VirtualAddress = reinterpret_cast<PVOID>(page);
            }
            if (m_Api->queryWorkingSetEx(
                    m_Process, m_Buffer.data(), static_cast<DWORD>(batch * sizeof(PSAPI_WORKING_SET_EX_INFORMATION))) == 0)
            {
                if (GetLastError() == ERROR_ACCESS_DENIED)
                {
                    m_Allowed = false;
                }
                return std::nullopt;
            }
            for (const PSAPI_WORKING_SET_EX_INFORMATION& entry : m_Buffer)
            {
                countPage(counted, entry.VirtualAttributes);
            }
            done += batch;
        }
        return counted;
    }

  private:
    const ProcessMemoryMapFunctions* m_Api; // the reader's, which outlives this read
    HANDLE m_Process;
    WorkingSetLimits m_Limits;
    std::uint64_t m_Remaining;
    bool m_Allowed;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> m_Buffer;
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
    // QueryWorkingSetEx needs the full right, even on recent Windows: on the limited one the sizes stay absent.
    const bool canQueryWorkingSet = opened.has_value();
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
    WorkingSetCounter workingSet(m_Api, process.get(), m_Limits, canQueryWorkingSet);
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
            MemoryMapping& mapping =
                result.mappings.emplace_back(toMapping(info, regionMayHaveFile(info.Type) ? files.name(info) : std::string{}));
            if (const std::optional<WorkingSetPages> pages = workingSet.count(info))
            {
                setWorkingSetSizes(mapping, *pages);
            }
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
