#pragma once

// The Windows Drivers facts (#1521), unprivileged and enumeration only: the kernel and file system driver
// services from the Service Control Manager (EnumServicesStatusExW with SERVICE_DRIVER), each with its start
// type and image path from Windows::readServiceConfig() (the Services tab's reader), and the image's file
// version and company from its version resource.
// The SCM is the one source: a running driver service is a loaded driver, and it names its start type and
// image. Loaded images that aren't services (the kernel, the HAL, kdcom and the like, which
// EnumDeviceDrivers would add) aren't listed. Stopped drivers are left out too, except (#1661) a Boot,
// System or Automatic start driver that stopped with an error: it failed to start. Errors a healthy machine
// shows aren't failures: never started, not supported, no hypervisor, and a Microsoft-signed image's
// general failure. Each image's signature is checked with WinVerifyTrust, embedded or through the
// catalogs, without the network, and cached by path and file time for the session (#1661).
// No driver is started, stopped or changed. Every call goes through an injectable table;
// WindowsSystemInfoProbe.cpp supplies the real one, tests substitute fakes.

#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsServiceProbeMath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::WindowsDrivers
{

/// One driver service, as listDriverServices() reads it.
struct DriverServiceRecord
{
    std::string name;                                       ///< lpServiceName
    std::string displayName;                                ///< lpDisplayName
    std::uint32_t serviceType = 0;                          ///< SERVICE_KERNEL_DRIVER (1) / SERVICE_FILE_SYSTEM_DRIVER (2)
    std::uint32_t currentState = 0;                         ///< SERVICE_STOPPED (1), SERVICE_RUNNING (4), ...
    ServiceStartType startType = ServiceStartType::Unknown; ///< From the service's configuration
    std::string binaryPath;                                 ///< lpBinaryPathName as configured: "\SystemRoot\System32\drivers\x.sys"
    std::uint32_t win32ExitCode = 0;                        ///< SERVICE_STATUS_PROCESS::dwWin32ExitCode
    std::uint32_t serviceSpecificExitCode = 0;              ///< dwServiceSpecificExitCode, when win32ExitCode says to look
};

/// An image file's version resource.
struct FileVersionRecord
{
    std::string version; ///< VS_FIXEDFILEINFO's file version, "10.0.26100.1"
    std::string company; ///< StringFileInfo's CompanyName
};

/// An image file as the signature cache knows it: a replaced or rebuilt driver is checked again.
struct FileStamp
{
    std::uint64_t lastWriteTime = 0; ///< FILETIME's 100 ns ticks
    std::uint64_t size = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

/// What checking an image's signature found.
struct SignatureCheck
{
    DriverSignature signature = DriverSignature::NotChecked;
    std::string note;   ///< Untrusted or Unknown: the failing result's text
    std::string signer; ///< Embedded or Catalog: the signing certificate's name ("Microsoft Windows")
};

/// One WinVerifyTrust call's answer.
struct TrustCheck
{
    std::int32_t result = 0; ///< 0 when the signature is trusted, else the failing HRESULT
    std::string signer;      ///< When trusted: the signing certificate's simple name; empty if unread
};

/// The calls readDrivers() makes.
struct Functions
{
    std::optional<std::vector<DriverServiceRecord>> (*listDriverServices)() = nullptr; ///< nullopt when the SCM fails
    std::string (*windowsDirectory)() = nullptr;                                       ///< "C:\Windows"
    FileVersionRecord (*readFileVersion)(const std::string& path) = nullptr;           ///< Empty fields when it has none
    /// FormatMessage's text for a Win32 error, NTSTATUS or HRESULT; empty when there is none.
    std::string (*errorMessage)(std::uint32_t code) = nullptr;
    /// The file's last write time and size; nullopt when it can't be read. Without it (or either verify
    /// call) signatures aren't checked.
    std::optional<FileStamp> (*fileStamp)(const std::string& path) = nullptr;
    /// WinVerifyTrust's answer for the file's own (embedded) signature: result 0 when it is trusted.
    TrustCheck (*verifyEmbeddedSignature)(const std::string& path) = nullptr;
    /// WinVerifyTrust's answer for the catalog that holds the file's hash: result 0 when it is trusted;
    /// nullopt when no catalog holds it; a failure's HRESULT when the catalogs couldn't be searched.
    std::optional<TrustCheck> (*verifyCatalogSignature)(const std::string& path) = nullptr;
};

inline constexpr std::uint32_t SERVICE_STOPPED_STATE = 1;    ///< SERVICE_STOPPED
inline constexpr std::uint32_t GEN_FAILURE_EXIT = 31;        ///< ERROR_GEN_FAILURE
inline constexpr std::uint32_t NOT_SUPPORTED_EXIT = 50;      ///< ERROR_NOT_SUPPORTED: the driver doesn't apply to this machine
inline constexpr std::uint32_t SERVICE_SPECIFIC_EXIT = 1066; ///< ERROR_SERVICE_SPECIFIC_ERROR
inline constexpr std::uint32_t NEVER_STARTED_EXIT = 1077;    ///< ERROR_SERVICE_NEVER_STARTED

/// WinVerifyTrust results (HRESULTs) that readDrivers() tells apart.
namespace TrustResult
{
inline constexpr std::int32_t NO_SIGNATURE = static_cast<std::int32_t>(0x800B0100U);         ///< TRUST_E_NOSIGNATURE
inline constexpr std::int32_t SUBJECT_FORM_UNKNOWN = static_cast<std::int32_t>(0x800B0003U); ///< TRUST_E_SUBJECT_FORM_UNKNOWN
inline constexpr std::int32_t SUBJECT_NOT_TRUSTED = static_cast<std::int32_t>(0x800B0004U);  ///< TRUST_E_SUBJECT_NOT_TRUSTED
inline constexpr std::int32_t EXPIRED = static_cast<std::int32_t>(0x800B0101U);              ///< CERT_E_EXPIRED
inline constexpr std::int32_t UNTRUSTED_ROOT = static_cast<std::int32_t>(0x800B0109U);       ///< CERT_E_UNTRUSTEDROOT
inline constexpr std::int32_t REVOKED = static_cast<std::int32_t>(0x800B010CU);              ///< CERT_E_REVOKED
inline constexpr std::int32_t UNTRUSTED_TEST_ROOT = static_cast<std::int32_t>(0x800B010DU);  ///< CERT_E_UNTRUSTEDTESTROOT
inline constexpr std::int32_t WRONG_USAGE = static_cast<std::int32_t>(0x800B0110U);          ///< CERT_E_WRONG_USAGE
inline constexpr std::int32_t EXPLICIT_DISTRUST = static_cast<std::int32_t>(0x800B0111U);    ///< TRUST_E_EXPLICIT_DISTRUST
inline constexpr std::int32_t CERT_SIGNATURE = static_cast<std::int32_t>(0x80096004U);       ///< TRUST_E_CERT_SIGNATURE
inline constexpr std::int32_t BAD_DIGEST = static_cast<std::int32_t>(0x80096010U);           ///< TRUST_E_BAD_DIGEST
} // namespace TrustResult

/// A hypervisor-facility NTSTATUS (0xC035xxxx): a virtualization driver that found no hypervisor, or not
/// the feature it serves ("A hypervisor feature is not available to the user"), on a machine without one.
[[nodiscard]] constexpr bool isHypervisorStatus(std::uint32_t code) noexcept
{
    constexpr std::uint32_t FACILITY_MASK = 0xFFFF0000U;
    constexpr std::uint32_t HYPERVISOR_ERROR = 0xC0350000U; // STATUS_SEVERITY_ERROR | FACILITY_HYPERVISOR (0x35)
    return (code & FACILITY_MASK) == HYPERVISOR_ERROR;
}

/// A stopped driver's exit code that may mean it tried to start and failed. 1077 is "never started" (the
/// usual code of a driver for hardware that isn't there), 50 "not supported" (a driver that declined to
/// load on this machine) and a hypervisor-facility NTSTATUS a virtualization driver on a machine without
/// the hypervisor; none is a failure. readDrivers() also excuses ERROR_GEN_FAILURE from a Microsoft-signed
/// image (isBenignInboxFailure()).
[[nodiscard]] constexpr bool isStartFailureCode(std::uint32_t win32ExitCode) noexcept
{
    return win32ExitCode != 0 && win32ExitCode != NEVER_STARTED_EXIT && win32ExitCode != NOT_SUPPORTED_EXIT &&
           !isHypervisorStatus(win32ExitCode);
}

/// Windows' own signer names: an image they sign trustedly is an inbox one. "Microsoft Windows Hardware
/// Compatibility Publisher" (WHQL, which signs third-party drivers) is deliberately not among them.
[[nodiscard]] inline bool isMicrosoftSigned(DriverSignature signature, std::string_view signer) noexcept
{
    const bool trusted = signature == DriverSignature::Embedded || signature == DriverSignature::Catalog;
    return trusted && (signer == "Microsoft Windows" || signer == "Microsoft Corporation");
}

/// A start failure that is expected on a healthy machine: ERROR_GEN_FAILURE from a Microsoft-signed image
/// (hwpolicy, the Hardware Policy Driver, commonly stops so). From any other image it still counts.
[[nodiscard]] inline bool isBenignInboxFailure(std::uint32_t win32ExitCode, DriverSignature signature, std::string_view signer) noexcept
{
    return win32ExitCode == GEN_FAILURE_EXIT && isMicrosoftSigned(signature, signer);
}

/// A start type with which Windows loads the driver itself, at boot or startup.
[[nodiscard]] constexpr bool startsWithWindows(ServiceStartType startType) noexcept
{
    return startType == ServiceStartType::Boot || startType == ServiceStartType::System || startType == ServiceStartType::Automatic ||
           startType == ServiceStartType::AutomaticDelayed;
}

/// A driver Windows should have loaded that is stopped with an error. A stopped Manual (demand) start
/// driver isn't one: most are for hardware that isn't present.
[[nodiscard]] inline bool failedToStart(const DriverServiceRecord& service) noexcept
{
    return service.currentState == SERVICE_STOPPED_STATE && startsWithWindows(service.startType) &&
           isStartFailureCode(service.win32ExitCode);
}

/// A failing WinVerifyTrust result that says the signature is there but isn't trusted, rather than that
/// the check couldn't be made.
[[nodiscard]] constexpr bool isTrustFailure(std::int32_t result) noexcept
{
    constexpr std::array<std::int32_t, 9> FAILURES{
        TrustResult::SUBJECT_NOT_TRUSTED,
        TrustResult::EXPIRED,
        TrustResult::UNTRUSTED_ROOT,
        TrustResult::REVOKED,
        TrustResult::UNTRUSTED_TEST_ROOT,
        TrustResult::WRONG_USAGE,
        TrustResult::EXPLICIT_DISTRUST,
        TrustResult::CERT_SIGNATURE,
        TrustResult::BAD_DIGEST,
    };
    return std::ranges::find(FAILURES, result) != FAILURES.end();
}

/// The signature and the result that decided it (0 when signed).
struct SignatureVerdict
{
    DriverSignature signature = DriverSignature::NotChecked;
    std::int32_t result = 0;
};

/// The signature from the embedded check's result and, when that failed, the catalog check's.
[[nodiscard]] constexpr SignatureVerdict classifySignature(std::int32_t embedded, std::optional<std::int32_t> catalog) noexcept
{
    if (embedded == 0)
    {
        return {.signature = DriverSignature::Embedded, .result = 0};
    }
    if (catalog.has_value())
    {
        if (*catalog == 0)
        {
            return {.signature = DriverSignature::Catalog, .result = 0};
        }
        return {.signature = isTrustFailure(*catalog) ? DriverSignature::Untrusted : DriverSignature::Unknown, .result = *catalog};
    }
    if (embedded == TrustResult::NO_SIGNATURE || embedded == TrustResult::SUBJECT_FORM_UNKNOWN)
    {
        return {.signature = DriverSignature::Unsigned, .result = embedded};
    }
    return {.signature = isTrustFailure(embedded) ? DriverSignature::Untrusted : DriverSignature::Unknown, .result = embedded};
}

/// Signature checks by image path and file stamp, kept for the session (the probe's lifetime). Only definite
/// answers are kept: a check that failed is tried again on the next read.
class SignatureCache
{
  public:
    [[nodiscard]] std::optional<SignatureCheck> find(const std::string& path, const FileStamp& stamp) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Entries.find(path);
        if (found == m_Entries.end() || found->second.stamp != stamp)
        {
            return std::nullopt;
        }
        return found->second.check;
    }

    void store(const std::string& path, const FileStamp& stamp, SignatureCheck check)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Entries.insert_or_assign(path, Entry{.stamp = stamp, .check = std::move(check)});
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Entries.size();
    }

  private:
    struct Entry
    {
        FileStamp stamp;
        SignatureCheck check;
    };
    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, Entry> m_Entries;
};

