#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW: the command line's arguments (#3093 part 2); after windows.h
// clang-format on
#endif

#include "App/AboutLayer.h"
#include "App/DebugLogFile.h"
#include "App/DemoLayer.h"
#include "App/DemoOverride.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/InstanceLock.h"
#include "App/LaunchOptions.h"
#ifdef __linux__
#include "App/LinuxHeldFolder.h"
#endif
#include "App/PanelPlatforms.h"
#include "App/Panels/RestartAsAdministratorButton.h"
#include "App/RenderMetricsOverride.h"
#include "App/ScreenshotLayer.h"
#include "App/ScreenshotOverride.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/SyntheticScenario.h"
#include "App/TaskManagerNoticeLayer.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "App/WindowOverride.h"
#include "Core/Application.h"
#include "Core/ConfigDirOverride.h"
// NOLINTNEXTLINE(misc-include-cleaner) - used in the NDEBUG (release) branch below, invisible to debug-config analysis
#include "Core/EnvRead.h"
#include "Core/EnvUtils.h"
#include "Core/Event.h"
#include "Core/LocaleSetup.h"
#include "Core/Utf8Path.h"
#include "Core/WindowConstants.h"
#include "Platform/IElevatedRestart.h"
#ifdef _WIN32
#include "Platform/Factory.h"
#include "Platform/IHandedOverFolder.h"
#include "Platform/Windows/DllSearchPolicy.h"
#include "Platform/Windows/SdlEnvironment.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#endif
#ifdef __linux__
#include "Domain/ProcessSecurity.h"
#include "Platform/Linux/RootCapabilities.h"
#include "Platform/Linux/RootEnvironment.h"
#include "Platform/Linux/SdlEnvironment.h"
#include "Platform/Linux/SessionUser.h"

#include <system_error>

#include <unistd.h> // geteuid(): a root session's log (#4150)
#endif
#include "UI/Format.h"
#include "UI/UILayer.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <utility>
#ifdef _WIN32
// spdlog's msvc_sink.h is itself guarded on _WIN32; the other sinks are portable.
#include <spdlog/sinks/msvc_sink.h>

