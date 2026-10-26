#include "HelperLauncher.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsHandles.h"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <objbase.h>

namespace Platform::Windows::HelperLauncher
{

namespace
{

/// @p parameters split as the helper's CommandLineToArgvW will split its command line, after its path.
[[nodiscard]] std::optional<std::vector<std::wstring>> splitAsTheHelperWill(const std::wstring& parameters)
{
    const std::wstring commandLine = std::format(L"\"{}\" {}", HELPER_FILE_NAME, parameters);
    int count = 0;
    struct Free
    {
        void operator()(void* memory) const noexcept
        {
            LocalFree(memory);
        }
    };
    const std::unique_ptr<LPWSTR, Free> argv(CommandLineToArgvW(commandLine.c_str(), &count));
    if (!argv)
    {
        return std::nullopt;
    }
    const std::span<LPWSTR> all(argv.get(), static_cast<std::size_t>(count));
    std::vector<std::wstring> split;
    if (!all.empty())
    {
        split.assign(all.begin() + 1, all.end());
    }
    return split;
}

/// Whether @p response is a read the helper refused because it runs as another account than
/// TaskSmack's user (BrokerHelper::readsOnlyOwnProcesses()): a process read, or a system-wide one
/// answered with an ErrorReply, that carries BrokerHelper::OTHER_USERS_READ_REFUSED. Its words are then
/// replaced by @p explanation. An environment or memory rollup refused the same way carries no words,
/// so it teaches nothing: both are refused the same way for other reasons too.
[[nodiscard]] bool refusedForAnotherAccount(Broker::Response& response, std::string_view explanation)
{
    const auto reworded = [explanation](std::string& words)
    {
        if (words != BrokerHelper::OTHER_USERS_READ_REFUSED)
        {
            return false;
        }
        words = explanation;
        return true;
    };
    if (auto* const modules = std::get_if<Broker::ModulesReply>(&response))
    {
        return reworded(modules->result.detail);
    }
    if (auto* const files = std::get_if<Broker::OpenFilesReply>(&response))
    {
        return reworded(files->result.detail);
    }
    if (auto* const connections = std::get_if<Broker::ConnectionsReply>(&response))
    {
        return reworded(connections->result.detail);
    }
    if (auto* const error = std::get_if<Broker::ErrorReply>(&response))
    {
        return reworded(error->message); // a system-wide read: the firmware, every user's crashes
    }
    return false;
}

} // namespace

BOOL WINAPI shellExecuteWithCom(SHELLEXECUTEINFOW* info)
{
    // ShellExecuteExW may hand the work to shell extensions, which need COM on the calling thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const BOOL started = ShellExecuteExW(info);
    const DWORD error = GetLastError();
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    SetLastError(error);
    return started;
}

std::expected<std::wstring, std::string> parameters(const BrokerHelper::Arguments& arguments)
{
    std::wstring line = BrokerHelper::commandLine(arguments);
    // What the helper will read: nothing that quoting or spaces could split differently.
    const auto split = splitAsTheHelperWill(line);
    if (!split.has_value())
    {
        return std::unexpected(std::string("the helper's command line couldn't be split"));
    }
    const std::vector<std::wstring_view> views(split->begin(), split->end());
    const auto parsed = BrokerHelper::parseArguments(views);
    if (!parsed.has_value() || parsed->pipeName != arguments.pipeName || parsed->parentPid != arguments.parentPid ||
        parsed->parentStartTicks != arguments.parentStartTicks)
    {
        return std::unexpected(std::string("the helper wouldn't read its command line as meant"));
    }
    return line;
}

std::string launchError(DWORD error)
{
    switch (error)
    {
    case ERROR_CANCELLED:
        return std::string(PROMPT_DISMISSED);
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return std::string(HELPER_MISSING);
    default:
        break;
    }
    return std::format("couldn't start TaskSmackHelper.exe: {}", std::system_category().message(static_cast<int>(error)));
}

std::expected<UniqueHandle, std::string>
launch(const std::wstring& pipeName, const std::stop_token& stopToken, const Functions& functions, HWND owner)
{
    if (stopToken.stop_requested())
    {
        return std::unexpected(std::string("cancelled"));
    }
    const auto image = BrokerHelper::ownImage(functions.getModuleFileName);
    if (!image.has_value())
    {
        return std::unexpected(
            std::format("couldn't tell where TaskSmack is: {}", std::system_category().message(static_cast<int>(GetLastError()))));
    }
    const std::wstring helper = BrokerHelper::besidePath(*image, HELPER_FILE_NAME);
    if (helper.empty())
    {
        return std::unexpected(std::string("couldn't tell where TaskSmack is"));
    }
    // What the helper checks TaskSmack's against, read as the helper reads it (#2772).
    const auto ticks = BrokerHelper::startTicksOf(GetCurrentProcess());
    if (!ticks.has_value())
    {
        return std::unexpected(std::string("couldn't read TaskSmack's start time"));
    }
    const auto line = parameters({.pipeName = pipeName, .parentPid = GetCurrentProcessId(), .parentStartTicks = *ticks});
    if (!line.has_value())
    {
        return std::unexpected(line.error());
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // The process back, ShellExecuteExW's work done before it returns (this thread may end next), and
    // no error dialog of the shell's: the error is TaskSmack's to show.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner; // the prompt in front of TaskSmack (#2550)
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = line->c_str();
    info.nShow = SW_HIDE;
    if (functions.shellExecuteEx(&info) == FALSE)
    {
        return std::unexpected(launchError(GetLastError()));
    }
    UniqueHandle process(info.hProcess);
    if (!process)
    {
        return std::unexpected(std::string("the helper started without a process to check"));
    }
    return process;
}

PipeBroker::Launcher makeLauncher(Functions functions, HWND owner)
{
    return [functions, owner](const std::wstring& pipeName, const std::stop_token& stopToken)
    {
        return launch(pipeName, stopToken, functions, owner);
    };
}

std::expected<void, std::string> availability(bool elevated, const std::wstring& ownImage, BrokerHelper::Self::InstallCheck installCheck)
{
    if (elevated)
    {
        return std::unexpected(std::string("TaskSmack already runs as an administrator"));
    }
    const std::wstring helper = BrokerHelper::besidePath(ownImage, HELPER_FILE_NAME);
    std::error_code error;
    if (helper.empty() || !std::filesystem::is_regular_file(helper, error))
    {
        return std::unexpected(std::string(HELPER_MISSING));
    }
    // What the helper will check before it serves (#2572): ask first, rather than raise a UAC prompt
    // for a helper that then refuses TaskSmack.
    const std::array<std::wstring, 2> install{ownImage, helper};
    if (installCheck == nullptr)
    {
        return std::unexpected(std::string("no install check"));
    }
    if (const auto unchangeable = installCheck(GetCurrentProcess(), install); !unchangeable.has_value())
    {
        return std::unexpected(std::format("TaskSmack's folder isn't one only an administrator can change, so Windows can't run its "
                                           "administrator helper for it: {}",
                                           unchangeable.error()));
    }
    return {};
}

bool readsOtherUsers(TOKEN_ELEVATION_TYPE elevationType) noexcept
{
    return elevationType == TokenElevationTypeLimited;
}

bool readsOtherUsers() noexcept
{
    UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return false;
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    if (GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size) == FALSE)
    {
        return false;
    }
    return readsOtherUsers(type);
}

bool administratorProtection() noexcept
{
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"TypeOfAdminApprovalMode",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &mode,
                     &size) != ERROR_SUCCESS)
    {
        return false;
    }
    return mode == ADMINISTRATOR_PROTECTION_MODE;
}

