#include "BrokerPipe.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#include <bcrypt.h>
#include <sddl.h>
// clang-format on

namespace Platform::Windows::BrokerPipe
{

namespace
{

/// How often a wait looks at its stop token, as the Linux socket's poll does.
constexpr std::chrono::milliseconds POLL_STEP{100};
/// How often connectToServer() looks again for a pipe the helper hasn't made yet.
constexpr std::chrono::milliseconds CONNECT_RETRY_STEP{25};
constexpr std::wstring_view PIPE_NAME_PREFIX = L"\\\\.\\pipe\\TaskSmack.Broker.";

/// Frees what the security APIs allocate with LocalAlloc (string SIDs, security descriptors).
struct LocalFreeDeleter
{
    void operator()(void* memory) const noexcept
    {
        LocalFree(memory);
    }
};

[[nodiscard]] std::string lastErrorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// The errors that mean the other end has closed the pipe.
[[nodiscard]] bool isHangUp(DWORD error) noexcept
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

enum class IoEnd : std::uint8_t
{
    Done,
    Closed,
    TimedOut,
    Stopped,
    Failed,
};

struct IoOutcome
{
    IoEnd end = IoEnd::Failed;
    DWORD bytes = 0;
};

[[nodiscard]] UniqueHandle makeEvent() noexcept
{
    // Manual reset: ReadFile, WriteFile and ConnectNamedPipe reset it as they start and set it as they finish.
    return UniqueHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

/// Waits until @p deadline for the I/O pending on @p overlapped to finish, looking at @p stopToken every
/// POLL_STEP. I/O given up on is cancelled and waited out, so @p overlapped and its buffer are free when
/// this returns. I/O that finished before the cancel took is Done, whatever the deadline.
[[nodiscard]] IoOutcome
finishIo(HANDLE pipe, OVERLAPPED& overlapped, std::chrono::steady_clock::time_point deadline, const std::stop_token& stopToken)
{
    IoEnd gaveUp = IoEnd::Done;
    while (true)
    {
        if (stopToken.stop_requested())
        {
            gaveUp = IoEnd::Stopped;
            break;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            gaveUp = IoEnd::TimedOut;
            break;
        }
        const DWORD waited = WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(std::min(left, POLL_STEP).count()));
        if (waited == WAIT_OBJECT_0)
        {
            break;
        }
        if (waited != WAIT_TIMEOUT)
        {
            gaveUp = IoEnd::Failed;
            break;
        }
    }
    if (gaveUp != IoEnd::Done)
    {
        CancelIoEx(pipe, &overlapped);
    }
    DWORD bytes = 0;
    if (GetOverlappedResult(pipe, &overlapped, &bytes, TRUE) != FALSE)
    {
        return {.end = IoEnd::Done, .bytes = bytes};
    }
    const DWORD error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED && gaveUp != IoEnd::Done)
    {
        return {.end = gaveUp, .bytes = bytes};
    }
    return {.end = isHangUp(error) ? IoEnd::Closed : IoEnd::Failed, .bytes = bytes};
}

struct ReadOutcome
{
    IoEnd end = IoEnd::Failed;
    std::size_t got = 0;
};

/// Fills @p into from @p pipe, until @p deadline. Byte mode: a read returns what has arrived, so it
/// takes as many as it needs. got says how much came before an end other than Done.
[[nodiscard]] ReadOutcome
readExactly(HANDLE pipe, std::span<std::uint8_t> into, std::chrono::steady_clock::time_point deadline, const std::stop_token& stopToken)
{
    const UniqueHandle event = makeEvent();
    if (!event)
    {
        return {.end = IoEnd::Failed, .got = 0};
    }
    std::size_t got = 0;
    while (got < into.size())
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        const auto chunk = static_cast<DWORD>(into.size() - got); // a frame (at most MAX_MESSAGE_BYTES) fits a DWORD
        if (ReadFile(pipe, into.subspan(got).data(), chunk, nullptr, &overlapped) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                return {.end = isHangUp(error) ? IoEnd::Closed : IoEnd::Failed, .got = got};
            }
        }
        const IoOutcome outcome = finishIo(pipe, overlapped, deadline, stopToken);
        got += outcome.bytes;
        if (outcome.end != IoEnd::Done)
        {
            return {.end = outcome.end, .got = got};
        }
    }
    return {.end = IoEnd::Done, .got = got};
}

} // namespace

std::expected<std::wstring, std::string> makePipeName()
{
    std::array<std::uint8_t, 16> random{};
    const NTSTATUS status = BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status))
    {
        return std::unexpected(std::format("no random pipe name: BCryptGenRandom failed (0x{:08X})", static_cast<std::uint32_t>(status)));
    }
    std::wstring name(PIPE_NAME_PREFIX);
    for (const std::uint8_t byte : random)
    {
        name += std::format(L"{:02x}", byte);
    }
    return name;
}

