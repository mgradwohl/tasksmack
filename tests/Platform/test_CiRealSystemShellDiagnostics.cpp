// A diagnostic, not a test (#3979): why Open file location's unelevated shell open fails on the windows-11-arm
// runner, where IShellWindows::FindWindowSW(SWC_DESKTOP) answers S_FALSE though GetShellWindow() is set, and passes
// on windows-2025 (x64). It logs what decides between the hypotheses and never fails:
//   (a) COM reached another Explorer (a fresh "explorer.exe -Embedding", say) than the one that owns the desktop,
//       for example because UAC is on and this elevated process's activation goes to another server;
//   (b) the shell's Explorer never registers its desktop with ShellWindows, or does so late;
//   (c) something specific to ARM64, such as an emulated process.
// Every line it writes starts with "[3979]". It runs only on a disposable GitHub-hosted runner (CiRealSystem.h's
// guard), and changes nothing there but the COM objects it asks for.

#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Support/CiRealSystem.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <exdisp.h>
#include <tlhelp32.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-union-access,performance-no-int-to-ptr,bugprone-invalid-enum-default-initialization) - a Win32/COM diagnostic
namespace
{

using namespace std::chrono_literals;
using Platform::ComPtr;
using Platform::Windows::UniqueHandle;

class CiRealSystemShellDiagnosticsTest : public TestSupport::CiRealSystemTest
{};

[[nodiscard]] std::string narrow(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

void log(const std::string& line)
{
    std::cout << "[3979] " << line << '\n' << std::flush;
}

[[nodiscard]] std::string hex(HRESULT hr)
{
    return std::format("0x{:08X}", static_cast<unsigned long>(hr));
}

[[nodiscard]] std::string machineName(USHORT machine)
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
    case IMAGE_FILE_MACHINE_ARM64:
        return "ARM64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_ARMNT:
        return "ARM32";
    case IMAGE_FILE_MACHINE_UNKNOWN:
        return "unknown";
    default:
        return std::format("0x{:04X}", machine);
    }
}

/// A DWORD from HKLM, or a note why there is none.
[[nodiscard]] std::string hklmDword(const wchar_t* key, const wchar_t* value)
{
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    return (status == ERROR_SUCCESS) ? std::to_string(data) : std::format("(not read: {})", status);
}

/// A string from @p hive, or a note why there is none.
[[nodiscard]] std::string registryString(HKEY hive, const wchar_t* key, const wchar_t* value)
{
    std::array<wchar_t, 1024> data{};
    auto size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(hive, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, data.data(), &size);
    return (status == ERROR_SUCCESS) ? ("\"" + narrow(data.data()) + "\"") : std::format("(not read: {})", status);
}

[[nodiscard]] std::string hkcrString(const wchar_t* key, const wchar_t* value)
{
    return registryString(HKEY_CLASSES_ROOT, key, value);
}

[[nodiscard]] const char* elevationTypeName(TOKEN_ELEVATION_TYPE type)
{
    switch (type)
    {
    case TokenElevationTypeDefault:
        return "Default";
    case TokenElevationTypeFull:
        return "Full";
    case TokenElevationTypeLimited:
        return "Limited";
    default:
        return "?";
    }
}