LearnedReadsBroker::LearnedReadsBroker(std::unique_ptr<IPrivilegedBroker> inner, RefusalLasts lasts)
    : m_Inner(std::move(inner)), m_Lasts(lasts)
{
    if (m_Inner == nullptr)
    {
        throw std::invalid_argument("LearnedReadsBroker requires a broker");
    }
    m_ExpectedReadsOtherUsers = m_Inner->readsOtherUsers();
}

bool LearnedReadsBroker::connected() const noexcept
{
    return m_Inner->connected();
}

std::expected<void, std::string> LearnedReadsBroker::connect(const std::stop_token& stopToken)
{
    return m_Inner->connect(stopToken);
}

bool LearnedReadsBroker::readsOtherUsers() const noexcept
{
    // Acquire, before the count is loaded: the count is then never older than the one the refusal
    // was taken at, so the session that refused never reads as one that came after it.
    const std::uint64_t refusedIn = m_RefusedIn.load(std::memory_order_acquire);
    if (refusedIn == NOT_REFUSED)
    {
        return m_ExpectedReadsOtherUsers;
    }
    if (m_Lasts == RefusalLasts::ForTheProcess)
    {
        return false;
    }
    // The helper that refused, still running or gone with none started since: still no. A newer
    // helper's prompt was answered afresh, so it is the token's again until that one refuses too.
    return m_ExpectedReadsOtherUsers && m_Inner->sessionsStarted() != refusedIn;
}