/// A driver's image as a file path. The SCM keeps it as the kernel loads it: "\SystemRoot\..." or a path
/// relative to the Windows directory ("System32\drivers\x.sys"), an NT "\??\C:\..." path, or nothing, which
/// means "%SystemRoot%\System32\drivers\<name>.sys".
[[nodiscard]] inline std::string driverImagePath(std::string_view binaryPath, std::string_view name, std::string_view windowsDir)
{
    const auto startsWithIgnoringCase = [](std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() &&
               std::ranges::equal(text.substr(0, prefix.size()),
                                  prefix,
                                  [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    };
    std::string dir(windowsDir);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/'))
    {
        dir.pop_back();
    }
    // A driver's image path has no arguments, and may hold spaces unquoted; drop surrounding quotes and spaces.
    std::string_view path = binaryPath;
    const std::size_t first = path.find_first_not_of(" \t\"");
    path = first == std::string_view::npos ? std::string_view{} : path.substr(first, path.find_last_not_of(" \t\"") - first + 1);
    if (path.empty())
    {
        return name.empty() || dir.empty() ? std::string{} : std::format(R"({}\System32\drivers\{}.sys)", dir, name);
    }
    for (const std::string_view prefix : {std::string_view(R"(\??\)"), std::string_view(R"(\\?\)")})
    {
        if (path.starts_with(prefix))
        {
            return std::string(path.substr(prefix.size()));
        }
    }
    for (const std::string_view root : {std::string_view("\\SystemRoot\\"), std::string_view("%SystemRoot%\\")})
    {
        if (startsWithIgnoringCase(path, root))
        {
            return dir.empty() ? std::string{} : std::format("{}\\{}", dir, path.substr(root.size()));
        }
    }
    const bool absolute = path.starts_with('\\') || (path.size() >= 2 && path[1] == ':');
    if (absolute)
    {
        return std::string(path);
    }
    return dir.empty() ? std::string{} : std::format("{}\\{}", dir, path);
}

/// VS_FIXEDFILEINFO's file version: "10.0.26100.1"; empty when both halves are 0.
[[nodiscard]] inline std::string formatFileVersion(std::uint32_t ms, std::uint32_t ls)
{
    constexpr unsigned HALF = 16;
    constexpr std::uint32_t MASK = 0xFFFF;
    if (ms == 0 && ls == 0)
    {
        return {};
    }
    return std::format("{}.{}.{}.{}", ms >> HALF, ms & MASK, ls >> HALF, ls & MASK);
}

/// An error code's text through @p fns, without the trailing period and line break FormatMessage adds; the
/// code itself when there is no text ("error 0xC035001E" for an NTSTATUS or HRESULT, "error 31" otherwise).
[[nodiscard]] inline std::string describeError(const Functions& fns, std::uint32_t code)
{
    std::string text = fns.errorMessage != nullptr ? fns.errorMessage(code) : std::string{};
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0 || text.back() == '.'))
    {
        text.pop_back();
    }
    if (!text.empty())
    {
        return text;
    }
    constexpr std::uint32_t SEVERITY_BIT = 0x80000000U;
    return (code & SEVERITY_BIT) != 0 ? std::format("error 0x{:08X}", code) : std::format("error {}", code);
}