#include <cstdint>      // std::uint32_t, the Standard TaskSmack's PID in the hand-off (#3781)
#include <system_error> // the own config folder's fallback (#4106)
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio> // NOLINT(misc-include-cleaner) - FILE* is used only in the _WIN32 console-attach branch below
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <locale>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace
{
/// Sets the C and C++ locales to the user's, with UTF-8 text (Core/LocaleSetup.h). Runs before logging is up: the caller logs the returned
/// summary once it is.
auto initializeLocale() -> const Core::LocaleSetup::Summary&
{
    // The global C++ locale keeps the classic numeric facets, so streams, "{:L}" and parsing are
    // locale-independent. Displayed numbers follow the user's locale through UI::Format, which
    // takes its punctuation from LocaleSetup::userNumberPunctuation() (#1648, slice E).
    const Core::LocaleSetup::Summary& summary = Core::LocaleSetup::initialize(Core::LocaleSetup::NumericFacets::Classic);
    UI::Format::setDisplayPunctuation(Core::LocaleSetup::userNumberPunctuation());
    const std::locale global;
    std::cout.imbue(global);
    std::cerr.imbue(global);
    std::cin.imbue(global);
    // No SetConsoleOutputCP here (#4317): this runs before AttachConsole/AllocConsole, so it set nothing
    // on a console that did not exist yet and, once attached, would change the parent cmd's code page.
    // The log's console sink writes UTF-16 (SPDLOG_UTF8_TO_WCHAR_CONSOLE), so no code page is needed.
    return summary;
}

/// @p duration in seconds, for the hand-off's log lines (#3781).
[[nodiscard]] auto inSeconds(std::chrono::milliseconds duration) -> double
{
    return std::chrono::duration<double>(duration).count();
}

#ifdef _WIN32
/// How the elevated copy's wait for the Standard one ended, for the log.
[[nodiscard]] constexpr auto describeHandoffWait(Platform::HandoffWait wait) -> const char*
{
    switch (wait)
    {
    case Platform::HandoffWait::Exited:
        return "which closed";
    case Platform::HandoffWait::NotRunning:
        return "which had already closed";
    case Platform::HandoffWait::NotThatProcess:
        return "but that PID is another process's, so it wasn't waited for";
    case Platform::HandoffWait::Unopenable:
        return "which this account may not open, so the instance lock was the hand-off";
    case Platform::HandoffWait::TimedOut:
        return "which was still running after the wait";
    }
    return "";
}
#endif

/// Whether this session's environment may be the less-privileged side's (#4104), the one rule for the test hooks that
/// write files: on Windows, elevated or restarted with --handoff (DllSearchPolicy::system32OnlyForThisProcess(), the
/// rule the DLL search order and SDL's variables follow, #4103); on Linux, secure execution or root with an environment
/// that may be a user's (SessionUser::environmentMayBeAnotherUsers(), #4154: setcap, sudo, pkexec, doas, su).
[[nodiscard]] bool lessPrivilegedEnvironment()
{
#ifdef _WIN32
    return Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess();
#elif defined(__linux__)
    return Platform::SessionUser::thisProcessMayHaveAnotherUsersEnvironment(); // #4154: worked out once, there
#else
    return false;
#endif
}

auto runApp(const App::LaunchOptions& launch) -> int
{
#ifdef __linux__
    // An Administrator session keeps only the capabilities TaskSmack uses (#3621). First, before any thread
    // starts: capabilities are per thread, and each thread made later inherits the narrowed sets. Logged below.
    const Platform::RootCapabilities::DropResult capabilityDrop = Platform::RootCapabilities::dropUnused();
#endif
    const Core::LocaleSetup::Summary& localeSummary = initializeLocale();

// Required on Windows to see console output when launching from an IDE or debugger. Debug-only
// on purpose: a release-class GUI build must not pop up a console window.
#if defined(_WIN32) && !defined(NDEBUG)
    // LCOV_EXCL_START - main.cpp runs only in TaskSmack.exe; no test binary links it (#2039)
    // Try to attach to parent console if it's a console app
    // If no parent console exists because it's a Windows app create our own console
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
    {
        AllocConsole();
        // Redirect stdout/stderr to the new console
        // NOLINTNEXTLINE(misc-const-correctness) - freopen_s writes to these pointers
        FILE* out = nullptr;
        // NOLINTNEXTLINE(misc-const-correctness) - freopen_s writes to these pointers
        FILE* err = nullptr;
        if (freopen_s(&out, "CONOUT$", "w", stdout) != 0)
        {
            // Redirection failed, but continue - spdlog will still work via msvc_sink
        }
        if (freopen_s(&err, "CONOUT$", "w", stderr) != 0)
        {
            // Redirection failed, but continue - spdlog will still work via msvc_sink
        }
    }
    // LCOV_EXCL_STOP
#endif

    // Restarted as administrator (#2728, the hand-off in Platform/IElevatedRestart.h): wait for the Standard
    // TaskSmack to close where this account may open it, then take the launching user's config directory only if it
    // passes the checks for a directory handed over from the less-privileged side; otherwise keep this account's
    // own. The instance lock below, tried again for a while, covers a Standard copy this one couldn't wait on.
    // Logged below. A Linux session is never restarted (#3580), so there the arguments are ignored.
    // Its environment may be the less-privileged side's (#4104): every test hook that writes a file or folder at a path it
    // names (TASKSMACK_CONFIG_DIR, _SCREENSHOT, _DEMO's metrics export, _RENDER_METRICS) is ignored, before anything reads
    // one, so nothing is written where that side chose (#3920, #4090). One rule on both platforms, lessPrivilegedEnvironment().
    // Keep this ahead of every read of a hook: each hook keeps its first answer (a function-local static), so a hook read
    // before the gate would keep a path the less-privileged side chose for the whole run.
    if (lessPrivilegedEnvironment()) // LCOV_EXCL_LINE - an elevated or root-for-a-user session only (#2039)
    {
        Core::ignoreWritingHooks(); // LCOV_EXCL_LINE
    }
#ifdef _WIN32
    const bool handoffSession = launch.handoffPid.has_value();
    std::optional<Platform::HandoffWait> handoffWait;
    std::string handoffDirRefused;
    // The handed-over folder as checked, held open until runApp() returns (#3787): the one owner of the hold.
    // ConfigDirOverride keeps only a weak reference, so a save after this is released fails closed.
    std::shared_ptr<Platform::IHandedOverFolder> handedOverFolder;
    std::chrono::milliseconds handoffWaited{0};
    if (handoffSession)
    {
        // No window opens until the hand-off ends (#3781), so it says how it goes while it goes: to the debugger and the
        // console. tasksmack-debug.log can't be opened yet (opening it truncates it, and the Standard copy may still be
        // writing its close there); it gets the outcome once it opens, below.
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("TaskSmack",
                                                                    spdlog::sinks_init_list{
                                                                        std::make_shared<spdlog::sinks::msvc_sink_mt>(),
                                                                        std::make_shared<spdlog::sinks::stdout_color_sink_mt>(),
                                                                    }));
        const std::uint32_t standardPid = *launch.handoffPid;
        spdlog::info("Restarted as administrator: waiting up to {:.0f} s for the Standard TaskSmack (PID {}) to close",
                     inSeconds(Platform::HANDOFF_PATIENCE),
                     standardPid);
        const auto waitStarted = std::chrono::steady_clock::now();
        handoffWait = Platform::waitForHandoff(
            standardPid,
            launch.handoffStartTicks,
            [standardPid](std::chrono::milliseconds waited)
            {
                spdlog::info("Still waiting for the Standard TaskSmack (PID {}) to close: {:.1f} s so far", standardPid, inSeconds(waited));
            });
        handoffWaited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStarted);
        spdlog::info("The wait for the Standard TaskSmack (PID {}) ended after {:.1f} s, {}",
                     standardPid,
                     inSeconds(handoffWaited),
                     describeHandoffWait(*handoffWait));
        if (launch.configDir.has_value())
        {
            const std::filesystem::path& handedOver = *launch.configDir; // exactly as the command line gave it (#4310)
            if (handoffWait == Platform::HandoffWait::NotThatProcess)
            {
                handoffDirRefused = "the process it names isn't the TaskSmack that restarted this one";
            }
            else if (auto checked = Platform::checkHandedOverConfigDir(handedOver); !checked.has_value())
            {
                handoffDirRefused = checked.error();
            }
            else
            {
                handedOverFolder = std::move(*checked);
                Core::ConfigDirOverride::adoptHandedOver(handedOver, handedOverFolder);
            }
        }
    }
    // Elevated or handed off with no folder handed over (a refused hand-off, or Run as administrator; #4106): this
    // account's own folder, %APPDATA%\TaskSmack, is the less-privileged side's as much as a handed-over one, since its
    // known folder and anything on its path are the user's to change. So it is held as one (checkOwnConfigDir()), and
    // adopted as one: UserConfig, the instance lock, the themes and Open Themes Folder go only through the folder held,
    // and its dump folder and recent commands aren't offered. Refused, it is adopted with nothing held, which fails
    // closed: this run starts from the defaults and saves nothing. The same rule as the DLL search order and SDL's
    // variables (#3897, #4103); a session whose environment is its own (no UAC split token) is unchanged.
    std::string ownDirRefused;
    bool ownDirHeld = false;
    // LCOV_EXCL_START - TaskSmack.exe only, elevated (#2039); checkOwnConfigDir() has the tests
    if (handedOverFolder == nullptr && Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess())
    {
        auto own = Platform::holdOwnElevatedConfigFolder();
        // A known folder that can't be read still fails closed: adopted, with nothing held, at the working directory.
        std::error_code cwdError;
        const std::filesystem::path ownDir = own.path.empty() ? std::filesystem::current_path(cwdError) : own.path;
        if (own.held.has_value() && *own.held != nullptr)
        {
            handedOverFolder = std::move(*own.held);
            Core::ConfigDirOverride::adoptHandedOver(ownDir, handedOverFolder);
            ownDirHeld = true;
        }
        else
        {
            ownDirRefused = own.held.has_value() ? std::string("it can't be held") : own.held.error();
            Core::ConfigDirOverride::adoptHandedOver(ownDir);
        }
    }
    // LCOV_EXCL_STOP
