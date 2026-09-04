#include "WindowsProcessProbe.h"

#include "Domain/Numeric.h"

#include <spdlog/spdlog.h>

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; symbols come from implementation
// sub-headers that cannot be included individually without breaking the required include order.
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <sddl.h>
#include <winternl.h>
#include <winver.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#undef max
#undef min

#include "WinString.h"
#include "WindowsProcAddress.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// RAII wrapper for a Win32 HANDLE — guarantees CloseHandle() runs on every exit path
/// (including an exception unwinding the stack between acquisition and an explicit
/// CloseHandle call), not just the normal fall-through path (#774).
class ScopedHandle
{
  public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE hIn) noexcept : m_Handle(hIn)
    {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : m_Handle(other.m_Handle)
    {
        other.m_Handle = nullptr;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Handle = other.m_Handle;
            other.m_Handle = nullptr;
        }
        return *this;
    }
    ~ScopedHandle() noexcept
    {
        reset();
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_Handle;
    }
    // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion lets a ScopedHandle
    // be passed directly to HANDLE-taking Win32 APIs without a get() call at every use site.
    [[nodiscard]] operator HANDLE() const noexcept
    {
        return m_Handle;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_Handle != nullptr;
    }
    /// For APIs that write the handle through an out-param (e.g. OpenProcessToken's PHANDLE):
    /// releases any handle already held, then returns the address to write the new one into.
    [[nodiscard]] HANDLE* put() noexcept
    {
        reset();
        return &m_Handle;
    }

  private:
    void reset() noexcept
    {
        if (m_Handle != nullptr)
        {
            CloseHandle(m_Handle);
            m_Handle = nullptr;
        }
    }

    HANDLE m_Handle = nullptr;
};

/// Convert FILETIME to 100-nanosecond intervals (ticks)
[[nodiscard]] uint64_t filetimeToTicks(const FILETIME& ft)
{
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

/// Convert 100-nanosecond intervals since January 1, 1601 UTC to Unix epoch seconds
/// Unix epoch is seconds since January 1, 1970 UTC
/// The difference is 11644473600 seconds (369 years)
[[nodiscard]] uint64_t ticksToUnixEpoch(uint64_t windowsTicks)
{
    constexpr uint64_t WINDOWS_TICKS_PER_SECOND = 10'000'000;
    constexpr uint64_t WINDOWS_EPOCH_TO_UNIX_EPOCH = 11644473600ULL;

    // Convert 100-nanosecond intervals to seconds and adjust for epoch difference
    const std::uint64_t windowsSeconds = windowsTicks / WINDOWS_TICKS_PER_SECOND;
    if (windowsSeconds < WINDOWS_EPOCH_TO_UNIX_EPOCH)
    {
        return 0; // Invalid time (before Unix epoch)
    }
    return windowsSeconds - WINDOWS_EPOCH_TO_UNIX_EPOCH;
}

/// Map Windows process state to single character
[[nodiscard]] char getProcessState(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return '?';
    }

    // Note: Windows APIs require DWORD for exit codes; usage is localized here.
    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) != 0)
    {
        if (exitCode == STILL_ACTIVE)
        {
            return 'R'; // Running
        }
        return 'Z'; // Zombie/terminated
    }
    return '?';
}

/// Get the username (owner) of a process
[[nodiscard]] std::string getProcessOwner(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    ScopedHandle hToken;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, hToken.put()) == 0)
    {
        return {};
    }

    // Get token user size
    DWORD tokenInfoLen = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &tokenInfoLen);
    if (tokenInfoLen == 0)
    {
        return {};
    }

    // Allocate buffer and get token user. hToken is released automatically (ScopedHandle) on
    // every exit path below, including if this allocation throws std::bad_alloc (#774's leak
    // class, one level deeper than getProcessDetails' own hProcess/hQueryInfo).
    std::vector<BYTE> tokenInfo(tokenInfoLen);
    if (GetTokenInformation(hToken, TokenUser, tokenInfo.data(), tokenInfoLen, &tokenInfoLen) == 0)
    {
        return {};
    }

    // Look up the user name from the SID
    if (tokenInfo.size() < sizeof(TOKEN_USER))
    {
        return {};
    }

    TOKEN_USER tokenUser{};
    std::memcpy(&tokenUser, tokenInfo.data(), sizeof(TOKEN_USER));
    std::array<WCHAR, 256> userName{};
    std::array<WCHAR, 256> domainName{};
    // Fallback to the actual array size constant (256) if conversion fails
    DWORD userNameLen = Domain::Numeric::narrowOr<DWORD>(userName.size(), DWORD{256});
    DWORD domainNameLen = Domain::Numeric::narrowOr<DWORD>(domainName.size(), DWORD{256});
    SID_NAME_USE sidType{SidTypeUnknown}; // LookupAccountSidW will overwrite this; SidTypeUnknown is the nearest valid zero-like sentinel

    if (LookupAccountSidW(nullptr, tokenUser.User.Sid, userName.data(), &userNameLen, domainName.data(), &domainNameLen, &sidType) == 0)
    {
        return {};
    }

    return WinString::wideToUtf8(userName.data());
}

/// Get the full command line (image path) of a process
[[nodiscard]] std::string getProcessCommandLine(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    // Start at MAX_PATH and grow up to the Windows long-path limit (32767 chars).
    // QueryFullProcessImageNameW sets the buffer size to the number of chars written
    // (excluding the null terminator) on success, or leaves it unchanged on failure.
    constexpr DWORD kInitialSize = MAX_PATH;
    constexpr DWORD kMaxLongPath = 32767;

    std::wstring path(kInitialSize, L'\0');
    for (;;)
    {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(hProcess, 0, path.data(), &size) != 0)
        {
            path.resize(size);
            return WinString::wideToUtf8(path);
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= kMaxLongPath)
        {
            return {};
        }
        const DWORD newSize = std::min(static_cast<DWORD>(path.size()) * 2, kMaxLongPath);
        path.assign(newSize, L'\0');
    }
}

using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

[[nodiscard]] NtQueryInformationProcessFn getNtQueryInformationProcessFn() noexcept
{
    static NtQueryInformationProcessFn cachedFn = []() -> NtQueryInformationProcessFn
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return Windows::getProcAddress<NtQueryInformationProcessFn>(ntdll, "NtQueryInformationProcess");
    }();

    return cachedFn;
}

