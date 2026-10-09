#pragma once

// The System Information page's static facts (#1399), read once (and again on Refresh) off the UI
// thread. Each section of the page has a raw struct here and a read*() method on the probe; the App
// layer turns them into labelled rows (App/Panels/SystemInfoSections.h).

#include <cstdint>
#include <optional>
#include <string>

namespace Platform
{

/// Which OS the facts come from; it picks the rows the Operating system section shows.
enum class OsFamily : std::uint8_t
{
    Unknown,
    Windows,
    Linux,
};

/// The Operating system & session facts (#1512), raw as the OS reports them. A string the probe could
/// not read is left empty and a time 0; the page shows those as unavailable.
struct OsInfo
{
    OsFamily family = OsFamily::Unknown;
    std::string name;                     ///< "Windows 11 Home" / os-release PRETTY_NAME
    std::string version;                  ///< Windows DisplayVersion ("25H2") / os-release VERSION_ID
    std::string build;                    ///< Windows "CurrentBuild.UBR"
    std::string kernel;                   ///< Linux: uname's sysname and release
    std::string architecture;             ///< The machine's native architecture ("x64", "ARM64", "x86_64")
    std::uint64_t installUnixSeconds = 0; ///< Windows InstallDate
    std::uint64_t bootUnixSeconds = 0;
    std::string computerName;            ///< DNS host name
    std::string domainOrWorkgroup;       ///< Windows: the domain or workgroup joined
    bool joinedToDomain = false;         ///< domainOrWorkgroup is a domain, not a workgroup
    std::string userName;                ///< Windows DOMAIN\user, Linux the login name
    std::string locale;                  ///< "en-US" / "en_US.UTF-8"
    std::string timeZone;                ///< "Pacific Standard Time" / "America/Los_Angeles"
    std::optional<int> utcOffsetMinutes; ///< The current offset east of UTC, daylight saving included
    std::string systemDirectory;         ///< Windows
    std::string windowsDirectory;        ///< Windows
    std::string initSystem;              ///< Linux: /proc/1/comm
    std::string desktop;                 ///< Linux: XDG_CURRENT_DESKTOP
    std::string sessionType;             ///< Linux: XDG_SESSION_TYPE ("wayland", "x11", "tty")
    std::string virtualization;          ///< Linux: a container or VM hint, or "None detected"
};

/// What the platform can read at all.
struct SystemInfoCapabilities
{
    bool hasOs = false;            ///< readOs() returns facts.
    std::string unavailableReason; ///< Why hasOs is false, for the UI. Empty when it is true.
};

/// Reads the System Information page's static facts. Called from one thread at a time, which may
/// differ between calls; each read is a fresh read, nothing is cached.
class ISystemInfoProbe
{
  public:
    virtual ~ISystemInfoProbe() = default;

    ISystemInfoProbe() = default;
    ISystemInfoProbe(const ISystemInfoProbe&) = default;
    ISystemInfoProbe& operator=(const ISystemInfoProbe&) = default;
    ISystemInfoProbe(ISystemInfoProbe&&) = default;
    ISystemInfoProbe& operator=(ISystemInfoProbe&&) = default;

    [[nodiscard]] virtual SystemInfoCapabilities capabilities() const = 0;

    /// The Operating system & session facts; a fact that can't be read is left empty.
    [[nodiscard]] virtual OsInfo readOs() = 0;
};

/// The probe for a platform without an implementation: no facts, and hasOs false so the UI says so.
class UnsupportedSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    [[nodiscard]] SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = false, .unavailableReason = "System information isn't available on this platform"};
    }

    [[nodiscard]] OsInfo readOs() override
    {
        return {};
    }
};

} // namespace Platform