#else
    constexpr bool handoffSession = false;
    constexpr std::chrono::milliseconds handoffWaited{0};
#endif
#ifdef __linux__
    // Run as root for a user (sudo, pkexec, #3580), the config directory is that user's, the less-privileged side's, as
    // a handed-over one is on Windows (#3759): UserConfig writes through no link of theirs in it, and the dump folder and
    // recent commands stored there aren't offered. Before the configuration is first read.
    // Its environment is the less-privileged side's too: the writing test hooks were ignored above (#4090, #4104).
    // And the folder is held from here on, the one way into it (#4167, #4257), as an elevated Windows session holds its
    // handed-over one (#3774): config.toml is read and saved, the instance lock taken and the user themes listed and read
    // only relative to it, never by a path a link of theirs swapped in later could redirect. Refused (it can't be walked or
    // made for them without a link of theirs), it is adopted with nothing held, which fails closed: this run starts from
    // the defaults, saves nothing and takes no lock there.
    std::shared_ptr<App::LinuxHeldFolder> heldUserFolder;
    std::string heldUserFolderRefused;
    if (Platform::SessionUser::invokingUserHome().has_value()) // LCOV_EXCL_START - root for a user only (UserConfigRootTest)
    {
        Core::ConfigDirOverride::markLessPrivilegedFolder();
        const std::filesystem::path userDir = App::UserConfig::configDirectoryAtStartup();
        if (auto held = App::LinuxHeldFolder::hold(userDir, ::geteuid()); held.has_value())
        {
            heldUserFolder = std::move(*held);
            Core::ConfigDirOverride::adoptHandedOver(userDir, heldUserFolder);
        }
        else
        {
            heldUserFolderRefused = std::move(held.error());
            Core::ConfigDirOverride::adoptHandedOver(userDir);
        }
    } // LCOV_EXCL_STOP
#endif

    // Load user configuration early so we can apply window geometry before creating the SDL window.
    auto& userConfig = App::UserConfig::get();

    // One TaskSmack per config directory (#1230): two instances saving the same config.toml could
    // undo each other's settings change. Held until runApp() returns; the OS drops it on any exit.
    // A lock that can't be taken at all (read-only directory, say) doesn't stop TaskSmack starting.
    // Taken before the file logger below opens tasksmack-debug.log, which truncates it: a rejected
    // second launch must not erase the running instance's log. Its warning goes to the default
    // (console) logger.
    // An elevated restart tries it again while the Standard copy it replaces closes (#2728): that copy holds it
    // until it has saved its settings, so the settings below are read only after its last save. It gets what the
    // wait above left of the hand-off's patience (#3781), and says while it tries that it is still waiting.
    const std::filesystem::path instanceLockPath = App::instanceLockPath(userConfig.configPath().parent_path());
    const std::chrono::milliseconds lockPatience =
        handoffSession ? Platform::handoffLockPatience(handoffWaited) : std::chrono::milliseconds{0};
    const App::LockWaitProgress lockProgress{
        .report = [](std::chrono::milliseconds tried)
        { spdlog::info("Still waiting for the Standard TaskSmack to let go of the instance lock: {:.1f} s so far", inSeconds(tried)); },
        .every = handoffSession ? Platform::HANDOFF_PROGRESS_EVERY : std::chrono::milliseconds{0},
    };
    if (handoffSession)
    {
        spdlog::info("Taking the instance lock, trying for up to {:.1f} s while the Standard TaskSmack lets go of it",
                     inSeconds(lockPatience));
    }
#ifdef _WIN32
    const auto lockStarted = std::chrono::steady_clock::now();
    // A handed-over folder's lock is opened only through the folder held (#3774), never by its path. A refused own folder
    // (#4106) gets none: nothing is opened or made there.
    std::unique_ptr<App::InstanceLock> instanceLock;
    if (handedOverFolder != nullptr)
    {
        instanceLock =
            App::acquireInstanceLock(*handedOverFolder, instanceLockPath.filename(), lockPatience, App::LOCK_RETRY_EVERY, lockProgress);
    }
    else if (!ownDirRefused.empty())
    {
        instanceLock =
            std::make_unique<App::InstanceLock>(App::InstanceLock::NotTaken{.why = "this account's settings folder was refused"});
    }
    else
    {
        instanceLock = App::acquireInstanceLock(instanceLockPath, lockPatience, App::LOCK_RETRY_EVERY, lockProgress);
    }
#elif defined(__linux__)
    // Run as root for a user, the lock too is opened only through the folder held (#4167); a refused folder gets none.
    std::unique_ptr<App::InstanceLock> instanceLock;
    if (heldUserFolder != nullptr) // LCOV_EXCL_START - root for a user only
    {
        instanceLock =
            App::acquireInstanceLock(*heldUserFolder, instanceLockPath.filename(), lockPatience, App::LOCK_RETRY_EVERY, lockProgress);
    }
    else if (!heldUserFolderRefused.empty())
    {
        instanceLock = std::make_unique<App::InstanceLock>(App::InstanceLock::NotTaken{.why = "the user's settings folder was refused"});
    } // LCOV_EXCL_STOP
    else
    {
        instanceLock = App::acquireInstanceLock(instanceLockPath, lockPatience, App::LOCK_RETRY_EVERY, lockProgress);
    }
#else
    const std::unique_ptr<App::InstanceLock> instanceLock =
        App::acquireInstanceLock(instanceLockPath, lockPatience, App::LOCK_RETRY_EVERY, lockProgress);
#endif
#ifdef _WIN32
    // Windows only, as the hand-off is (#3580): how long the lock's tries took, for the log (#3781).
    const auto lockWaited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lockStarted);
    if (handoffSession)
    {
        spdlog::info("The instance lock's tries ended after {:.1f} s: {}",
                     inSeconds(lockWaited),
                     instanceLock->status() == App::InstanceLock::Status::HeldByAnotherInstance ? "still held" : "done");
    }