// Some Windows SDK versions omit the full SYSTEM_PROCESS_INFORMATION layout — winternl.h
// declares a truncated variant with reserved fields. Declare the complete documented layout
// (stable ABI since Windows XP) so one bulk NtQuerySystemInformation(SystemProcessInformation)
// snapshot provides CPU times, memory, I/O, handle/thread counts, and names for all processes.
struct SystemProcessInfo
{
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    LARGE_INTEGER workingSetPrivateSize;
    ULONG hardFaultCount;
    ULONG numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    UNICODE_STRING imageName;
    LONG basePriority;
    HANDLE uniqueProcessId;
    HANDLE inheritedFromUniqueProcessId;
    ULONG handleCount;
    ULONG sessionId;
    ULONG_PTR uniqueProcessKey;
    SIZE_T peakVirtualSize;
    SIZE_T virtualSize;
    ULONG pageFaultCount;
    SIZE_T peakWorkingSetSize;
    SIZE_T workingSetSize;
    SIZE_T quotaPeakPagedPoolUsage;
    SIZE_T quotaPagedPoolUsage;
    SIZE_T quotaPeakNonPagedPoolUsage;
    SIZE_T quotaNonPagedPoolUsage;
    SIZE_T pagefileUsage;
    SIZE_T peakPagefileUsage;
    SIZE_T privatePageCount;
    LARGE_INTEGER readOperationCount;
    LARGE_INTEGER writeOperationCount;
    LARGE_INTEGER otherOperationCount;
    LARGE_INTEGER readTransferCount;
    LARGE_INTEGER writeTransferCount;
    LARGE_INTEGER otherTransferCount;
};

// Compile-time ABI validation against the public SDK declaration: winternl.h names a subset of
// the fields (hiding the rest behind Reserved blocks of identical size), so every named field's
// offset and the total size must match our complete layout exactly. Any SDK layout change or a
// mistake in our declaration fails the build instead of silently producing garbage counters.
static_assert(sizeof(SystemProcessInfo) == sizeof(SYSTEM_PROCESS_INFORMATION));
static_assert(offsetof(SystemProcessInfo, nextEntryOffset) == offsetof(SYSTEM_PROCESS_INFORMATION, NextEntryOffset));
static_assert(offsetof(SystemProcessInfo, numberOfThreads) == offsetof(SYSTEM_PROCESS_INFORMATION, NumberOfThreads));
static_assert(offsetof(SystemProcessInfo, imageName) == offsetof(SYSTEM_PROCESS_INFORMATION, ImageName));
static_assert(offsetof(SystemProcessInfo, basePriority) == offsetof(SYSTEM_PROCESS_INFORMATION, BasePriority));
static_assert(offsetof(SystemProcessInfo, uniqueProcessId) == offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId));
static_assert(offsetof(SystemProcessInfo, handleCount) == offsetof(SYSTEM_PROCESS_INFORMATION, HandleCount));
static_assert(offsetof(SystemProcessInfo, sessionId) == offsetof(SYSTEM_PROCESS_INFORMATION, SessionId));
static_assert(offsetof(SystemProcessInfo, peakVirtualSize) == offsetof(SYSTEM_PROCESS_INFORMATION, PeakVirtualSize));
static_assert(offsetof(SystemProcessInfo, virtualSize) == offsetof(SYSTEM_PROCESS_INFORMATION, VirtualSize));
static_assert(offsetof(SystemProcessInfo, peakWorkingSetSize) == offsetof(SYSTEM_PROCESS_INFORMATION, PeakWorkingSetSize));
static_assert(offsetof(SystemProcessInfo, workingSetSize) == offsetof(SYSTEM_PROCESS_INFORMATION, WorkingSetSize));
static_assert(offsetof(SystemProcessInfo, quotaPagedPoolUsage) == offsetof(SYSTEM_PROCESS_INFORMATION, QuotaPagedPoolUsage));
static_assert(offsetof(SystemProcessInfo, quotaNonPagedPoolUsage) == offsetof(SYSTEM_PROCESS_INFORMATION, QuotaNonPagedPoolUsage));
static_assert(offsetof(SystemProcessInfo, pagefileUsage) == offsetof(SYSTEM_PROCESS_INFORMATION, PagefileUsage));
static_assert(offsetof(SystemProcessInfo, peakPagefileUsage) == offsetof(SYSTEM_PROCESS_INFORMATION, PeakPagefileUsage));
static_assert(offsetof(SystemProcessInfo, privatePageCount) == offsetof(SYSTEM_PROCESS_INFORMATION, PrivatePageCount));
// The SDK's trailing Reserved7[6] covers the six I/O LARGE_INTEGER counters we declare.
static_assert(offsetof(SystemProcessInfo, readOperationCount) == offsetof(SYSTEM_PROCESS_INFORMATION, Reserved7));

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

[[nodiscard]] NtQuerySystemInformationFn getNtQuerySystemInformationFn() noexcept
{
    static NtQuerySystemInformationFn cachedFn = []() -> NtQuerySystemInformationFn
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return Windows::getProcAddress<NtQuerySystemInformationFn>(ntdll, "NtQuerySystemInformation");
    }();

    return cachedFn;
}

// Safe and necessary: STATUS_INFO_LENGTH_MISMATCH is the documented NTSTATUS value 0xC0000004;
// NTSTATUS is signed, so the high-bit-set literal requires an explicit cast.
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<NTSTATUS>(0xC0000004L);
constexpr std::size_t SNAPSHOT_INITIAL_BYTES = std::size_t{512} * std::size_t{1024};
constexpr std::size_t SNAPSHOT_HEADROOM_BYTES = std::size_t{64} * std::size_t{1024};

// Structure for ProcessExtendedBasicInformation
// Available since Windows 8/10, contains IsFrozen and IsBackground flags
struct ProcessExtendedBasicInformation
{
    SIZE_T size = 0;
    PROCESS_BASIC_INFORMATION basicInfo{};
    ULONG flags = 0;
};

// Some Windows SDKs cap PROCESSINFOCLASS enum to a smaller range; use a non-constexpr
// conversion to allow the extended value used by ProcessExtendedBasicInformation.
const PROCESSINFOCLASS PROCESS_INFO_EXTENDED_BASIC = static_cast<PROCESSINFOCLASS>(64);

// Bit flags for ProcessExtendedBasicInformation.flags (only keep flags we use)
constexpr ULONG PEBI_IS_FROZEN = 0x00000010;     // Process is suspended (UWP apps, frozen by OS)
constexpr ULONG PEBI_IS_BACKGROUND = 0x00000020; // Background process (efficiency mode)