/// What one token says: user, elevation type, elevated, integrity RID, logon session (AuthenticationId), linked token.
[[nodiscard]] std::string describeToken(HANDLE token)
{
    std::string out;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    if (size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE)
    {
        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(user.data());
        std::array<wchar_t, 256> name{};
        std::array<wchar_t, 256> domain{};
        auto nameSize = static_cast<DWORD>(name.size());
        auto domainSize = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountSidW(nullptr, tokenUser->User.Sid, name.data(), &nameSize, domain.data(), &domainSize, &use) != FALSE)
        {
            out += std::format("user={}\\{}", narrow(domain.data()), narrow(name.data()));
        }
        else
        {
            out += std::format("user=(lookup failed {})", GetLastError());
        }
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    if (GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) != FALSE)
    {
        const char* typeName = elevationTypeName(type);
        out += std::format(" elevationType={}", typeName);
    }
    TOKEN_ELEVATION elevation{};
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE)
    {
        out += std::format(" elevated={}", elevation.TokenIsElevated != 0);
    }
    size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<std::byte> label(size);
    if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size) != FALSE)
    {
        const auto* mandatory = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data());
        const DWORD count = *GetSidSubAuthorityCount(mandatory->Label.Sid);
        const DWORD rid = (count > 0) ? *GetSidSubAuthority(mandatory->Label.Sid, count - 1) : 0;
        out += std::format(" integrity=0x{:04X}", rid);
    }
    TOKEN_STATISTICS statistics{};
    if (GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &size) != FALSE)
    {
        out += std::format(" logonLuid={:08X}:{:08X}",
                           static_cast<unsigned long>(statistics.AuthenticationId.HighPart),
                           statistics.AuthenticationId.LowPart);
    }
    DWORD sessionId = 0;
    if (GetTokenInformation(token, TokenSessionId, &sessionId, sizeof(sessionId), &size) != FALSE)
    {
        out += std::format(" tokenSession={}", sessionId);
    }
    TOKEN_LINKED_TOKEN linked{};
    if (GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &size) != FALSE)
    {
        out += " linkedToken=yes";
        CloseHandle(linked.LinkedToken);
    }
    else
    {
        out += std::format(" linkedToken=no({})", GetLastError());
    }
    return out;
}

/// The process's command line (NtQueryInformationProcess(ProcessCommandLineInformation)), or a note.
[[nodiscard]] std::string commandLineOf(HANDLE process)
{
    using NtQuery = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    // NOLINTNEXTLINE(clang-diagnostic-cast-function-type-strict) - GetProcAddress's FARPROC to the real signature
    const auto query = reinterpret_cast<NtQuery>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (query == nullptr)
    {
        return "(no NtQueryInformationProcess)";
    }
    constexpr ULONG PROCESS_COMMAND_LINE_INFORMATION = 60;
    std::vector<std::byte> buffer(8192);
    ULONG returned = 0;
    const LONG status = query(process, PROCESS_COMMAND_LINE_INFORMATION, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
    if (status < 0)
    {
        return std::format("(not read: 0x{:08X})", static_cast<unsigned long>(status));
    }
    struct CountedString
    {
        USHORT length;
        USHORT maximumLength;
        PWSTR text;
    };
    CountedString counted{};
    std::memcpy(&counted, buffer.data(), sizeof(counted));
    if (counted.text == nullptr)
    {
        return "(empty)";
    }
    return "\"" + narrow(std::wstring_view(counted.text, counted.length / sizeof(wchar_t))) + "\"";
}

[[nodiscard]] std::string startTimeOf(HANDLE process)
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    SYSTEMTIME utc{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE || FileTimeToSystemTime(&created, &utc) == FALSE)
    {
        return "?";
    }
    return std::format("{:02}:{:02}:{:02}.{:03}Z", utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
}

/// Everything about process @p pid this diagnostic compares: image, architecture, session, token, start time,
/// command line.
[[nodiscard]] std::string describeProcess(DWORD pid)
{
    std::string out = std::format("pid={}", pid);
    DWORD session = 0;
    if (ProcessIdToSessionId(pid, &session) != FALSE)
    {
        out += std::format(" session={}", session);
    }
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
    {
        return out + std::format(" (OpenProcess failed {})", GetLastError());
    }
    std::array<wchar_t, 1024> image{};
    auto imageSize = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(process.get(), 0, image.data(), &imageSize) != FALSE)
    {
        out += " image=" + narrow(std::wstring_view(image.data(), imageSize));
    }
    // ProcessMachineTypeInfo (9): the architecture the process runs as, emulated or not (IsWow64Process2 can't tell an
    // x64 process emulated on ARM64 from a native one).
    struct MachineInformation
    {
        USHORT processMachine;
        USHORT reserved;
        DWORD attributes;
    };
    MachineInformation machine{};
    if (GetProcessInformation(process.get(), static_cast<PROCESS_INFORMATION_CLASS>(9), &machine, sizeof(machine)) != FALSE)
    {
        out += std::format(" arch={} machineAttributes=0x{:X}", machineName(machine.processMachine), machine.attributes);
    }
    else
    {
        out += std::format(" arch=(not read: {})", GetLastError());
    }
    out += " started=" + startTimeOf(process.get());
    HANDLE token = nullptr;
    if (OpenProcessToken(process.get(), TOKEN_QUERY, &token) != FALSE)
    {
        const UniqueHandle owned(token);
        out += " " + describeToken(owned.get());
    }
    else
    {
        out += std::format(" (OpenProcessToken failed {})", GetLastError());
    }
    out += " cmd=" + commandLineOf(process.get());
    return out;
}