#endif
    if (const std::optional<int> lockExitCode = App::exitCodeForLockStatus(instanceLock->status()); lockExitCode.has_value())
    {
        spdlog::warn("TaskSmack is already running with the settings in {}; exiting", Core::pathToUtf8(instanceLockPath.parent_path()));
        if (handoffSession)
        {
            // The Standard copy this one replaces didn't close in time (#3781): say so, not just that one runs.
            const std::string message = std::format("TaskSmack couldn't restart as administrator: another TaskSmack, most likely the "
                                                    "Standard one it replaces, still held your settings after {:.0f} seconds.\n\nClose "
                                                    "it, then choose Restart as administrator again.",
                                                    inSeconds(Platform::HANDOFF_PATIENCE));
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "TaskSmack", message.c_str(), nullptr);
            return *lockExitCode;
        }
#ifdef _WIN32
        // Ctrl+Shift+Esc with TaskSmack in place of Task Manager (#3072): the running one comes to the
        // front, as Task Manager's own second start does, rather than a message box saying it runs.
        if (launch.asTaskManager && Platform::showRunningTaskSmack()) // LCOV_EXCL_LINE - TaskSmack.exe only (#2039)
        {
            return *lockExitCode;
        }
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                                 "TaskSmack",
                                 "TaskSmack is already running.\n\nOnly one TaskSmack can run at a time, so that two can't overwrite "
                                 "each other's settings.",
                                 nullptr);
        // Not EXIT_SUCCESS (#2250): a profiling script that launched this copy must see it never ran.
        return *lockExitCode;
    }

    // Logger construction runs in EVERY configuration. It used to sit inside the console-attach
    // guard above, so every NDEBUG build (win-release/win-optimized/win-profile) and every Linux
    // build fell back to spdlog's implicit default logger: no log file, no MSVC sink, and none of
    // this project's sink configuration. Because output still reached stdout, the app looked fine
    // and the loss was silent. Diagnostics matter most in exactly those builds (#915): on Windows
    // TaskSmack is a GUI-subsystem binary with no console of its own, so without a file sink the
    // only record of a run is a stdout stream the launcher had to redirect in advance -- a
    // TASKSMACK_TRACE_RESIZE_PERF capture started from Explorer discarded all of its evidence.
    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(3);
#ifdef _WIN32
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>()); // LCOV_EXCL_LINE - TaskSmack.exe only (#2039)
#endif
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    // tasksmack-debug.log: the temp folder's, or, where the temp folder is the less-privileged side's, a folder only
    // administrators may change (App/DebugLogFile.h): elevated or handed off on Windows, %ProgramData%\TaskSmack\Logs
    // (#4105); any root session on Linux, /run/tasksmack (#4150): sudo -E, su -m and doas keep the user's TMPDIR, and
    // root never needs its log in a temp folder it didn't set, so the rule there is the effective uid, not the invoking
    // user (the #4165 review). Windows' is the rule the DLL search order and SDL's variables follow (#3897, #4103).
#ifdef _WIN32
    const bool adminOnlyLog = lessPrivilegedEnvironment();
#elif defined(__linux__)
    const bool adminOnlyLog = ::geteuid() == 0;
#else
    constexpr bool adminOnlyLog = false;
#endif
    const auto logFile = App::DebugLogFile::open(adminOnlyLog);
    if (logFile.has_value())
    {
        sinks.push_back(logFile->sink);
    }
    else
    {
        // Best-effort: keep the console (and MSVC) sinks, and say why below once they are the logger's.
        std::println(stderr, "Failed to initialize file logging: {}", logFile.error());
    }

    spdlog::set_default_logger(std::make_shared<spdlog::logger>("TaskSmack", sinks.begin(), sinks.end()));

    if (logFile.has_value())
    {
        // pathToUtf8(), not string(): std::filesystem::path::string() performs a narrowing
        // conversion that can throw for a path the active code page cannot represent.
        spdlog::info("Debug log file: {}", Core::pathToUtf8(logFile->path));
    }
    else if (adminOnlyLog)
    {
        spdlog::warn("No debug log file: TaskSmack runs as administrator (elevated, or as root), and {} in its "
                     "administrators-only folder couldn't be opened safely: {}. This log goes to the debugger and the console only",
#ifdef _WIN32
                     App::DebugLogFile::FILE_NAME,
#else
                     // The file it tried: root's per-user name in /run/tasksmack (#4261, #4654).
                     App::DebugLogFile::rootLogName(App::DebugLogFile::systemInvokingUid()),
#endif
                     logFile.error());
    }

    // Logged here rather than when it is read (UserConfig::get() above), so it reaches the log file.
    if (const auto& configDir = Core::ConfigDirOverride::active(); configDir.has_value())
    {
        if (Core::ConfigDirOverride::fromTestHook())
        {
            spdlog::info("Config directory (test hook {}): {}", Core::ConfigDirOverride::ENV_VAR, Core::pathToUtf8(*configDir));
        }
#ifdef _WIN32
        else if (ownDirHeld)
        {
            spdlog::info("Config directory (this account's own, checked and held as a handed-over one is, #4106): {}",
                         Core::pathToUtf8(*configDir));
        }
        else if (!ownDirRefused.empty())
        {
            spdlog::warn("Not using this account's settings folder {} ({}): TaskSmack runs as administrator, and writes there only "
                         "through a folder it has checked and holds. This run starts from the default settings and saves none",
                         Core::pathToUtf8(*configDir),
                         ownDirRefused);
        }
#endif
#ifdef __linux__
        else if (heldUserFolder != nullptr) // LCOV_EXCL_START - root for a user only
        {
            spdlog::info("Config directory (the user TaskSmack runs as root for, checked and held, #4167): {}",
                         Core::pathToUtf8(*configDir));
        }
        else if (!heldUserFolderRefused.empty())
        {
            spdlog::warn("Not using {} ({}): TaskSmack runs as root for its user, and reads and writes there only through a folder "
                         "it has checked and holds. This run starts from the default settings and saves none",
                         Core::pathToUtf8(*configDir),
                         heldUserFolderRefused);
        } // LCOV_EXCL_STOP
#endif
        else
        {
            spdlog::info("Config directory (the launching user's, handed over): {}", Core::pathToUtf8(*configDir));
        }
    }