/// Query process status (Suspended, Efficiency Mode)
[[nodiscard]] std::string getProcessStatus(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    auto* fn = getNtQueryInformationProcessFn();
    if (fn == nullptr)
    {
        return {};
    }

    ProcessExtendedBasicInformation extInfo{};
    extInfo.size = sizeof(extInfo);
    ULONG returnLen = 0;

    static_assert(sizeof(extInfo) <= std::numeric_limits<ULONG>::max(), "ProcessExtendedBasicInformation size exceeds ULONG range");
    const ULONG extInfoSize = Domain::Numeric::narrowOr<ULONG>(sizeof(extInfo), ULONG{0});

    const NTSTATUS status = fn(hProcess, PROCESS_INFO_EXTENDED_BASIC, &extInfo, extInfoSize, &returnLen);
    if (status < 0)
    {
        // API not available or process not accessible
        return {};
    }

    // Check for frozen state (Suspended)
    if ((extInfo.flags & PEBI_IS_FROZEN) != 0)
    {
        return "Suspended";
    }

    // Check for background/efficiency mode
    if ((extInfo.flags & PEBI_IS_BACKGROUND) != 0)
    {
        return "Efficiency Mode";
    }

    // No special status
    return {};
}

/// Query GDI object count for a process via GetGuiResources.
/// Requires a handle opened with at least PROCESS_QUERY_INFORMATION.
/// Returns std::nullopt when hQuery is null (process not accessible with required rights).
/// Returns 0 when the process is accessible but owns no GDI objects.
/// Note: GetGuiResources returns 0 on error as well as on genuine zero; SetLastError(0) before
/// the call lets us distinguish the two cases via GetLastError() afterwards.
[[nodiscard]] std::optional<std::int32_t> getProcessGdiObjectCount(HANDLE hQuery)
{
    if (hQuery == nullptr)
    {
        return std::nullopt;
    }

    SetLastError(0);
    const DWORD count = GetGuiResources(hQuery, GR_GDIOBJECTS);
    if (count == 0 && GetLastError() != 0)
    {
        // GetGuiResources failed (e.g. insufficient access rights on this handle type).
        return std::nullopt;
    }
    return Domain::Numeric::narrowOr<std::int32_t>(count, std::int32_t{0});
}

/// Read the CompanyName from a PE file's version info (publisher/vendor).
/// Enumerates all language/codepage pairs from VarFileInfo\Translation so that
/// localised executables with non-English version resources are handled correctly.
/// Results are cached by executable path; version resources do not change at runtime.
/// Returns empty string if not available or on failure.
[[nodiscard]] std::string getFilePublisher(const std::wstring& imagePath)
{
    if (imagePath.empty())
    {
        return {};
    }

    // Cache publisher by executable path — version info is static for a given binary.
    // Normalise to lowercase before lookup so that different-cased paths for the same
    // binary (which QueryFullProcessImageNameW does not guarantee to be stable) share
    // a single cache entry.
    // Only cache lookups and inserts are serialised; the expensive version-info I/O
    // is performed outside the lock so concurrent threads do not stall each other.
    // Capped and cleared on overflow, matching PDHGPUProbeImpl::instanceCache in this same file
    // family: an unbounded cache here would grow for the life of this long-running, background-
    // resident process on a system with high churn of distinct executable paths (#775).
    static constexpr std::size_t MAX_PUBLISHER_CACHE_ENTRIES = 16384;
    static std::mutex s_cacheMutex;
    static std::unordered_map<std::wstring, std::string> s_cache;
    std::wstring cacheKey(imagePath);
    std::ranges::transform(
        cacheKey, cacheKey.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))); });
    {
        std::scoped_lock const lock(s_cacheMutex);
        if (const auto it = s_cache.find(cacheKey); it != s_cache.end())
        {
            return it->second;
        }
        if (s_cache.size() > MAX_PUBLISHER_CACHE_ENTRIES)
        {
            s_cache.clear();
        }
    }

    // GetFileVersionInfoSizeW requires a LPDWORD that receives an internal handle.
    // The handle is always set to 0 on Windows NT and later, but must be provided.
    DWORD unused = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(imagePath.c_str(), &unused);
    if (infoSize == 0)
    {
        std::scoped_lock const lock(s_cacheMutex);
        s_cache.try_emplace(cacheKey, std::string{});
        return {};
    }

    std::vector<BYTE> buffer(infoSize);
    if (GetFileVersionInfoW(imagePath.c_str(), 0, infoSize, buffer.data()) == 0)
    {
        std::scoped_lock const lock(s_cacheMutex);
        s_cache.try_emplace(cacheKey, std::string{});
        return {};
    }

    // Helper: query CompanyName using a specific translation path (e.g. \StringFileInfo\040904B0\CompanyName).
    // The pointer returned by VerQueryValueW is into buffer, valid while buffer is alive.
    const auto queryCompanyName = [&](const wchar_t* translationPath) -> std::string
    {
        LPVOID companyNamePtr = nullptr;
        UINT companyNameLen = 0;
        if (VerQueryValueW(buffer.data(), translationPath, &companyNamePtr, &companyNameLen) != 0 && companyNameLen > 0 &&
            companyNamePtr != nullptr)
        {
            // StringFileInfo queries return wchar_t strings per the Windows API contract.
            // LPVOID is void* so an explicit cast is required; this is safe here.
            const wchar_t* companyName = static_cast<const wchar_t*>(companyNamePtr);
            return WinString::wideToUtf8(companyName);
        }
        return {};
    };

    std::string result;

    // Enumerate actual translation pairs from VarFileInfo\Translation.
    // Each entry is a DWORD: low WORD = language ID, high WORD = code page.
    LPVOID translationData = nullptr;
    UINT translationSize = 0;
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", &translationData, &translationSize) != 0 &&
        translationData != nullptr && translationSize >= sizeof(DWORD))
    {
        const UINT entryCount = translationSize / sizeof(DWORD);
        // LPVOID is void* — cast to DWORD* to iterate over translation pairs; safe per MSDN.
        const auto* entries = static_cast<const DWORD*>(translationData);
        for (UINT i = 0; i < entryCount && result.empty(); ++i)
        {
            const WORD langId = LOWORD(entries[i]);
            const WORD codePage = HIWORD(entries[i]);
            std::array<WCHAR, 64> pathBuf{};
            swprintf_s(pathBuf.data(), std::size(pathBuf), L"\\StringFileInfo\\%04X%04X\\CompanyName", langId, codePage);
            result = queryCompanyName(pathBuf.data());
        }
    }

    // Fallback: try common English translation IDs for executables without VarFileInfo\Translation.
    if (result.empty())
    {
        result = queryCompanyName(L"\\StringFileInfo\\040904B0\\CompanyName");
    }
    if (result.empty())
    {
        result = queryCompanyName(L"\\StringFileInfo\\040904E4\\CompanyName");
    }

    {
        std::scoped_lock const lock(s_cacheMutex);
        s_cache.try_emplace(cacheKey, result);
    }
    return result;
}