struct ProcessEntry
{
    DWORD pid;
    DWORD parent;
};

[[nodiscard]] std::vector<ProcessEntry> explorerProcesses()
{
    std::vector<ProcessEntry> found;
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
    {
        return found;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE; more = Process32NextW(snapshot.get(), &entry))
    {
        if (CompareStringOrdinal(entry.szExeFile, -1, L"explorer.exe", -1, TRUE) == CSTR_EQUAL)
        {
            found.push_back({.pid = entry.th32ProcessID, .parent = entry.th32ParentProcessID});
        }
    }
    return found;
}

void logExplorers(std::string_view when)
{
    const std::vector<ProcessEntry> explorers = explorerProcesses();
    log(std::format("explorer.exe processes {}: {}", when, explorers.size()));
    for (const ProcessEntry& entry : explorers)
    {
        log(std::format("  explorer parent={} {}", entry.parent, describeProcess(entry.pid)));
    }
}

[[nodiscard]] DWORD ownerOf(HWND window)
{
    DWORD pid = 0;
    if (window != nullptr)
    {
        GetWindowThreadProcessId(window, &pid);
    }
    return pid;
}

void logWindow(std::string_view what, HWND window)
{
    log(std::format("{}: hwnd=0x{:X} {}",
                    what,
                    reinterpret_cast<std::uintptr_t>(window),
                    (window != nullptr) ? describeProcess(ownerOf(window)) : std::string("(none)")));
}

[[nodiscard]] std::string classOf(HWND window)
{
    std::array<wchar_t, 128> name{};
    const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    return narrow(std::wstring_view(name.data(), static_cast<std::size_t>(std::max(length, 0))));
}

/// The desktop's folder view: which top-level window (Progman or a WorkerW) holds SHELLDLL_DefView and its
/// SysListView32, and whether they are visible. No DefView means Explorer never made the desktop's view.
void logDesktopView()
{
    struct Found
    {
        int topLevels = 0;
        std::vector<std::string> lines;
    };
    Found found;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL
        {
            auto* into = reinterpret_cast<Found*>(param);
            const std::string name = classOf(window);
            if (name != "Progman" && name != "WorkerW")
            {
                return TRUE;
            }
            ++into->topLevels;
            HWND view = FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr);
            HWND list = (view != nullptr) ? FindWindowExW(view, nullptr, L"SysListView32", nullptr) : nullptr;
            RECT rect{};
            GetWindowRect(window, &rect);
            into->lines.push_back(
                std::format("  {} hwnd=0x{:X} owner={} visible={} rect=({},{})-({},{}) DefView=0x{:X} visible={} ListView=0x{:X}",
                            name,
                            reinterpret_cast<std::uintptr_t>(window),
                            ownerOf(window),
                            IsWindowVisible(window) != FALSE,
                            rect.left,
                            rect.top,
                            rect.right,
                            rect.bottom,
                            reinterpret_cast<std::uintptr_t>(view),
                            (view != nullptr) && IsWindowVisible(view) != FALSE,
                            reinterpret_cast<std::uintptr_t>(list)));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    log(std::format("desktop windows (Progman/WorkerW): {}", found.topLevels));
    for (const std::string& line : found.lines)
    {
        log(line);
    }
}