#ifdef __linux__
    // Removed in main(), before any SDL call (#4158, #4159, #4179). Names only.
    if (const auto& dropped = Platform::RootEnvironment::removedFromThisProcess(); !dropped.empty())
    {
        std::string names;
        for (const std::string& name : dropped)
        {
            names += names.empty() ? name : ", " + name;
        }
        spdlog::warn("Kept only an allow-list of environment variables, and root's PATH and HOME: TaskSmack runs as root "
                     "with an environment that may be another user's. Dropped: {}",
                     names);
    }
#endif
#ifdef __linux__
    // Removed in main(), before any SDL call (#4111). Names only: a value may be a path the user would rather not log.
    if (const auto& sdlRemoved = Platform::SdlEnvironment::removedFromThisProcess(); !sdlRemoved.empty())
    {
        std::string names;
        for (const std::string& name : sdlRemoved)
        {
            names += names.empty() ? name : ", " + name;
        }
        spdlog::warn("Ignoring the SDL and graphics-loader environment variables {}: TaskSmack runs as root for a user, "
                     "and takes no SDL setting or library path from the environment they started it with (#4151)",
                     names);
    }
#endif
    // Names only: a value may be a path the user would rather not log.
    if (Core::writingHooksIgnored())
    {
        std::string ignored;
        for (const Core::EnvHook& hook : Core::KNOWN_ENV_HOOKS)
        {
            if (hook.writesFiles && Core::readEnvUnfiltered(hook.name) != nullptr)
            {
                ignored += ignored.empty() ? std::string(hook.name) : ", " + std::string(hook.name);
            }
        }
        if (!ignored.empty())
        {
            spdlog::warn("{} set but ignored: TaskSmack runs as administrator (elevated, or as root for a user), and writes "
                         "nothing where an environment the less-privileged side may have set points (#4104)",
                         ignored);
        }
    }
#ifdef _WIN32
    // Removed in WinMain, before any SDL call (#4103). Names only: a value may be a path the user would rather not log.
    if (const auto& sdlRemoved = Platform::Windows::SdlEnvironment::removedFromThisProcess(); !sdlRemoved.empty())
    {
        std::string names;
        for (const std::string& name : sdlRemoved)
        {
            names += names.empty() ? name : ", " + name;
        }
        spdlog::warn("Ignoring the SDL environment variables {}: TaskSmack runs as administrator, and takes no SDL setting "
                     "from an environment the less-privileged side may have set",
                     names);
    }
    if (handoffWait.has_value())
    {
        spdlog::info("Restarted as administrator for TaskSmack PID {}, {}, after {:.1f} s; the instance lock after {:.1f} s more",
                     launch.handoffPid.value_or(0),
                     describeHandoffWait(*handoffWait),
                     inSeconds(handoffWaited),
                     inSeconds(lockWaited));
    }
    if (!handoffDirRefused.empty())
    {
        spdlog::warn("Not using the handed-over settings folder ({}); using this account's own", handoffDirRefused);
    }
#else
    if (launch.handoffPid.has_value())
    {
        spdlog::info("Ignoring {}: nothing restarts a Linux session (#3580)", Platform::HANDOFF_ARGUMENT);
    }
#endif

#ifndef NDEBUG
    spdlog::set_level(spdlog::level::debug);
    constexpr spdlog::level::level_enum flushLevel = App::DebugLogFile::DEBUG_FLUSH_LEVEL;
#else
    constexpr spdlog::level::level_enum flushLevel = App::DebugLogFile::RELEASE_FLUSH_LEVEL;
    // In release builds, default to warn to silence info/debug noise while
    // preserving warnings, errors, and critical failures.
    // Override at runtime via TASKSMACK_LOG_LEVEL=trace|debug|info|warn|error|critical|off.
    // TASKSMACK_TRACE_RESIZE_PERF also promotes the level to info automatically.
    {
        spdlog::level::level_enum runtimeLevel = spdlog::level::warn;
        if (const char* levelEnv = Core::readEnv(Core::LOG_LEVEL_ENV_VAR); levelEnv != nullptr)
        {
            runtimeLevel = Core::logLevelFromEnv(levelEnv);
        }
        else if (Core::resizePerfTraceRequested())
        {
            runtimeLevel = spdlog::level::info;
        }
        spdlog::set_level(runtimeLevel);
    }
#endif
    // After the level, which it reads (#4419): the flush level's lines as they're logged, the rest every few seconds on a
    // worker of its own while the level lets any through, and all of it once more when runApp() returns or throws.
    const App::DebugLogFile::FlushPolicy flushPolicy(spdlog::default_logger(), flushLevel);

    spdlog::info("{} v{} ({} build)", tasksmack::Version::PROJECT_NAME, tasksmack::Version::FULL, tasksmack::Version::BUILD_TYPE);
    // Set first thing in runApp(), before logging was up, so logged here.
    spdlog::info("{}", Core::LocaleSetup::describe(localeSummary));
    for (const std::string& warning : localeSummary.warnings)
    {
        spdlog::warn("Locale: {}", warning);
    }
    // The command line (#3093 part 2): --minimized from the sign-in entry; anything else is ignored.
    if (launch.minimized)
    {
        spdlog::info("Starting minimized (--minimized)");
    }
    for (const std::string& ignored : launch.ignored)
    {
        spdlog::info("Ignoring the command-line argument {}", ignored);
    }
    if (launch.asTaskManager)
    {
        spdlog::info("Started in place of Task Manager (#3072); Task Manager's own arguments are not TaskSmack's");
    }
    spdlog::debug("Compiler: {} {}", tasksmack::Version::COMPILER_ID, tasksmack::Version::COMPILER_VERSION);
    spdlog::debug("Built: {} {}", tasksmack::Version::BUILD_DATE, tasksmack::Version::BUILD_TIME);

#ifdef __linux__
    if (capabilityDrop.dropped)
    {
        spdlog::info("Administrator session: kept {} of root's capabilities (#3621): {}",
                     Domain::ProcessSecurity::capabilityNames(capabilityDrop.after.effective).size(),
                     Domain::ProcessSecurity::capabilitySetText(capabilityDrop.after.effective));
        if (capabilityDrop.threads > 1)
        {
            // capset() narrows only the calling thread (#3765): the others already running keep root's full sets.
            spdlog::warn("Administrator session: {} threads were running when root's capabilities were narrowed; only this "
                         "thread was narrowed, and the other {} keep root's full sets",
                         capabilityDrop.threads,
                         capabilityDrop.threads - 1);
        }
    }
    else if (capabilityDrop.root)
    {
        spdlog::warn("Administrator session: root's capabilities couldn't be narrowed (#3621): {}",
                     std::error_code(capabilityDrop.error, std::generic_category()).message());
    }