/// Why a driver that failed to start stopped: its exit code's text, or its own (service-specific) code.
[[nodiscard]] inline std::string startErrorText(const Functions& fns, const DriverServiceRecord& service)
{
    if (service.win32ExitCode == SERVICE_SPECIFIC_EXIT)
    {
        return std::format("service-specific error {}", service.serviceSpecificExitCode);
    }
    return describeError(fns, service.win32ExitCode);
}

/// @p path's signature through @p fns: the embedded signature, else the catalogs; from @p cache when it
/// already holds this path with the same file stamp. NotChecked when the table has no signature calls.
[[nodiscard]] inline SignatureCheck checkSignature(const Functions& fns, SignatureCache* cache, const std::string& path)
{
    if (fns.fileStamp == nullptr || fns.verifyEmbeddedSignature == nullptr || fns.verifyCatalogSignature == nullptr)
    {
        return {};
    }
    const std::optional<FileStamp> stamp = fns.fileStamp(path);
    if (!stamp.has_value())
    {
        return {.signature = DriverSignature::Unknown, .note = "The image file couldn't be read", .signer = {}};
    }
    if (cache != nullptr)
    {
        if (std::optional<SignatureCheck> cached = cache->find(path, *stamp); cached.has_value())
        {
            return std::move(*cached);
        }
    }
    TrustCheck embedded = fns.verifyEmbeddedSignature(path);
    std::optional<TrustCheck> catalog = embedded.result == 0 ? std::nullopt : fns.verifyCatalogSignature(path);
    const SignatureVerdict verdict =
        classifySignature(embedded.result, catalog.has_value() ? std::optional<std::int32_t>(catalog->result) : std::nullopt);
    SignatureCheck check{.signature = verdict.signature, .note = {}, .signer = {}};
    if (verdict.signature == DriverSignature::Untrusted || verdict.signature == DriverSignature::Unknown)
    {
        check.note = describeError(fns, static_cast<std::uint32_t>(verdict.result)); // an HRESULT's bits
    }
    else if (verdict.signature == DriverSignature::Embedded)
    {
        check.signer = std::move(embedded.signer);
    }
    else if (verdict.signature == DriverSignature::Catalog && catalog.has_value())
    {
        check.signer = std::move(catalog->signer);
    }
    if (cache != nullptr && verdict.signature != DriverSignature::Unknown)
    {
        cache->store(path, *stamp, check);
    }
    return check;
}