/// Whether named event @p name exists and is signalled (the shell's ready events, set once the desktop is up).
[[nodiscard]] std::string eventState(const wchar_t* name)
{
    const UniqueHandle event(OpenEventW(SYNCHRONIZE, FALSE, name));
    if (!event)
    {
        return std::format("(not opened: {})", GetLastError());
    }
    return (WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0) ? "signalled" : "not signalled";
}

/// The OS build, the display, the desktop-hiding policies and the shell's ready events.
void logDesktopContext()
{
    const wchar_t* version = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    log(std::format("OS: ProductName={} EditionID={} InstallationType={} DisplayVersion={} CurrentBuild={} UBR={}",
                    registryString(HKEY_LOCAL_MACHINE, version, L"ProductName"),
                    registryString(HKEY_LOCAL_MACHINE, version, L"EditionID"),
                    registryString(HKEY_LOCAL_MACHINE, version, L"InstallationType"),
                    registryString(HKEY_LOCAL_MACHINE, version, L"DisplayVersion"),
                    registryString(HKEY_LOCAL_MACHINE, version, L"CurrentBuild"),
                    hklmDword(version, L"UBR")));
    log(std::format("display: monitors={} screen={}x{} virtual={}x{} remoteSession={}",
                    GetSystemMetrics(SM_CMONITORS),
                    GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN),
                    GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    GetSystemMetrics(SM_CYVIRTUALSCREEN),
                    GetSystemMetrics(SM_REMOTESESSION)));
    const wchar_t* explorerPolicies = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    const wchar_t* advanced = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    const auto hkcuDword = [](const wchar_t* key, const wchar_t* value)
    {
        DWORD data = 0;
        DWORD size = sizeof(data);
        const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
        return (status == ERROR_SUCCESS) ? std::to_string(data) : std::format("(not read: {})", status);
    };
    log(std::format("policies: HKLM NoDesktop={} HKCU NoDesktop={} HKCU Advanced HideIcons={} HKLM NoActiveDesktop={}",
                    hklmDword(explorerPolicies, L"NoDesktop"),
                    hkcuDword(explorerPolicies, L"NoDesktop"),
                    hkcuDword(advanced, L"HideIcons"),
                    hklmDword(explorerPolicies, L"NoActiveDesktop")));
    log(std::format("shell events: ShellDesktopSwitchEvent={} ShellReadyEvent={} Local\\ShellReadyEvent={}",
                    eventState(L"ShellDesktopSwitchEvent"),
                    eventState(L"ShellReadyEvent"),
                    eventState(L"Local\\ShellReadyEvent")));
    logDesktopView();
}