/// Read the publisher of a process from its PE file version information.
/// Uses a growing buffer for QueryFullProcessImageNameW to support long-path executables.
[[nodiscard]] std::string getProcessPublisher(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    // Grow the buffer up to the Windows long-path limit (32767 wide chars) if needed,
    // matching the pattern used in WindowsPathProvider for GetModuleFileNameW.
    constexpr DWORD kInitialSize = MAX_PATH;
    constexpr DWORD kMaxLongPath = 32767;

    std::wstring imagePath(kInitialSize, L'\0');
    for (;;)
    {
        DWORD size = static_cast<DWORD>(imagePath.size());
        if (QueryFullProcessImageNameW(hProcess, 0, imagePath.data(), &size) != 0)
        {
            imagePath.resize(size);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || static_cast<DWORD>(imagePath.size()) >= kMaxLongPath)
        {
            return {};
        }
        const DWORD newSize = std::min(static_cast<DWORD>(imagePath.size()) * 2, kMaxLongPath);
        imagePath.assign(static_cast<std::size_t>(newSize), L'\0');
    }

    return getFilePublisher(imagePath);
}

/// Classify a process as "App", "Background Process", or "Windows Process".
/// - "App": has USER objects (owns an interactive UI window)
/// - "Windows Process": core OS components in the Windows system directories
/// - "Background Process": everything else (services, daemons, headless apps)
///
/// hQuery must be opened with at least PROCESS_QUERY_INFORMATION (required by
/// GetGuiResources); it may be null, in which case the USER-objects check is skipped.
///
/// Limitations: console apps host their window in conhost.exe, so they return
/// zero USER objects and are classified as "Background Process" even if
/// user-facing. Services with message-only windows may be misclassified as "App".
[[nodiscard]] std::string classifyProcessType(HANDLE hQuery, DWORD pid, std::string_view imagePath)
{
    // PID 0 (Idle) and PID 4 (System) are always Windows processes.
    if (pid == 0 || pid == 4)
    {
        return "Windows Process";
    }

    // Check USER objects first: a non-zero count means the process owns an active
    // UI window. Inbox apps like Notepad and Paint live in System32, so this check
    // must come before the directory heuristic to classify them as "App" correctly.
    if (hQuery != nullptr)
    {
        const DWORD userObjects = GetGuiResources(hQuery, GR_USEROBJECTS);
        if (userObjects > 0)
        {
            return "App";
        }
    }

    // Check whether the executable lives in a Windows system directory.
    // Use case-insensitive comparison because QueryFullProcessImageNameW can
    // return paths with arbitrary casing (e.g. C:\WINDOWS\System32\...).
    if (!imagePath.empty())
    {
        std::string lowerPath(imagePath);
        for (auto& c : lowerPath)
        {
            // Cast to unsigned char before tolower to avoid UB on signed char values.
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        // Anchor to the drive root: check that the path starts with <letter>:\windows\
        // so that a user directory that happens to contain the word "windows" (e.g.
        // C:\Users\awindows\System32\) cannot false-match the heuristic.
        const bool isInWindowsDir = lowerPath.size() > 3 && lowerPath[1] == ':' && lowerPath.substr(2).starts_with("\\windows\\");
        const bool isInSystemDir =
            lowerPath.contains("\\system32\\") || lowerPath.contains("\\syswow64\\") || lowerPath.contains("\\systemapps\\");

        if (isInWindowsDir && isInSystemDir)
        {
            return "Windows Process";
        }
    }

    // If we could inspect USER objects (hQuery != null) and the process had none,
    // it is definitely not a GUI app: classify it as Background Process.
    // If we could NOT open the process, we cannot rule out that it owns UI windows,
    // so return an empty string to indicate the classification is unknown.
    return (hQuery != nullptr) ? "Background Process" : "";
}

} // namespace

WindowsProcessProbe::WindowsProcessProbe() : m_HasPowerMonitoring(detectPowerMonitoring())
{
    if (m_HasPowerMonitoring)
    {
        spdlog::info("Power monitoring available on Windows");
    }
    else
    {
        spdlog::debug("Power monitoring not available on Windows");
    }

    m_HasNetworkCounters = detectNetworkCounters();
    if (m_HasNetworkCounters)
    {
        spdlog::info("Per-process network counters available via TCP EStats");
    }
    else
    {
        spdlog::debug("Per-process network counters not available (EStats unsupported or access denied)");
    }

    // Tune detail cache TTLs based on total physical RAM
    calculateDetailTTLsFromTotalRAM(m_LightDetailTTL, m_HeavyDetailTTL);
    spdlog::debug(
        "Detail cache TTLs tuned for total physical RAM: light={}ms, heavy={}ms", m_LightDetailTTL.count(), m_HeavyDetailTTL.count());
}

WindowsProcessProbe::~WindowsProcessProbe()
{
    if (m_IphlpModule != nullptr)
    {
        FreeLibrary(m_IphlpModule);
        m_IphlpModule = nullptr;
    }
}