std::uint64_t LearnedReadsBroker::sessionsStarted() const noexcept
{
    return m_Inner->sessionsStarted();
}

std::expected<Broker::Response, std::string> LearnedReadsBroker::call(const Broker::Request& request, const std::stop_token& stopToken)
{
    auto response = m_Inner->call(request, stopToken);
    learnFrom(response);
    return response;
}

std::expected<Broker::Response, std::string> LearnedReadsBroker::callIfIdle(const Broker::Request& request,
                                                                            std::chrono::milliseconds timeout)
{
    auto response = m_Inner->callIfIdle(request, timeout);
    learnFrom(response);
    return response;
}

void LearnedReadsBroker::learnFrom(std::expected<Broker::Response, std::string>& response)
{
    const std::string_view explanation =
        m_Lasts == RefusalLasts::ForTheProcess ? ADMINISTRATOR_PROTECTION_READS_UNAVAILABLE : OTHER_USERS_READS_UNAVAILABLE;
    if (!response.has_value() || !refusedForAnotherAccount(*response, explanation))
    {
        return;
    }
    // Counted after the reply: the session that answered, or a newer one, never an older one, since a
    // session is counted before any call can reach it (PipeBroker::connectWithin()).
    const std::uint64_t session = m_Inner->sessionsStarted();
    std::uint64_t seen = m_RefusedIn.load(std::memory_order_relaxed);
    // The newest session's refusal stands: calls answered by two helpers may come back in either order.
    while (seen == NOT_REFUSED || seen < session)
    {
        if (m_RefusedIn.compare_exchange_weak(seen, session, std::memory_order_release, std::memory_order_relaxed))
        {
            if (m_Lasts == RefusalLasts::ForTheProcess)
            {
                spdlog::info("Under Administrator Protection the administrator helper runs as another account than TaskSmack's "
                             "user, so it reads only the user's own processes: \"Read as administrator\" isn't offered again "
                             "while TaskSmack runs (#2769, #2993)");
            }
            else
            {
                spdlog::info("The administrator helper runs as another account than TaskSmack's user, so it reads only the user's "
                             "own processes: \"Read as administrator\" isn't offered again until a new helper starts (#2769, "
                             "#2993)");
            }
            return;
        }
    }
}

std::expected<void, std::string> availability()
{
    const auto image = BrokerHelper::ownImage();
    if (!image.has_value())
    {
        return std::unexpected(std::string("couldn't tell where TaskSmack is"));
    }
    return availability(isProcessElevated(), *image, &BrokerHelper::checkUnchangeableByParent);
}

} // namespace Platform::Windows::HelperLauncher