/// The PID of the server behind COM proxy @p object: its standard OBJREF's IPID carries the server's PID in Data2
/// (16 bits: 0xFFFF means the PID is larger). Also reports the OXID, so two proxies can be told apart.
[[nodiscard]] std::string serverOf(IUnknown* object)
{
    ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.releaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return "(CreateStreamOnHGlobal " + hex(hr) + ")";
    }
    hr = CoMarshalInterface(stream.get(), IID_IUnknown, object, MSHCTX_INPROC, nullptr, MSHLFLAGS_NORMAL);
    if (FAILED(hr))
    {
        return "(CoMarshalInterface " + hex(hr) + ")";
    }
    HGLOBAL memory = nullptr;
    std::string out;
    if (SUCCEEDED(GetHGlobalFromStream(stream.get(), &memory)) && memory != nullptr)
    {
        const void* locked = GlobalLock(memory);
        const SIZE_T size = GlobalSize(memory);
        // OBJREF: signature 'MEOW', flags (1 = standard), IID, then STDOBJREF: flags, cPublicRefs, OXID (8), OID (8), IPID.
        constexpr std::size_t IPID_OFFSET = 48;
        if (locked != nullptr && size >= IPID_OFFSET + sizeof(GUID))
        {
            const auto* bytes = static_cast<const std::byte*>(locked);
            std::uint32_t signature = 0;
            std::uint32_t flags = 0;
            std::uint64_t oxid = 0;
            GUID ipid{};
            std::memcpy(&signature, bytes, sizeof(signature));
            std::memcpy(&flags, bytes + 4, sizeof(flags));
            std::memcpy(&oxid, bytes + 32, sizeof(oxid));
            std::memcpy(&ipid, bytes + IPID_OFFSET, sizeof(ipid));
            if (signature == 0x574F454DU && flags == 1U)
            {
                out = std::format("serverPid16={} oxid=0x{:016X}", ipid.Data2, oxid);
            }
            else
            {
                out = std::format("(not a standard OBJREF: signature=0x{:08X} flags={})", signature, flags);
            }
        }
        if (locked != nullptr)
        {
            GlobalUnlock(memory);
        }
    }
    const LARGE_INTEGER start{};
    stream->Seek(start, STREAM_SEEK_SET, nullptr);
    CoReleaseMarshalData(stream.get());
    return out.empty() ? std::string("(no OBJREF read)") : out;
}

/// FindWindowSW(SWC_DESKTOP) with @p options: its answer, the window it names, that window's owner.
void logFindDesktop(IShellWindows* windows, int options, std::string_view label)
{
    VARIANT location{};
    VARIANT root{};
    VariantInit(&location);
    VariantInit(&root);
    long window = 0;
    ComPtr<IDispatch> dispatch;
    const HRESULT hr = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window, options, dispatch.releaseAndGetAddressOf());
    auto* const hwnd = reinterpret_cast<HWND>(static_cast<std::intptr_t>(window));
    log(std::format("FindWindowSW(SWC_DESKTOP, {}) = {} hwnd=0x{:X} owner={} dispatch={}",
                    label,
                    hex(hr),
                    static_cast<unsigned long>(window),
                    ownerOf(hwnd),
                    static_cast<bool>(dispatch)));
}

/// IShellWindows' count and each window it lists, with its HWND's owner.
void logShellWindows(IShellWindows* windows)
{
    long count = -1;
    const HRESULT hr = windows->get_Count(&count);
    log(std::format("IShellWindows::get_Count = {} count={}", hex(hr), count));
    for (long i = 0; SUCCEEDED(hr) && i < count; ++i)
    {
        VARIANT index{};
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        const HRESULT itemHr = windows->Item(index, item.releaseAndGetAddressOf());
        ComPtr<IWebBrowserApp> browser;
        SHANDLE_PTR window = 0;
        if (SUCCEEDED(itemHr) && item)
        {
            item->QueryInterface(IID_IWebBrowserApp, reinterpret_cast<void**>(browser.releaseAndGetAddressOf()));
        }
        if (browser)
        {
            browser->get_HWND(&window);
        }
        auto* const hwnd = reinterpret_cast<HWND>(window);
        log(std::format("  item {}: Item={} hwnd=0x{:X} owner={}", i, hex(itemHr), static_cast<std::uintptr_t>(window), ownerOf(hwnd)));
    }
}