std::vector<ProcessCounters> WindowsProcessProbe::enumerate()
{
    std::vector<ProcessCounters> results;
    results.reserve(m_LastEnumeratedProcessCount);
    ++m_DetailCacheGeneration;

    auto* queryFn = getNtQuerySystemInformationFn();
    if (queryFn == nullptr)
    {
        spdlog::error("NtQuerySystemInformation unavailable; cannot enumerate processes");
        return results;
    }

    if (m_SnapshotBuffer.empty())
    {
        m_SnapshotBuffer.resize(SNAPSHOT_INITIAL_BYTES);
    }

    // One bulk kernel snapshot provides PIDs, names, CPU times, memory, I/O, and handle/thread
    // counts for every process — replacing per-process OpenProcess + query calls per sample.
    NTSTATUS status = 0;
    ULONG returnedBytes = 0;
    for (;;)
    {
        returnedBytes = 0;
        // Fallback saturates at ULONG max; the kernel simply reports the buffer as too small.
        const ULONG bufferSize = Domain::Numeric::narrowOr<ULONG>(m_SnapshotBuffer.size(), std::numeric_limits<ULONG>::max());
        status = queryFn(SystemProcessInformation, m_SnapshotBuffer.data(), bufferSize, &returnedBytes);
        if (status != STATUS_INFO_LENGTH_MISMATCH_NT)
        {
            break;
        }
        // Snapshot didn't fit; grow with headroom because the process list can change between calls.
        m_SnapshotBuffer.resize(static_cast<std::size_t>(returnedBytes) + SNAPSHOT_HEADROOM_BYTES);
    }

    if (status < 0)
    {
        spdlog::error("NtQuerySystemInformation(SystemProcessInformation) failed: status=0x{:08X}", static_cast<std::uint32_t>(status));
        return results;
    }

    // Never trust nextEntryOffset chains beyond the bytes the kernel actually wrote; clamp to the
    // buffer size in case ReturnLength was not populated.
    const std::size_t snapshotBytes =
        std::min<std::size_t>(m_SnapshotBuffer.size(), returnedBytes != 0 ? returnedBytes : m_SnapshotBuffer.size());

    for (std::size_t offset = 0; (offset + sizeof(SystemProcessInfo)) <= snapshotBytes;)
    {
        // Safe and necessary: parsing the OS-defined SYSTEM_PROCESS_INFORMATION layout out of the
        // kernel-filled byte buffer; entries are chained via nextEntryOffset.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* info = reinterpret_cast<const SystemProcessInfo*>(m_SnapshotBuffer.data() + offset);

        ProcessCounters counters{};
        // Safe and necessary: the kernel stores PIDs as HANDLE-sized integers in this structure;
        // Windows PIDs always fit in 32 bits.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info->uniqueProcessId));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto parentPid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info->inheritedFromUniqueProcessId));

        // Fallback to 0 for PID/parent PID/counts if out of range (should never happen in practice)
        counters.pid = Domain::Numeric::narrowOr<std::int32_t>(pid, std::int32_t{0});
        counters.parentPid = Domain::Numeric::narrowOr<std::int32_t>(parentPid, std::int32_t{0});
        counters.threadCount = Domain::Numeric::narrowOr<std::int32_t>(info->numberOfThreads, std::int32_t{0});
        counters.handleCount = Domain::Numeric::narrowOr<std::int32_t>(info->handleCount, std::int32_t{0});

        // Safe and necessary: kernel time values are non-negative 100-ns tick counts stored in
        // LARGE_INTEGER (signed); converting to uint64_t preserves the value.
        counters.userTime = static_cast<std::uint64_t>(info->userTime.QuadPart);
        counters.systemTime = static_cast<std::uint64_t>(info->kernelTime.QuadPart);
        counters.startTimeTicks = static_cast<std::uint64_t>(info->createTime.QuadPart);
        counters.startTimeEpoch = ticksToUnixEpoch(counters.startTimeTicks);

        counters.rssBytes = info->workingSetSize;
        counters.peakRssBytes = info->peakWorkingSetSize;
        counters.virtualBytes = info->virtualSize;
        counters.pageFaultCount = Domain::Numeric::narrowOr<std::uint64_t>(info->pageFaultCount, std::uint64_t{0});

        // Safe and necessary: cumulative transfer counts are non-negative; LARGE_INTEGER is signed.
        counters.readBytes = static_cast<std::uint64_t>(info->readTransferCount.QuadPart);
        counters.writeBytes = static_cast<std::uint64_t>(info->writeTransferCount.QuadPart);

        std::wstring_view imageName;
        if (info->imageName.Buffer != nullptr && info->imageName.Length != 0)
        {
            // imageName.Buffer is a separate absolute pointer into the same kernel-filled
            // allocation, not implicitly covered by the nextEntryOffset bounds check above.
            // Reject it if it falls outside the bytes the kernel actually wrote, the same
            // threat model that check already defends against (a corrupted/mismatched
            // entry, or a future OS layout change). Compared as integer addresses rather
            // than via pointer relational operators, since imageName.Buffer is not known
            // to actually point into m_SnapshotBuffer until after this check passes.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - pointer-range check against the raw snapshot buffer
            const auto nameBegin = reinterpret_cast<std::uintptr_t>(info->imageName.Buffer);
            const auto nameEnd = nameBegin + info->imageName.Length;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - pointer-range check against the raw snapshot buffer
            const auto snapshotBegin = reinterpret_cast<std::uintptr_t>(m_SnapshotBuffer.data());
            const auto snapshotEnd = snapshotBegin + snapshotBytes;
            if (nameBegin >= snapshotBegin && nameEnd <= snapshotEnd)
            {
                imageName = std::wstring_view(info->imageName.Buffer, info->imageName.Length / sizeof(wchar_t));
            }
            else
            {
                spdlog::debug("WindowsProcessProbe: rejecting out-of-bounds imageName for pid {}", pid);
            }
        }
        if (imageName.empty() && pid == 0)
        {
            counters.name = "[System Process]"; // Parity with the previous Toolhelp name for the Idle pseudo-process
        }

        // Refresh TTL-cached details (owner, command, publisher, ...) - may fail for protected processes
        // Ignore return value - we still want to include the process even if details fail
        (void) getProcessDetails(pid, counters, imageName);

        results.push_back(std::move(counters));

        if (info->nextEntryOffset == 0)
        {
            break;
        }
        // The loop condition re-validates the advanced offset against snapshotBytes, so a
        // corrupt or truncated entry chain terminates instead of reading past the buffer.
        offset += info->nextEntryOffset;
    }

    m_LastEnumeratedProcessCount = std::max<std::size_t>(results.size(), std::size_t{64});

    std::erase_if(m_DetailCache,
                  [generation = m_DetailCacheGeneration](const auto& entry) { return entry.second.generation != generation; });

    // Attribute energy to processes if power monitoring is available
    if (m_HasPowerMonitoring)
    {
        attributeEnergyToProcesses(results);
    }

    // Attach per-process network counters if available (best effort)
    applyNetworkCounters(results);

    spdlog::trace("Enumerated {} processes", results.size());
    return results;
}

