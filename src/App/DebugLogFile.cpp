#include "App/DebugLogFile.h"

#include "Core/LogFileSink.h"
#ifdef __linux__
#include "Platform/Linux/SessionUser.h"
#endif

#include <spdlog/common.h>

#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath
// clang-format on

#include "Core/Utf8Path.h"
#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/WindowsHandles.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <format>
#include <memory>
#include <mutex>
#include <string_view>
#else
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <cerrno>
#include <cstddef>
#include <memory>
#include <mutex>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace App::DebugLogFile
{
namespace
{

#ifdef _WIN32
namespace WinFile = Platform::Windows;

/// The log written through the file handle checked when it was opened (#4105), never reopened by its path.
class HandleFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit HandleFileSink(WinFile::UniqueHandle file) : m_File(std::move(file))
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        DWORD written = 0;
        static_cast<void>(WriteFile(m_File.get(), formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr));
    }

    void flush_() override
    {}

  private:
    WinFile::UniqueHandle m_File;
};

/// An elevated session's log (#4105), in %ProgramData%\TaskSmack\Logs, as this file's header says.
[[nodiscard]] std::expected<Opened, std::string> openElevated(const Functions& functions)
{
    const std::optional<std::filesystem::path> programData = functions.programDataFolder();
    if (!programData.has_value())
    {
        return std::unexpected(std::string("the ProgramData folder isn't known"));
    }
    const WinFile::OwnedSecurityDescriptor descriptor = WinFile::securityDescriptorFrom(functions.folderSecurity);
    if (descriptor == nullptr)
    {
        return std::unexpected(std::string("the log folder's security can't be built"));
    }
    // %ProgramData% by path, once: a system folder owned by SYSTEM, the trust root. Held without FILE_SHARE_DELETE.
    const auto root = WinFile::openFolderByPath(*programData);
    if (!root.has_value())
    {
        return std::unexpected(std::format("{} can't be opened: {}", Core::pathToUtf8(*programData), root.error()));
    }
    if (WinFile::isPlainFolder(root->get()) != std::optional(true))
    {
        return std::unexpected(std::format("{} isn't a plain folder", Core::pathToUtf8(*programData)));
    }
    auto taskSmack = WinFile::secureChildFolder(
        root->get(), L"TaskSmack", descriptor.get(), Core::pathToUtf8(*programData / "TaskSmack"), functions.ownerTrusted);
    if (!taskSmack.has_value())
    {
        return std::unexpected(std::move(taskSmack.error()));
    }
    const std::filesystem::path logs = elevatedLogFolder(*programData);
    auto logsHandle =
        WinFile::secureChildFolder(taskSmack->get(), L"Logs", descriptor.get(), Core::pathToUtf8(logs), functions.ownerTrusted);
    if (!logsHandle.has_value())
    {
        return std::unexpected(std::move(logsHandle.error()));
    }
    // The log, relative to the held Logs folder. Any existing one is deleted through its own handle, then a fresh one is
    // made with CREATE-only: a planted file with a user's DACL is never written to, and a race that re-makes one is refused.
    const std::wstring name(FILE_NAME.begin(), FILE_NAME.end());
    {
        // DELETE alone, whatever it is (a file, a link): granted through the held Logs folder's FILE_DELETE_CHILD even
        // when the planted file's own DACL grants this process nothing. Closed before the fresh create, so the classic
        // delete-on-close (where POSIX semantics aren't taken) has freed the name.
        auto stale = WinFile::openRelative(
            logsHandle->get(), name, WinFile::RelativeOpen{.access = DELETE, .disposition = FILE_OPEN, .kind = WinFile::EntryKind::Either});
        if (stale.has_value())
        {
            if (!WinFile::deleteThroughHandle(stale->handle.get()))
            {
                return std::unexpected(std::format("the stale {} can't be removed", std::string(FILE_NAME)));
            }
        }
        else if (!WinFile::isNotFound(stale.error()))
        {
            return std::unexpected(std::format("{} can't be checked: {}", std::string(FILE_NAME), WinFile::ntStatusText(stale.error())));
        }
    }
    auto file = WinFile::openRelative(logsHandle->get(),
                                      name,
                                      WinFile::RelativeOpen{
                                          .access = GENERIC_WRITE | SYNCHRONIZE,
                                          .disposition = FILE_CREATE,
                                          .kind = WinFile::EntryKind::File,
                                          .security = descriptor.get(),
                                      });
    if (!file.has_value() || !file->created)
    {
        return std::unexpected(std::format(
            "{} can't be made fresh{}", std::string(FILE_NAME), file.has_value() ? "" : ": " + WinFile::ntStatusText(file.error())));
    }
    // Where it is, as Windows names it: the held folder's final path, which doesn't follow a link, plus the name.
    const std::wstring logsFinal = functions.finalPath(logsHandle->get());
    const std::filesystem::path path = logsFinal.empty() ? logs / FILE_NAME : std::filesystem::path(logsFinal) / FILE_NAME;
    return Opened{.path = path, .sink = std::make_shared<HandleFileSink>(std::move(file->handle))};
}
#endif