/// One pass: a fresh ShellWindows object, which process serves it, what it lists and whether it has the desktop.
void probeShellWindows(std::string_view when)
{
    log(std::format("--- ShellWindows probe ({}) ---", when));
    ComPtr<IShellWindows> windows;
    const HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(windows.releaseAndGetAddressOf()));
    log(std::format("CoCreateInstance(CLSID_ShellWindows, CLSCTX_LOCAL_SERVER) = {}", hex(hr)));
    if (FAILED(hr) || !windows)
    {
        return;
    }
    const DWORD shellPid = ownerOf(GetShellWindow());
    log(std::format("ShellWindows server: {} (GetShellWindow()'s owner pid={}, low 16 bits={})",
                    serverOf(windows.get()),
                    shellPid,
                    shellPid & 0xFFFFU));
    logShellWindows(windows.get());
    logFindDesktop(windows.get(), SWFO_NEEDDISPATCH, "SWFO_NEEDDISPATCH");
    logFindDesktop(windows.get(), 0, "0");
    logFindDesktop(windows.get(), SWFO_INCLUDEPENDING, "SWFO_INCLUDEPENDING");
    logFindDesktop(windows.get(), SWFO_NEEDDISPATCH | SWFO_INCLUDEPENDING, "SWFO_NEEDDISPATCH|SWFO_INCLUDEPENDING");
}

TEST_F(CiRealSystemShellDiagnosticsTest, LogsWhoServesShellWindowsAndWhoOwnsTheDesktop)
{
#if defined(_M_ARM64EC)
    log("test binary built for: ARM64EC");
#elif defined(_M_ARM64) || defined(__aarch64__)
    log("test binary built for: ARM64");
#elif defined(_M_X64) || defined(__x86_64__)
    log("test binary built for: x64");
#else
    log("test binary built for: other");
#endif
    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) != FALSE)
    {
        log(std::format("IsWow64Process2: process={} native={}", machineName(processMachine), machineName(nativeMachine)));
    }
    const wchar_t* policies = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    log(std::format("UAC: EnableLUA={} ConsentPromptBehaviorAdmin={} PromptOnSecureDesktop={} FilterAdministratorToken={} "
                    "EnableUIADesktopToggle={}",
                    hklmDword(policies, L"EnableLUA"),
                    hklmDword(policies, L"ConsentPromptBehaviorAdmin"),
                    hklmDword(policies, L"PromptOnSecureDesktop"),
                    hklmDword(policies, L"FilterAdministratorToken"),
                    hklmDword(policies, L"EnableUIADesktopToggle")));
    const wchar_t* winlogon = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    log(std::format("Winlogon: Shell={} AutoAdminLogon={}",
                    registryString(HKEY_LOCAL_MACHINE, winlogon, L"Shell"),
                    registryString(HKEY_LOCAL_MACHINE, winlogon, L"AutoAdminLogon")));
    log(std::format("ShellWindows CLSID: default={} LocalServer32={} AppID={}",
                    hkcrString(L"CLSID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}", nullptr),
                    hkcrString(L"CLSID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}\\LocalServer32", nullptr),
                    hkcrString(L"CLSID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}", L"AppID")));
    log(std::format("ShellWindows AppID: RunAs={} LocalService={}",
                    hkcrString(L"AppID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}", L"RunAs"),
                    hkcrString(L"AppID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}", L"LocalService")));

    log("this test process: " + describeProcess(GetCurrentProcessId()));
    logWindow("GetShellWindow()", GetShellWindow());
    logWindow("Progman", FindWindowW(L"Progman", nullptr));
    logWindow("Shell_TrayWnd", FindWindowW(L"Shell_TrayWnd", nullptr));
    logExplorers("before CoCreateInstance");
    logDesktopContext();

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    log(std::format("CoInitializeEx(STA) = {}", hex(init)));
    probeShellWindows("now");
    logExplorers("after the first CoCreateInstance");
    std::this_thread::sleep_for(5s);
    probeShellWindows("after 5 s");
    logExplorers("after 5 s");
    if (SUCCEEDED(init))
    {
        CoUninitialize();
    }
    SUCCEED() << "diagnostic only: see the [3979] lines";
}

} // namespace
// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-union-access,performance-no-int-to-ptr,bugprone-invalid-enum-default-initialization)