bool WindowsProcessProbe::getProcessDetails(uint32_t pid, ProcessCounters& counters, std::wstring_view imageName)
{
    const auto now = std::chrono::steady_clock::now();

    // When startTimeTicks is 0, the snapshot had no create time (Idle/System pseudo-processes).
    // Treat as always-miss: skip caching to prevent {pid, 0} from collapsing multiple processes onto
    // the same cache entry, which could serve stale details after PID recycling.
    const bool canCache = (counters.startTimeTicks != 0);

    DetailCacheEntry dummyCache{};
    const WindowsProcessProbe::DetailCacheKey detailKey{.pid = pid, .startTimeTicks = counters.startTimeTicks};
    auto [cacheIt, inserted] = canCache ? m_DetailCache.try_emplace(detailKey) : std::make_pair(m_DetailCache.end(), true);
    DetailCacheEntry& cache = canCache ? cacheIt->second : dummyCache;
    if (canCache)
    {
        cache.generation = m_DetailCacheGeneration;
    }

    // Image name: cache the UTF-8 conversion — a process image name never changes.
    if (!inserted && !cache.name.empty())
    {
        counters.name = cache.name;
    }
    else if (!imageName.empty())
    {
        counters.name = WinString::wideToUtf8(imageName);
        if (canCache)
        {
            cache.name = counters.name;
        }
    }

    if (!inserted)
    {
        counters.user = cache.user;
        counters.command = cache.command;
        counters.status = cache.status;
        counters.publisher = cache.publisher;
        counters.processType = cache.processType;
        counters.gdiObjectCount = cache.gdiObjectCount;
        // Restore slow-changing fields that are TTL-cached.
        // Sentinel value '\0' indicates the cache entry was inserted but not yet populated;
        // in that case the field remains at its default.
        counters.cpuAffinityMask = cache.cpuAffinityMask;
        if (cache.state != '\0')
        {
            counters.state = cache.state;
        }
        counters.nice = cache.nice;
    }

    const bool refreshLightDetails = inserted || (now >= cache.nextLightRefresh);
    const bool refreshHeavyDetails = inserted || (now >= cache.nextHeavyRefresh);

    if (!refreshLightDetails && !refreshHeavyDetails)
    {
        // All remaining fields are TTL-cached; no process handle is needed this sample.
        if (counters.command.empty())
        {
            counters.command = "[" + counters.name + "]";
        }
        return true;
    }

    // A handle is only needed to refresh TTL-cached details. Non-cacheable entries
    // (startTimeTicks == 0) are kernel pseudo-processes like Idle that OpenProcess can never
    // access, and TTL backoff cannot be remembered for them — skip the attempt entirely.
    const ScopedHandle hProcess(canCache ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
    if (!hProcess.valid())
    {
        // Can't access this process (protected/system). Push the TTLs forward so we don't retry
        // OpenProcess every sample; the bulk snapshot still provides fresh counters regardless.
        if (canCache)
        {
            cache.nextLightRefresh = now + m_LightDetailTTL;
            cache.nextHeavyRefresh = now + m_HeavyDetailTTL;
        }
        if (counters.command.empty())
        {
            counters.command = "[" + counters.name + "]";
        }
        return false;
    }

    if (refreshHeavyDetails)
    {
        // Expensive data: owner + image path + publisher are refreshed at a lower cadence.
        counters.user = getProcessOwner(hProcess);
        counters.command = getProcessCommandLine(hProcess);
        if (counters.command.empty())
        {
            counters.command = "[" + counters.name + "]";
        }

        // getFilePublisher() has its own path cache; this outer TTL avoids repeated path lookups.
        counters.publisher = getProcessPublisher(hProcess);

        // CPU affinity rarely changes — refresh alongside heavy details.
        DWORD_PTR processAffinityMask = 0;
        DWORD_PTR systemAffinityMask = 0;
        if (GetProcessAffinityMask(hProcess, &processAffinityMask, &systemAffinityMask) != 0)
        {
            // Safe: DWORD_PTR is pointer-sized (64-bit on x64); uint64_t can hold all values.
            counters.cpuAffinityMask = static_cast<std::uint64_t>(processAffinityMask);
        }
        else
        {
            counters.cpuAffinityMask = 0;
        }

        // Priority class rarely changes — refresh alongside heavy details.
        const DWORD priorityClass = GetPriorityClass(hProcess);
        switch (priorityClass)
        {
        case IDLE_PRIORITY_CLASS:
            counters.nice = 19;
            break;
        case BELOW_NORMAL_PRIORITY_CLASS:
            counters.nice = 10;
            break;
        case NORMAL_PRIORITY_CLASS:
            counters.nice = 0;
            break;
        case ABOVE_NORMAL_PRIORITY_CLASS:
            counters.nice = -5;
            break;
        case HIGH_PRIORITY_CLASS:
            counters.nice = -10;
            break;
        case REALTIME_PRIORITY_CLASS:
            counters.nice = -20;
            break;
        default:
            counters.nice = 0;
            break;
        }
    }
    else if (counters.command.empty())
    {
        counters.command = "[" + counters.name + "]";
    }

    if (refreshLightDetails || refreshHeavyDetails)
    {
        // Medium-cost data refreshed more frequently than heavy details.
        counters.status = getProcessStatus(hProcess);

        // Process state (R/Z/?) changes infrequently; refresh at light TTL cadence.
        counters.state = getProcessState(hProcess);
    }

    if (refreshHeavyDetails)
    {
        // Expensive classification metadata changes rarely; refresh only on heavy cadence.
        // Open a PROCESS_QUERY_INFORMATION handle — required by GetGuiResources — and share
        // it with classifyProcessType to avoid two consecutive OpenProcess calls for the same PID.
        const ScopedHandle hQueryInfo(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid)));
        counters.gdiObjectCount = getProcessGdiObjectCount(hQueryInfo);

        // Classify process type (App / Background Process / Windows Process)
        counters.processType = classifyProcessType(hQueryInfo, static_cast<DWORD>(pid), counters.command);
    }

    if (refreshHeavyDetails)
    {
        cache.user = counters.user;
        cache.command = counters.command;
        cache.publisher = counters.publisher;
        cache.processType = counters.processType;
        cache.gdiObjectCount = counters.gdiObjectCount;
        cache.cpuAffinityMask = counters.cpuAffinityMask;
        cache.nice = counters.nice;
        if (canCache)
        {
            cache.nextHeavyRefresh = now + m_HeavyDetailTTL;
        }
    }

    if (refreshLightDetails || refreshHeavyDetails)
    {
        cache.status = counters.status;
        cache.state = counters.state;
        if (canCache)
        {
            cache.nextLightRefresh = now + m_LightDetailTTL;
        }
    }

    return true;
}

ProcessCapabilities WindowsProcessProbe::capabilities() const
{
    // Reduced privileges: EStats-based network counters require Administrator.
    // Use GetTokenInformation(TokenElevation) to check if the current process token is elevated.
    // This is safe to call repeatedly; the elevation state is constant for the lifetime of the process.
    // NOLINTNEXTLINE(misc-include-cleaner) - TOKEN_ELEVATION is defined in windows.h via winnt.h
    bool reducedPrivileges = true; // Conservative default: assume non-elevated

    // ScopedHandle ensures CloseHandle is called on all paths.
    ScopedHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) != FALSE)
    {
        TOKEN_ELEVATION elevation{};
        DWORD dwSize = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &dwSize) != FALSE)
        {
            reducedPrivileges = (elevation.TokenIsElevated == 0);
        }
        else
        {
            spdlog::debug("WindowsProcessProbe: GetTokenInformation failed (error code: {})", GetLastError());
        }
    }
    else
    {
        spdlog::debug("WindowsProcessProbe: OpenProcessToken failed (error code: {})", GetLastError());
    }

    return ProcessCapabilities{
        .hasIoCounters = true,
        .hasThreadCount = true,
        .hasHandleCount = true, // From the SystemProcessInformation snapshot
        .hasUserSystemTime = true,
        .hasStartTime = true,
        .hasUser = true,        // From OpenProcessToken + LookupAccountSid
        .hasCommand = true,     // From QueryFullProcessImageName
        .hasNice = true,        // From GetPriorityClass
        .hasPageFaults = true,  // From the SystemProcessInformation snapshot
        .hasPeakRss = true,     // From the SystemProcessInformation snapshot (PeakWorkingSetSize)
        .hasCpuAffinity = true, // From GetProcessAffinityMask
        // Network counters: Requires ETW (Event Tracing for Windows) or GetPerTcpConnectionEStats
        // See GitHub issue for implementation tracking
        .hasNetworkCounters = m_HasNetworkCounters,
        .hasPowerUsage = m_HasPowerMonitoring, // Available if energy monitoring detected
        .hasStatus = true,                     // From NtQueryInformationProcess ProcessExtendedBasicInformation
        .hasPublisher = true,                  // From GetFileVersionInfo on process image path
        .hasProcessType = true,                // Classified from path + GetGuiResources
        .hasGdiObjects = true,                 // From GetGuiResources(GR_GDIOBJECTS)
        .hasReducedPrivileges =
            reducedPrivileges &&
            m_NetworkCountersAccessDenied, // Non-admin + EStats access-denied: network data unavailable due to privilege
    };
}