#endif

#ifdef _WIN32
    // Elevated, enable SeDebugPrivilege (#2128) before any probe or action opens a process, so
    // SYSTEM-service processes can be killed, reprioritized, dumped and read. It logs its own outcome,
    // changes nothing when not elevated, and a failure doesn't stop startup. Windows only: Linux has
    // no counterpart.
    Platform::enableDebugPrivilege(); // LCOV_EXCL_LINE - TaskSmack.exe only (#2039)
#endif

    // Read TASKSMACK_SYNTHETIC once, now, so the synthetic scenario (#1413) is logged at startup and
    // every panel sees the same one. Unset, this is the only trace of it.
    static_cast<void>(App::Synthetic::activeScenario());

    // TASKSMACK_WINDOW (#1453), likewise read and logged once: a fixed geometry for measurement runs
    // that replaces the saved one here and is not saved on exit (ShellLayer::onDetach).
    const std::optional<App::WindowOverride::Geometry>& windowOverride = App::WindowOverride::active();

    // TASKSMACK_SCREENSHOT (#2827), likewise read and logged once: a PNG of the window (or a sequence)
    // once startup has settled, then the normal close. Unset, nothing below is pushed for it.
    const std::optional<App::ScreenshotOverride::Request>& screenshot = App::ScreenshotOverride::active();

    // TASKSMACK_DEMO (#2827), likewise read and logged once: timed UI requests from the moment startup
    // has settled, for the screenshot to record. Unset, nothing below is pushed for it.
    const std::optional<std::vector<App::DemoOverride::Step>>& demo = App::DemoOverride::active();

    // TASKSMACK_RENDER_METRICS (#2923), likewise read and logged once: the Render Metrics overlay from the
    // first frame (ShellLayer::onAttach) and its CSV to this file at exit (ShellLayer::onDetach). It is
    // also where the demo script's metrics=export writes; with it unset, that step writes beside the
    // screenshot's path, and with neither set it is skipped with a warning.
    const std::optional<std::filesystem::path>& renderMetricsCsv = App::RenderMetricsOverride::active();
    std::optional<std::filesystem::path> demoMetricsCsv = renderMetricsCsv;
    if (!demoMetricsCsv.has_value() && screenshot.has_value())
    {
        demoMetricsCsv = App::RenderMetricsOverride::csvBesideScreenshot(screenshot->path);
    }

    if (instanceLock->status() == App::InstanceLock::Status::Unavailable)
    {
        spdlog::warn(
            "Can't take the single-instance lock {}: {}; starting anyway", Core::pathToUtf8(instanceLockPath), instanceLock->error());
    }
    else if (!instanceLock->error().empty())
    {
        spdlog::warn("Single-instance lock {}: {}", Core::pathToUtf8(instanceLockPath), instanceLock->error()); // #2437
    }

    userConfig.load();
    const auto& settings = userConfig.settings();

    // Create application and transfer ownership to the singleton
    Core::ApplicationSpecification appSpec;
    appSpec.Name = "TaskSmack";
    appSpec.Width =
        std::clamp(windowOverride ? windowOverride->width : settings.windowWidth, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    appSpec.Height =
        std::clamp(windowOverride ? windowOverride->height : settings.windowHeight, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    appSpec.VSync = true;
    appSpec.ForceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;
#ifdef _WIN32
    // --minimized (#3093 part 2): created hidden, placed, then shown minimized without the focus (below).
    appSpec.StartHidden = launch.minimized;
#endif

    auto app = std::make_unique<Core::Application>(appSpec);
    Core::Application::setInstance(std::move(app));

    // Get reference to the application for further setup
    Core::Application& appRef = Core::Application::get();

    // From here the Application owns the layers. However the rest ends, detach them while the
    // Application singleton is still reachable, then destroy it, both before the config and theme
    // singletons are torn down at static destruction (#1124).
    const auto tearDownApplication = [&appRef]
    {
        // CRITICAL: Manually detach all layers BEFORE clearing the Application singleton.
        // Reason: Layer onDetach() methods may call Application::get() to save state.
        // If we let ~Application() run during setInstance(nullptr), those calls will fail.
        App::RestartAsAdministrator::setOffered({}); // its press raises through the Application, going now
        appRef.detachAllLayers();
        // Explicitly destroy the Application singleton to ensure SDL_Quit()
        // and other teardown happen before main()/WinMain() returns.
        Core::Application::setInstance(nullptr);
    };

    int exitCode = EXIT_SUCCESS;
    try
    {

        // Apply the saved normal geometry and maximized state now that the window exists. The size
        // and position are checked against the connected displays, so a position saved on a monitor
        // that is gone no longer opens the borderless window off-screen (#1128), and the normal
        // rectangle is applied before maximizing so it is what Restore returns to (#1121). The size is
        // converted from the scale it was saved at to the scale of the display it opens on (#1168).
        // A TASKSMACK_WINDOW override replaces all of it: its size (already given to the window
        // above) is kept in window units with no scale conversion, only fitted to the display, at
        // the position the window was created at, and maximized only if it says so.
        if (windowOverride)
        {
            appRef.getWindow().applySavedGeometry(std::nullopt, windowOverride->maximized, std::nullopt);
        }
        else
        {
            std::optional<std::pair<int, int>> savedPosition;
            if (settings.windowPosX.has_value() && settings.windowPosY.has_value())
            {
                savedPosition = std::pair{*settings.windowPosX, *settings.windowPosY};
            }
            appRef.getWindow().applySavedGeometry(savedPosition, settings.windowMaximized, settings.windowScale);
        }

        // Push UI layer (initializes ImGui/ImPlot backends). Must be pushed (and therefore
        // onRender()'d) before ShellLayer: UILayer::onRender() calls ImGui::NewFrame(), which is
        // what actually advances ImGui::GetFrameCount() -- ShellLayer::onRender() reads that count
        // to publish RenderMetrics' per-frame totals (see #875) and needs the *current* frame's
        // value, not the previous one.
        // The saved Font size and Interface scale go to Theme first (#3876), so UILayer::onAttach() builds the fonts and
        // style at them once, instead of at Theme's defaults and again on the first frame.
        userConfig.applyFontSettings();
        appRef.pushLayer<UI::UILayer>();

        // Push title bar layer (custom window chrome) -- skipped when native OS decorations are in
        // use instead (opt-in, native Wayland only; see #745), since the OS/compositor already draws
        // a title bar in that case and ShellLayer reserves no space for a second one.
        App::TitleBarLayer* titleBar = nullptr;
        if (appRef.getWindow().isBorderless())
        {
            titleBar = &appRef.pushLayer<App::TitleBarLayer>();
        }

        // Push shell layer (docking workspace with panels). It hands the title bar the width its
        // panels need, which the title bar folds into the window's minimum size (#1207).
        auto& shell = appRef.pushLayer<App::ShellLayer>();
        shell.setTitleBar(titleBar);

        // The Help window (non-modal, #172) and the dialog layers (modal overlays), opened by
        // OpenHelpEvent, OpenAboutEvent, OpenSettingsEvent and OpenElevationNoticeEvent. The
        // elevation notice is shown at startup when running without elevated privileges.
        appRef.pushLayer<App::HelpLayer>();
        appRef.pushLayer<App::AboutLayer>();
        // Replace Task Manager (#3072): one controller for the Settings rows and the startup check, which
        // reads the registry on its worker and offers a repair when Task Manager starts another TaskSmack.
        const auto taskManager = App::makeTaskManagerReplacementController(App::Synthetic::activeScenario());
        // Whether the user turned it on, remembered in config.toml so the startup check can offer it back
        // after something turned it off (#3276). Saved on the config worker, never on the UI thread (#2916).
        taskManager->rememberIntent(userConfig.settings().replaceTaskManager,
                                    [](bool intended)
                                    {
                                        auto& config = App::UserConfig::get();
                                        config.settings().replaceTaskManager = intended;
                                        config.saveInBackground();
                                    });
        appRef.pushLayer<App::SettingsLayer>().setTaskManagerReplacement(taskManager);
        // The startup notice, Standard or Administrator (#2728), with Windows' Restart as administrator.
        auto elevatedRestart = App::makeElevatedRestart(App::Synthetic::activeScenario());
        // Where there's no helper to do a refused action or read as an administrator (the .zip, a copy the user can
        // change), the refusal offers Restart as administrator..., which opens this notice (#3658).
        if (elevatedRestart != nullptr && App::privilegedBroker(App::Synthetic::activeScenario()) == nullptr)
        {
            App::RestartAsAdministrator::setOffered([](Core::Event& event) { Core::Application::get().raiseEvent(event); });
        }
        appRef.pushLayer<App::ElevationNoticeLayer>(std::move(elevatedRestart));
        appRef.pushLayer<App::TaskManagerNoticeLayer>(taskManager);

        // The demo script's layer (#2827), after the layers its steps make requests of: a step is issued
        // in its onUpdate(), before any layer renders, so the shell draws it on that same frame.
        if (demo.has_value())
        {
            appRef.pushLayer<App::DemoLayer>(*demo, shell, demoMetricsCsv);
        }

        // The screenshot hook's layer (#2827), last, so it sees each frame once every layer above has
        // drawn into it; it waits for the shell's startup selections before capturing.
        const App::ScreenshotLayer* screenshotLayer = nullptr;
        if (screenshot.has_value())
        {
            screenshotLayer = &appRef.pushLayer<App::ScreenshotLayer>(*screenshot, &shell);
        }

        // --minimized (#3093 part 2), the sign-in entry's start: shown minimized, without taking the focus,
        // once the window is placed and every layer is attached. It draws no frames until it is restored
        // (the frame loop's hidden path, #3156).
        if (launch.minimized)
        {
            appRef.getWindow().showMinimizedWithoutFocus();
        }

        // Run the application
        appRef.run();

        // A run that was asked for a screenshot and wrote none failed, whatever else it did: the
        // script that asked sees it in the exit code.
        if (screenshotLayer != nullptr && screenshotLayer->outcome() != App::ScreenshotLayer::Outcome::Written)
        {
            exitCode = EXIT_FAILURE;
        }
    }
    catch (...)
    {
        try
        {
            tearDownApplication();
        }
        catch (...) // NOLINT(bugprone-empty-catch) - already failing; report the original error
        {}
        throw;
    }

    tearDownApplication();
    return exitCode;
}

/// Runs the app, turning an exception that escapes it into a logged error and a non-zero exit
/// instead of std::terminate (#1124). On Windows there is no console, so it is also shown.
auto runAppGuarded(const App::LaunchOptions& launch) -> int
{
    try
    {
        return runApp(launch);
    }
    catch (const std::exception& e)
    {
        try
        {
            spdlog::critical("TaskSmack stopped on an unexpected error: {}", e.what());
#ifdef _WIN32
            // LCOV_EXCL_START - main.cpp runs only in TaskSmack.exe; no test binary links it (#2039)
            const std::string message = std::string("TaskSmack stopped on an unexpected error:\n\n") + e.what();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TaskSmack", message.c_str(), nullptr);
            // LCOV_EXCL_STOP
#endif
        }
        catch (...) // NOLINT(bugprone-empty-catch) - reporting is best effort; the exit code still says it failed
        {}
        return EXIT_FAILURE;
    }
    catch (...)
    {
        // Something not derived from std::exception: same guarded exit, with a generic message.
        try
        {
            spdlog::critical("TaskSmack stopped on an unexpected error of an unknown type");
#ifdef _WIN32
            // LCOV_EXCL_START - main.cpp runs only in TaskSmack.exe; no test binary links it (#2039)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TaskSmack", "TaskSmack stopped on an unexpected error.", nullptr);
            // LCOV_EXCL_STOP
#endif
        }
        catch (...) // NOLINT(bugprone-empty-catch) - reporting is best effort; the exit code still says it failed
        {}
        return EXIT_FAILURE;
    }
}

#ifdef _WIN32
// LCOV_EXCL_START - main.cpp runs only in TaskSmack.exe; no test binary links it (#2039)
/// The command line's arguments, the program's path first: WinMain's own is the ANSI code page's, which can't hold
/// every path.
struct CommandLineArguments
{
    std::vector<std::string> utf8;             ///< As UTF-8, for the options (a lone surrogate shown as U+FFFD)
    std::vector<std::filesystem::path> native; ///< As Windows gave them, for a path (--config-dir, #4310)
};

auto commandLineArguments() -> CommandLineArguments
{
    CommandLineArguments arguments;
    int count = 0;
    // LocalFree'd however this returns, a copy that runs out of memory included (#4367).
    const Platform::Windows::UniqueLocal<LPWSTR> argv(CommandLineToArgvW(GetCommandLineW(), &count));
    if (argv == nullptr)
    {
        return arguments;
    }
    for (const wchar_t* const argument : std::span<LPWSTR>(argv.get(), static_cast<std::size_t>(std::max(count, 0))))
    {
        arguments.utf8.push_back(Platform::WinString::wideToUtf8(argument));
        arguments.native.emplace_back(std::wstring(argument));
    }
    return arguments;
}
// LCOV_EXCL_STOP
#endif

} // namespace