#ifndef _WIN32
/// A descriptor closed with its owner.
class OwnedFd
{
  public:
    explicit OwnedFd(int fd) noexcept : m_Fd(fd)
    {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1))
    {}
    OwnedFd& operator=(OwnedFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Fd = std::exchange(other.m_Fd, -1);
        }
        return *this;
    }
    ~OwnedFd()
    {
        reset();
    }
    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_Fd >= 0;
    }

  private:
    void reset() noexcept
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
            m_Fd = -1;
        }
    }
    int m_Fd = -1;
};

[[nodiscard]] std::string errnoText(int error)
{
    return std::system_category().message(error);
}

/// The log written through the descriptor checked when it was opened (#4150), never reopened by its path.
class FdFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit FdFileSink(OwnedFd file) : m_File(std::move(file))
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        const char* data = formatted.data();
        std::size_t left = formatted.size();
        while (left > 0)
        {
            const auto written = ::write(m_File.get(), data, left); // ssize_t
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written <= 0)
            {
                return; // best effort, as spdlog's file sink: a full disk loses lines, never the app
            }
            data += written;
            left -= static_cast<std::size_t>(written);
        }
    }

    void flush_() override
    {}

  private:
    OwnedFd m_File;
};

/// Whether @p info is a plain folder that the process's own user owns and no one else may write.
[[nodiscard]] bool ownedOnlyByUs(const struct stat& info) noexcept
{
    return S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

/// A Linux session run as root for a user (#4150): the log in <parent>/tasksmack, as open()'s comment says.
[[nodiscard]] std::expected<Opened, std::string> openRootLog(const Functions& functions)
{
    const std::filesystem::path& parent = functions.linuxLogParent;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const OwnedFd parentFd(::open(parent.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info = {};
    if (!parentFd.valid())
    {
        return std::unexpected(std::format("{} can't be opened: {}", parent.string(), errnoText(errno)));
    }
    if (::fstat(parentFd.get(), &info) != 0 || !ownedOnlyByUs(info))
    {
        return std::unexpected(std::format("{} isn't a folder only this user may change", parent.string()));
    }
    constexpr const char* FOLDER = "tasksmack";
    if (::mkdirat(parentFd.get(), FOLDER, S_IRWXU) != 0 && errno != EEXIST)
    {
        return std::unexpected(std::format("{}/{} can't be made: {}", parent.string(), FOLDER, errnoText(errno)));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const OwnedFd folderFd(::openat(parentFd.get(), FOLDER, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!folderFd.valid())
    {
        return std::unexpected(std::format("{}/{} isn't a plain folder: {}", parent.string(), FOLDER, errnoText(errno)));
    }
    if (::fstat(folderFd.get(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
    {
        return std::unexpected(std::format("{}/{} isn't this user's own folder", parent.string(), FOLDER));
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0 && ::fchmod(folderFd.get(), S_IRWXU) != 0)
    {
        return std::unexpected(std::format("{}/{} can't be made private: {}", parent.string(), FOLDER, errnoText(errno)));
    }
    const std::string fileName = rootLogName(functions.invokingUid());
    // Any old log of this user's sessions goes, a link or a planted file included; the new one is made fresh, so none is
    // reused. Another user's root session's log has its own name and is left alone (#4261).
    if (::unlinkat(folderFd.get(), fileName.c_str(), 0) != 0 && errno != ENOENT)
    {
        return std::unexpected(std::format("the old {} can't be removed: {}", fileName, errnoText(errno)));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    OwnedFd file(::openat(folderFd.get(), fileName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (!file.valid())
    {
        return std::unexpected(std::format("{} can't be made fresh: {}", fileName, errnoText(errno)));
    }
    return Opened{
        .path = parent / FOLDER / fileName,
        .sink = std::make_shared<FdFileSink>(std::move(file)),
    };
}
#endif

} // namespace

std::string rootLogName(std::optional<std::uint32_t> invokingUid)
{
    return invokingUid.has_value() ? std::format("tasksmack-debug-{}.log", *invokingUid) : std::string("tasksmack-debug-root.log");
}

std::optional<std::uint32_t> systemInvokingUid()
{
#ifdef __linux__
    if (const auto& invoking = Platform::SessionUser::processInvokingUser(); invoking.has_value())
    {
        return static_cast<std::uint32_t>(invoking->uid);
    }
#endif
    return std::nullopt;
}

std::filesystem::path systemTempFolder()
{
    return std::filesystem::temp_directory_path();
}

std::optional<std::filesystem::path> systemProgramDataFolder()
{
#ifdef _WIN32
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path);
    // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(path, &::CoTaskMemFree);
    if (FAILED(result) || owned == nullptr || owned.get()[0] == L'\0')
    {
        return std::nullopt;
    }
    return std::filesystem::path(owned.get());
#else
    return std::nullopt;
#endif
}

std::filesystem::path elevatedLogFolder(const std::filesystem::path& programData)
{
    return programData / "TaskSmack" / "Logs";
}

#ifdef _WIN32
std::wstring systemFinalPath(void* handle)
{
    return Platform::Windows::finalPathOf(handle).value_or(std::wstring{});
}

std::expected<void, std::string> secureFolder(const std::filesystem::path& folder, std::string_view sddl)
{
    const WinFile::OwnedSecurityDescriptor descriptor = WinFile::securityDescriptorFrom(sddl);
    if (descriptor == nullptr)
    {
        return std::unexpected(std::string("its security can't be built"));
    }
    const auto parent = WinFile::openFolderByPath(folder.parent_path());
    if (!parent.has_value())
    {
        return std::unexpected(std::format("{} can't be opened: {}", Core::pathToUtf8(folder.parent_path()), parent.error()));
    }
    auto held = WinFile::secureChildFolder(parent->get(), folder.filename().native(), descriptor.get(), Core::pathToUtf8(folder));
    if (!held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    return {};
}
#endif

std::expected<Opened, std::string> open(bool lessPrivilegedEnvironment, const Functions& functions)
{
    try
    {
        if (lessPrivilegedEnvironment)
        {
#ifdef _WIN32
            return openElevated(functions);
#else
            return openRootLog(functions);
#endif
        }
        // Opened by its native path (UTF-16 on Windows, SPDLOG_WCHAR_FILENAMES), never a narrowed one, so a temp folder
        // with any characters works with or without the manifest (#1648).
        std::filesystem::path path = functions.tempFolder() / FILE_NAME;
        spdlog::sink_ptr sink = Core::makeLogFileSink(path, true);
        return Opened{.path = std::move(path), .sink = std::move(sink)};
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string(e.what()));
    }
    catch (...)
    {
        return std::unexpected(std::string("unknown error"));
    }
}

} // namespace App::DebugLogFile