uint64_t WindowsProcessProbe::totalCpuTime() const
{
    return readTotalCpuTime();
}

uint64_t WindowsProcessProbe::readTotalCpuTime()
{
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser) == 0)
    {
        spdlog::error("GetSystemTimes failed: {}", GetLastError());
        return 0;
    }

    // Total = kernel + user (kernel includes idle time)
    return filetimeToTicks(ftKernel) + filetimeToTicks(ftUser);
}

long WindowsProcessProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    // 10,000,000 ticks per second
    return 10'000'000L;
}

uint64_t WindowsProcessProbe::systemTotalMemory() const
{
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus) != 0)
    {
        return memStatus.ullTotalPhys;
    }

    spdlog::error("GlobalMemoryStatusEx failed: {}", GetLastError());
    return 0;
}

bool WindowsProcessProbe::detectPowerMonitoring()
{
    // On Windows, we use a simplified approach: check if we can read battery status
    // This provides a basic system-wide energy estimate via battery discharge rate
    // More sophisticated approaches would use PDH (Performance Data Helper) or EMI (Energy Metering Interface)

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus) == 0)
    {
        return false;
    }

    // Power monitoring available if we have battery info or AC power with metrics
    // ACLineStatus: 0 = offline (battery), 1 = online (AC), 255 = unknown
    return powerStatus.ACLineStatus != 255;
}

void WindowsProcessProbe::calculateDetailTTLsFromTotalRAM(std::chrono::milliseconds& lightTTL, std::chrono::milliseconds& heavyTTL) noexcept
{
    // Query total physical RAM and tune cache refresh intervals based on system size.
    // Heuristic: Systems with abundant RAM can afford frequent detail refreshes (lower latency),
    // while memory-constrained systems should cache longer to reduce enumeration overhead.

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);

    if (GlobalMemoryStatusEx(&memStatus) == 0)
    {
        // Query failed; use defaults
        lightTTL = std::chrono::milliseconds(1000);
        heavyTTL = std::chrono::milliseconds(5000);
        return;
    }

    const uint64_t totalBytes = memStatus.ullTotalPhys;

    // Tier thresholds and corresponding TTLs (based on total physical RAM):
    // - < 2 GB total: Aggressive caching (preserve CPU/memory on low-end systems)
    // - 2-4 GB: Conservative refresh
    // - 4-8 GB: Balanced refresh
    // - 8-16 GB: Slightly more frequent, but still avoids per-frame thrash
    // - >= 16 GB: Keep responsive while preventing expensive detail recompute every sample

    if (totalBytes < (2ULL * 1024 * 1024 * 1024))
    {
        // < 2 GB: Cache longer to reduce CPU load on memory-constrained systems
        lightTTL = std::chrono::milliseconds(4000);
        heavyTTL = std::chrono::milliseconds(15000);
    }
    else if (totalBytes < (4ULL * 1024 * 1024 * 1024))
    {
        // 2-4 GB: Conservative but not aggressive
        lightTTL = std::chrono::milliseconds(3000);
        heavyTTL = std::chrono::milliseconds(10000);
    }
    else if (totalBytes < (8ULL * 1024 * 1024 * 1024))
    {
        // 4-8 GB: Balanced refresh
        lightTTL = std::chrono::milliseconds(2000);
        heavyTTL = std::chrono::milliseconds(8000);
    }
    else if (totalBytes < (16ULL * 1024 * 1024 * 1024))
    {
        // 8-16 GB: Moderate refresh
        lightTTL = std::chrono::milliseconds(1500);
        heavyTTL = std::chrono::milliseconds(6000);
    }
    else
    {
        // >= 16 GB: Responsive, but avoid 1Hz+ heavy metadata refresh churn
        lightTTL = std::chrono::milliseconds(1000);
        heavyTTL = std::chrono::milliseconds(4000);
    }
}

uint64_t WindowsProcessProbe::readSystemEnergy() const
{
    // Windows doesn't provide direct energy counters like Linux RAPL
    // This is a simplified implementation using battery discharge estimation
    // For production, consider using:
    // - PDH (Performance Data Helper) counters for power
    // - EMI (Energy Metering Interface) if available
    // - WMI queries for battery metrics

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus) == 0)
    {
        return 0;
    }

    // Estimate energy based on battery percentage and system state
    // This is a rough approximation - actual implementation would need more sophisticated tracking
    // Battery life percent: 0-100, 255 = unknown
    if (powerStatus.BatteryLifePercent > 100)
    {
        return 0;
    }

    // Use a synthetic energy value based on battery state
    // In a real implementation, this would integrate battery discharge rate over time
    // For now, return a cumulative-like value that changes with battery state

    // Increment synthetic energy counter (this simulates cumulative energy consumption)
    // In production, this would read actual hardware counters or integrate power over time
    // fetch_add returns the value *before* the increment; add the increment to get the new total.
    const uint64_t newEnergy = m_SyntheticEnergy.fetch_add(1000000, std::memory_order_relaxed) + 1000000;

    return newEnergy;
}

void WindowsProcessProbe::attributeEnergyToProcesses(std::vector<ProcessCounters>& processes) const
{
    // Read current system-wide energy
    const uint64_t systemEnergy = readSystemEnergy();
    if (systemEnergy == 0)
    {
        return;
    }

    // Calculate total CPU time across all processes
    uint64_t totalProcessCpuTime = 0;
    for (const auto& proc : processes)
    {
        totalProcessCpuTime += (proc.userTime + proc.systemTime);
    }

    // Avoid division by zero
    if (totalProcessCpuTime == 0)
    {
        return;
    }

    // Attribute energy proportionally based on CPU usage
    // This is an approximation: energy per process = systemEnergy * (processCpuTime / totalCpuTime)
    for (auto& proc : processes)
    {
        const uint64_t processCpuTime = proc.userTime + proc.systemTime;
        const double cpuProportion = static_cast<double>(processCpuTime) / static_cast<double>(totalProcessCpuTime);
        proc.energyMicrojoules = static_cast<uint64_t>(static_cast<double>(systemEnergy) * cpuProportion);
    }
}