// Entry points
#ifdef _WIN32
// LCOV_EXCL_START - main.cpp runs only in TaskSmack.exe; no test binary links it (#2039)
// Windows GUI application entry point
auto WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/) -> int
{
    // First, before anything loads a DLL by name (#3897). The imports already come from System32 only
    // (/DEPENDENTLOADFLAG, cmake/CompilerOptions.cmake); this covers what SDL, the GL loaders and the probes load
    // later, and what those DLLs import, and drops the current directory and PATH from every search.
    // Elevated through UAC (a full token) or started by Restart as administrator (--handoff, whatever account
    // approved it), System32 only, as the helper (HelperMain.cpp): the folder TaskSmack runs from may be one its
    // less-privileged user can write, and a DLL planted there (dwmapi.dll, which SDL loads by name) would run with
    // the administrator's token. Otherwise its own folder as well: it is where a software OpenGL is found on a PC
    // with no GPU driver (Mesa's opengl32.dll, which CI's display tests and the release smoke check put beside
    // TaskSmack.exe, on runners whose tests run elevated with a Default token). DllSearchPolicy.h has each case.
    SetDefaultDllDirectories(Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess()
                                 ? LOAD_LIBRARY_SEARCH_SYSTEM32
                                 : LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Next, under the same rule and before any SDL call (#4103): SDL takes a hint it isn't given from the variable of
    // the same name, and SDL_OPENGL_LIBRARY, SDL3_DYNAMIC_API and the like name a DLL it loads by full path, which no
    // search order restricts. Elevated, the environment may be the less-privileged side's (HKCU\Environment), so
    // every SDL variable goes. Logged once logging is up (runApp()). SdlEnvironment.h has why all of them.
    Platform::Windows::SdlEnvironment::removeForThisProcess(Platform::Windows::DllSearchPolicy::system32OnlyForThisProcess());
    // Fail closed (#4114): a variable still set (a removal the OS refused) would reach SDL, so don't start. Logging isn't
    // up, so say so in a message box (no SDL involved; user32 is imported, and SetDefaultDllDirectories has run) and, with
    // the names only, to the debugger. HKCU\Environment can't cause it.
    if (const auto& stillSet = Platform::Windows::SdlEnvironment::stillSetInThisProcess(); !stillSet.empty())
    {
        std::wstring names;
        for (const std::string& name : Platform::Windows::SdlEnvironment::loggedNames(stillSet))
        {
            names += (names.empty() ? L"" : L", ") + Platform::WinString::utf8ToWide(name);
        }
        OutputDebugStringW((L"TaskSmack: SDL environment variables still set, not starting: " + names + L"\n").c_str());
        MessageBoxW(nullptr,
                    L"TaskSmack will not start: it couldn't clear the SDL settings it inherited (or couldn't read its environment).",
                    L"TaskSmack",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return EXIT_FAILURE;
    }
    const CommandLineArguments arguments = commandLineArguments();
    return runAppGuarded(App::parseLaunchOptions(arguments.utf8, arguments.native));
}
// LCOV_EXCL_STOP
#else
// Standard entry point for Linux/macOS
// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog initialization may theoretically throw; acceptable at program start
auto main(int argc, char** argv) -> int
{
#ifdef __linux__
    // First, before any SDL call (#4111): run as root for a user (sudo -E, an env_keep list), the environment is the
    // user's, and SDL_OPENGL_LIBRARY, SDL3_DYNAMIC_API and the like name a library SDL would dlopen() as root, so every
    // SDL variable goes, and the graphics loaders' (LIBGL_DRIVERS_PATH, __EGL_VENDOR_LIBRARY_FILENAMES, ..., #4151). The
    // rule #4090 uses. Logged once logging is up (runApp()). SdlEnvironment.h has why all of them.
    Platform::SdlEnvironment::removeForThisProcess(lessPrivilegedEnvironment()); // #4154: no lookup needed
    // Root with an environment that may be a user's keeps only an allow-list (#4158, #4159, #4179): GTK's modules,
    // PATH, PYTHONPATH and whatever else a list of bad names would miss. After the SDL scrub, which a setcap session
    // (not root) still needs on its own. Logged in runApp().
    Platform::RootEnvironment::applyToThisProcess(::geteuid() == 0 && lessPrivilegedEnvironment());
    // Last, so neither pass above drops it (the #4284 review): no libdecor, so no GTK in the process, and no Mesa shader
    // cache (#4254, #4285). GTK loads the modules the user's own settings.ini names, and llvmpipe runs the user's cached
    // machine code; a setcap session would run either with its capabilities.
    Platform::SdlEnvironment::keepPrivilegedLoadersOutOfThisProcess(lessPrivilegedEnvironment());
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - argv is argc C strings, as the C++ standard gives them
    const std::vector<std::string> arguments(argv, argv + std::max(argc, 0));
    return runAppGuarded(App::parseLaunchOptions(arguments));
}
#endif