/// The running driver services, and those that failed to start, with their images' versions and
/// signatures, through @p fns; @p cache (optional) keeps the signature checks between reads.
inline void readDrivers(DriversInfo& info, const Functions& fns, SignatureCache* cache = nullptr)
{
    info.available = true;
    info.family = OsFamily::Windows;
    const std::optional<std::vector<DriverServiceRecord>> services = fns.listDriverServices();
    if (!services.has_value())
    {
        return;
    }
    info.listed = true;
    const std::string windowsDir = fns.windowsDirectory();
    constexpr std::uint32_t FILE_SYSTEM_DRIVER = 0x2;
    for (const DriverServiceRecord& service : *services)
    {
        const bool failed = failedToStart(service);
        if (service.currentState == SERVICE_STOPPED_STATE && !failed)
        {
            continue; // not loaded, and not expected to be
        }
        KernelDriver driver;
        driver.name = service.name;
        driver.displayName = service.displayName;
        driver.fileSystem = (service.serviceType & FILE_SYSTEM_DRIVER) != 0;
        driver.state = Windows::ServiceMath::stateFromCode(service.currentState);
        driver.startType = service.startType;
        driver.path = driverImagePath(service.binaryPath, service.name, windowsDir);
        std::string signer;
        if (!driver.path.empty())
        {
            FileVersionRecord file = fns.readFileVersion(driver.path);
            driver.version = std::move(file.version);
            driver.company = std::move(file.company);
            SignatureCheck signature = checkSignature(fns, cache, driver.path);
            driver.signature = signature.signature;
            driver.signatureNote = std::move(signature.note);
            signer = std::move(signature.signer);
        }
        if (failed)
        {
            if (isBenignInboxFailure(service.win32ExitCode, driver.signature, signer))
            {
                continue; // stopped as it does on a healthy machine: left out, as other stopped drivers are
            }
            driver.startError = startErrorText(fns, service);
        }
        info.drivers.push_back(std::move(driver));
    }
}

} // namespace Platform::WindowsDrivers