bool WindowsProcessProbe::detectNetworkCounters()
{
    // Defensive guard: today this is only ever called once (from the constructor via
    // m_HasNetworkCounters), so m_IphlpModule is always null here, but a future retry path
    // calling this twice would otherwise overwrite the handle without a matching FreeLibrary,
    // leaking one DLL reference count (same latent class as #781).
    if (m_IphlpModule != nullptr)
    {
        return m_GetPerTcpConnectionEStats != nullptr && m_SetPerTcpConnectionEStats != nullptr;
    }

    HMODULE iphlp = GetModuleHandleW(L"iphlpapi.dll");
    if (iphlp == nullptr)
    {
        iphlp = LoadLibraryW(L"iphlpapi.dll");
        if (iphlp == nullptr)
        {
            return false;
        }
        // iphlpapi.dll was not already loaded, so we own this handle and must free it in the destructor
        m_IphlpModule = iphlp;
    }

    m_GetPerTcpConnectionEStats = Windows::getProcAddress<GetPerTcpConnectionEStatsFn>(iphlp, "GetPerTcpConnectionEStats");
    m_SetPerTcpConnectionEStats = Windows::getProcAddress<SetPerTcpConnectionEStatsFn>(iphlp, "SetPerTcpConnectionEStats");

    if (m_GetPerTcpConnectionEStats == nullptr || m_SetPerTcpConnectionEStats == nullptr)
    {
        return false;
    }

    // TCP EStats requires elevated privileges to enable data collection.
    // Test with a dummy row to detect if we have sufficient privileges.
    MIB_TCPROW dummy{};
    TCP_ESTATS_DATA_RW_v0 rw{};
    rw.EnableCollection = TRUE;
    const DWORD status = m_SetPerTcpConnectionEStats(&dummy, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);

    // Access denied or not supported means we can't use EStats
    // ERROR_NOT_FOUND is expected for the dummy row and is OK
    if (status == ERROR_ACCESS_DENIED)
    {
        spdlog::debug("Per-process network counters not available (EStats requires administrator privileges)");
        m_NetworkCountersAccessDenied = true;
        return false;
    }
    if (status == ERROR_NOT_SUPPORTED)
    {
        spdlog::debug("Per-process network counters not available (EStats not supported on this system)");
        return false;
    }

    return true;
}

std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> WindowsProcessProbe::collectNetworkByteCounts() const
{
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> perPid;

    if (!m_HasNetworkCounters)
    {
        return perPid;
    }

    collectTcp4ByteCounts(perPid);

    return perPid;
}

void WindowsProcessProbe::collectTcp4ByteCounts(std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& perPid) const
{
    if (m_GetPerTcpConnectionEStats == nullptr)
    {
        return;
    }

    DWORD tableSize = 0;
    DWORD status = GetExtendedTcpTable(nullptr, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != ERROR_INSUFFICIENT_BUFFER || tableSize == 0)
    {
        return;
    }

    std::vector<unsigned char> buffer(tableSize);
    status = GetExtendedTcpTable(buffer.data(), &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != NO_ERROR)
    {
        return;
    }

    const auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());

    std::size_t enabledCount = 0;
    std::size_t readSuccessCount = 0;
    std::size_t hasDataCount = 0;
    std::size_t garbageCount = 0;
    std::size_t skippedStateCount = 0;

    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_TCPROW_OWNER_PID& ownerRow = table->table[i];

        // Only process ESTABLISHED connections (state 5) - these are actively transferring data
        // Skip LISTEN, TIME_WAIT, CLOSE_WAIT, etc. as they don't have meaningful byte counters
        constexpr DWORD MIB_TCP_STATE_ESTAB = 5;
        if (ownerRow.dwState != MIB_TCP_STATE_ESTAB)
        {
            ++skippedStateCount;
            continue;
        }

        MIB_TCPROW ownerRowBase{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - Windows API requires union access
        ownerRowBase.dwState = ownerRow.dwState;
        ownerRowBase.dwLocalAddr = ownerRow.dwLocalAddr;
        ownerRowBase.dwLocalPort = ownerRow.dwLocalPort;
        ownerRowBase.dwRemoteAddr = ownerRow.dwRemoteAddr;
        ownerRowBase.dwRemotePort = ownerRow.dwRemotePort;

        // Try to enable EStats collection (requires admin, may fail)
        if (m_SetPerTcpConnectionEStats != nullptr)
        {
            TCP_ESTATS_DATA_RW_v0 rw{};
            rw.EnableCollection = TRUE;
            const DWORD enableStatus =
                m_SetPerTcpConnectionEStats(&ownerRowBase, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
            if (enableStatus == NO_ERROR)
            {
                ++enabledCount;
            }
        }

        // Read the stats (may work even if enable failed, if another process enabled it)
        TCP_ESTATS_DATA_ROD_v0 rod{};
        const DWORD estats = m_GetPerTcpConnectionEStats(
            &ownerRowBase, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));

        if (estats != NO_ERROR)
        {
            continue;
        }

        ++readSuccessCount;

        // Sanity check: reject garbage values (> 1 TB is clearly wrong for a single connection)
        constexpr std::uint64_t MAX_SANE_BYTES = 1'000'000'000'000ULL; // 1 TB
        if (rod.DataBytesOut > MAX_SANE_BYTES || rod.DataBytesIn > MAX_SANE_BYTES)
        {
            ++garbageCount;
            continue; // Skip this connection - data is garbage/uninitialized
        }

        if (rod.DataBytesOut > 0 || rod.DataBytesIn > 0)
        {
            ++hasDataCount;
        }

        auto& agg = perPid[ownerRow.dwOwningPid];
        agg.first += rod.DataBytesOut;
        agg.second += rod.DataBytesIn;
    }

    // Log diagnostics periodically (once per ~60 samples)
    static std::size_t sampleCount = 0;
    if (++sampleCount % 60 == 1)
    {
        spdlog::debug("TCP EStats: {} total, {} established, {} enabled, {} read OK, {} have data, {} garbage",
                      table->dwNumEntries,
                      table->dwNumEntries - skippedStateCount,
                      enabledCount,
                      readSuccessCount,
                      hasDataCount,
                      garbageCount);
    }
}

void WindowsProcessProbe::applyNetworkCounters(std::vector<ProcessCounters>& processes) const
{
    if (!m_HasNetworkCounters)
    {
        return;
    }

    const auto perPid = collectNetworkByteCounts();
    if (perPid.empty())
    {
        return;
    }

    for (auto& proc : processes)
    {
        auto it = perPid.find(static_cast<uint32_t>(proc.pid));
        if (it != perPid.end())
        {
            proc.netSentBytes = it->second.first;
            proc.netReceivedBytes = it->second.second;
        }
    }
}

// NOLINTEND(misc-include-cleaner)
} // namespace Platform