std::expected<std::wstring, std::string> userSidOfToken(HANDLE token)
{
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0, &size));
    if (size == 0)
    {
        return std::unexpected(std::format("couldn't read the token's user: {}", lastErrorText(GetLastError())));
    }
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return std::unexpected(std::format("couldn't read the token's user: {}", lastErrorText(GetLastError())));
    }
    const auto* const user = reinterpret_cast<const TOKEN_USER*>(
        buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - TokenUser's buffer is a TOKEN_USER
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &text) == FALSE)
    {
        return std::unexpected(std::format("couldn't format the user's SID: {}", lastErrorText(GetLastError())));
    }
    const std::unique_ptr<wchar_t, LocalFreeDeleter> owned(text);
    return std::wstring(owned.get());
}

std::expected<std::wstring, std::string> userSidOfProcess(HANDLE process)
{
    UniqueHandle token;
    if (OpenProcessToken(process, TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::unexpected(std::format("couldn't open the process's token: {}", lastErrorText(GetLastError())));
    }
    return userSidOfToken(token.get());
}

std::expected<std::wstring, std::string> pipeSecurityDescriptor(std::wstring_view userSid)
{
    constexpr std::wstring_view SID_PREFIX = L"S-1-";
    const bool plainSid =
        userSid.size() > SID_PREFIX.size() && userSid.starts_with(SID_PREFIX) &&
        std::ranges::all_of(userSid.substr(SID_PREFIX.size()), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; });
    if (!plainSid)
    {
        return std::unexpected(std::string("not a SID"));
    }
    // D:P - a protected DACL (nothing inherited), one allow ACE: CLIENT_ACCESS for the user.
    // S:(ML;;NWNRNX;;;ME) - a medium label: no read, write or execute up from below medium.
    return std::format(L"D:P(A;;0x{:08X};;;{})S:(ML;;NWNRNX;;;ME)", CLIENT_ACCESS, userSid);
}

std::expected<UniqueHandle, std::string> createServerPipe(const std::wstring& name, std::wstring_view allowedUserSid)
{
    const auto sddl = pipeSecurityDescriptor(allowedUserSid);
    if (!sddl.has_value())
    {
        return std::unexpected(sddl.error());
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl->c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
    {
        return std::unexpected(std::format("couldn't build the pipe's security: {}", lastErrorText(GetLastError())));
    }
    const std::unique_ptr<void, LocalFreeDeleter> ownedDescriptor(descriptor);
    SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = descriptor, .bInheritHandle = FALSE};
    UniqueHandle pipe(CreateNamedPipeW(name.c_str(),
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1,
                                       PIPE_BUFFER_BYTES,
                                       PIPE_BUFFER_BYTES,
                                       0,
                                       &attributes));
    if (!pipe)
    {
        // ERROR_ACCESS_DENIED here is FILE_FLAG_FIRST_PIPE_INSTANCE's: the name is taken.
        return std::unexpected(std::format("couldn't make the broker's pipe: {}", lastErrorText(GetLastError())));
    }
    return pipe;
}

std::expected<void, std::string> checkClientProcess(HANDLE pipe, DWORD expectedPid)
{
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(pipe, &pid) == FALSE)
    {
        return std::unexpected(std::format("couldn't tell who opened the pipe: {}", lastErrorText(GetLastError())));
    }
    if (pid != expectedPid)
    {
        return std::unexpected(std::format("process {} opened the pipe, not TaskSmack ({})", pid, expectedPid));
    }
    return {};
}

std::expected<void, std::string> checkServerProcess(HANDLE pipe, DWORD expectedPid)
{
    ULONG pid = 0;
    if (GetNamedPipeServerProcessId(pipe, &pid) == FALSE)
    {
        return std::unexpected(std::format("couldn't tell who made the pipe: {}", lastErrorText(GetLastError())));
    }
    if (pid != expectedPid)
    {
        return std::unexpected(std::format("process {} made the pipe, not the helper ({})", pid, expectedPid));
    }
    return {};
}

std::expected<void, std::string>
acceptClient(HANDLE pipe, DWORD expectedClientPid, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const UniqueHandle event = makeEvent();
    if (!event)
    {
        return std::unexpected(std::format("couldn't wait for the client: {}", lastErrorText(GetLastError())));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            switch (finishIo(pipe, overlapped, deadline, stopToken).end)
            {
            case IoEnd::Done:
                break;
            case IoEnd::TimedOut:
                return std::unexpected(std::string("TaskSmack didn't open the pipe in time"));
            case IoEnd::Stopped:
                return std::unexpected(std::string("cancelled"));
            case IoEnd::Closed:
            case IoEnd::Failed:
                return std::unexpected(std::string("waiting for TaskSmack to open the pipe failed"));
            }
        }
        else if (error != ERROR_PIPE_CONNECTED) // one that opened it before ConnectNamedPipe was called
        {
            return std::unexpected(std::format("waiting for TaskSmack to open the pipe failed: {}", lastErrorText(error)));
        }
    }
    if (auto checked = checkClientProcess(pipe, expectedClientPid); !checked.has_value())
    {
        DisconnectNamedPipe(pipe);
        return checked;
    }
    return {};
}

std::expected<UniqueHandle, std::string> connectToServer(const std::wstring& name,
                                                         DWORD expectedServerPid,
                                                         HANDLE serverProcess,
                                                         std::chrono::milliseconds timeout,
                                                         const std::stop_token& stopToken)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        if (stopToken.stop_requested())
        {
            return std::unexpected(std::string("cancelled"));
        }
        // SECURITY_IDENTIFICATION: whoever serves the pipe may learn who TaskSmack is, never act as it.
        UniqueHandle pipe(CreateFileW(name.c_str(),
                                      CLIENT_ACCESS,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                      nullptr));
        if (pipe)
        {
            if (auto checked = checkServerProcess(pipe.get(), expectedServerPid); !checked.has_value())
            {
                return std::unexpected(std::move(checked.error()));
            }
            return pipe;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY)
        {
            // Its one instance is taken: whoever has it isn't this TaskSmack, and the helper will refuse them.
            return std::unexpected(std::string("another process opened the helper's pipe first"));
        }
        if (error != ERROR_FILE_NOT_FOUND)
        {
            return std::unexpected(std::format("couldn't open the helper's pipe: {}", lastErrorText(error)));
        }
        // Not made yet: the helper is still starting.
        if (WaitForSingleObject(serverProcess, 0) == WAIT_OBJECT_0)
        {
            return std::unexpected(std::string("the helper exited before it opened its pipe"));
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            return std::unexpected(std::string("the helper didn't open its pipe in time"));
        }
        std::this_thread::sleep_for(std::min(left, CONNECT_RETRY_STEP));
    }
}

bool sendFrame(HANDLE pipe,
               std::span<const std::uint8_t> frame,
               std::chrono::milliseconds timeout,
               const std::stop_token& stopToken,
               std::string& error)
{
    if (frame.size() > Broker::MAX_MESSAGE_BYTES)
    {
        error = "a frame larger than the protocol allows";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const UniqueHandle event = makeEvent();
    if (!event)
    {
        error = lastErrorText(GetLastError());
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    // One write for the whole frame: MAX_MESSAGE_BYTES fits a DWORD, and a byte-mode pipe takes it as
    // the reader drains the buffer.
    if (WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), nullptr, &overlapped) == FALSE)
    {
        const DWORD failure = GetLastError();
        if (failure != ERROR_IO_PENDING)
        {
            error = lastErrorText(failure);
            return false;
        }
    }
    const IoOutcome outcome = finishIo(pipe, overlapped, deadline, stopToken);
    switch (outcome.end)
    {
    case IoEnd::Done:
        if (outcome.bytes == frame.size())
        {
            return true;
        }
        error = "a short write";
        return false;
    case IoEnd::Closed:
        error = "the other end has gone";
        return false;
    case IoEnd::TimedOut:
        error = "the other end didn't read it in time";
        return false;
    case IoEnd::Stopped:
        error = "cancelled";
        return false;
    case IoEnd::Failed:
        break;
    }
    error = "the write failed";
    return false;
}

std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receiveFrame(HANDLE pipe, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::uint8_t, 4> prefix{};
    const ReadOutcome head = readExactly(pipe, prefix, deadline, stopToken);
    if (head.end != IoEnd::Done)
    {
        if (head.end == IoEnd::Stopped)
        {
            return std::unexpected(ReceiveEnd::Stopped);
        }
        if (head.got != 0)
        {
            return std::unexpected(ReceiveEnd::Failed); // a length cut short
        }
        switch (head.end)
        {
        case IoEnd::Closed:
            return std::unexpected(ReceiveEnd::Closed);
        case IoEnd::TimedOut:
            return std::unexpected(ReceiveEnd::TimedOut);
        default:
            return std::unexpected(ReceiveEnd::Failed);
        }
    }
    const std::size_t length = static_cast<std::size_t>(prefix[0]) | (static_cast<std::size_t>(prefix[1]) << 8U) |
                               (static_cast<std::size_t>(prefix[2]) << 16U) | (static_cast<std::size_t>(prefix[3]) << 24U);
    if (length > Broker::MAX_MESSAGE_BYTES - prefix.size())
    {
        return std::unexpected(ReceiveEnd::Failed); // refused before anything is allocated for it
    }
    std::vector<std::uint8_t> frame(prefix.size() + length);
    std::ranges::copy(prefix, frame.begin());
    const ReadOutcome body = readExactly(pipe, std::span(frame).subspan(prefix.size()), deadline, stopToken);
    if (body.end != IoEnd::Done)
    {
        // Cut short by the timeout or a writer that closed the pipe: the rest would be read as the next
        // frame by a client that keeps the channel after a timeout (#2406), so it is broken either way.
        return std::unexpected(body.end == IoEnd::Stopped ? ReceiveEnd::Stopped : ReceiveEnd::Failed);
    }
    return frame;
}

std::optional<DWORD> bytesWaiting(HANDLE pipe) noexcept
{
    DWORD available = 0;
    if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE)
    {
        return std::nullopt;
    }
    return available;
}

} // namespace Platform::Windows::BrokerPipe
