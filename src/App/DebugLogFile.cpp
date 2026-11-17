#include "App/DebugLogFile.h"

#include "Core/LogFileSink.h"

#include <spdlog/common.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include <aclapi.h> // SetSecurityInfo
#include <sddl.h>   // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <shlobj.h> // SHGetKnownFolderPath
// clang-format on

#include "Core/Utf8Path.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <format>
#include <mutex>
#include <system_error>
#include <vector>
#endif

namespace App::DebugLogFile
{
namespace
{

#ifdef _WIN32
/// Closes a Win32 handle; INVALID_HANDLE_VALUE and null are none.
struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};
using OwnedHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] OwnedHandle own(HANDLE handle) noexcept
{
    return OwnedHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

[[nodiscard]] std::string lastError()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

/// The log written through the file handle checked when it was opened (#4105), never reopened by its path. WriteFile
/// hands each line to the system as it is written, so there is nothing of its own to flush.
class HandleFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit HandleFileSink(OwnedHandle file) : m_File(std::move(file))
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        DWORD written = 0;
        // A line that can't be written is lost, as one the CRT can't write is in a path-opened file.
        static_cast<void>(WriteFile(m_File.get(), formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr));
    }

    void flush_() override
    {}

  private:
    OwnedHandle m_File;
};

/// Whether @p handle's final path is @p path (ignoring case, as NTFS names do).
[[nodiscard]] bool isAt(HANDLE handle, const std::filesystem::path& path)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (length >= buffer.size())
    {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    }
    if (length == 0 || length >= buffer.size())
    {
        return false;
    }
    std::wstring_view finalPath(buffer.data(), length);
    constexpr std::wstring_view LONG_PREFIX = LR"(\\?\)";
    if (finalPath.starts_with(LONG_PREFIX))
    {
        finalPath.remove_prefix(LONG_PREFIX.size());
    }
    const std::wstring& expected = path.native();
    return CompareStringOrdinal(
               finalPath.data(), static_cast<int>(finalPath.size()), expected.data(), static_cast<int>(expected.size()), TRUE) ==
           CSTR_EQUAL;
}

/// An elevated session's log (#4105), in %ProgramData%\TaskSmack\Logs, as this file's header says.
[[nodiscard]] std::expected<Opened, std::string> openElevated(const Functions& functions)
{
    const std::optional<std::filesystem::path> programData = functions.programDataFolder();
    if (!programData.has_value())
    {
        return std::unexpected(std::string("the ProgramData folder isn't known"));
    }
    const std::filesystem::path logs = elevatedLogFolder(*programData);
    for (const std::filesystem::path& folder : {logs.parent_path(), logs})
    {
        if (auto secured = secureFolder(folder, functions.folderSecurity); !secured.has_value())
        {
            return std::unexpected(std::move(secured.error()));
        }
    }
    const std::filesystem::path path = logs / FILE_NAME;
    // Opened without following a link (FILE_FLAG_OPEN_REPARSE_POINT), and not emptied until it is checked.
    OwnedHandle file = own(CreateFileW(path.c_str(),
                                       GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr));
    if (file == nullptr)
    {
        return std::unexpected(std::format("{} can't be opened: {}", Core::pathToUtf8(path), lastError()));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file.get(), &info) == FALSE)
    {
        return std::unexpected(std::format("{} can't be read: {}", Core::pathToUtf8(path), lastError()));
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        return std::unexpected(std::format("{} is a link or a folder", Core::pathToUtf8(path)));
    }
    if (info.nNumberOfLinks != 1)
    {
        return std::unexpected(std::format("{} is hard-linked to another file", Core::pathToUtf8(path)));
    }
    if (!isAt(file.get(), path))
    {
        return std::unexpected(std::format("{} opened somewhere else", Core::pathToUtf8(path)));
    }
    // Emptied only now, through the handle checked: a run's log starts empty, as a path-opened one does.
    FILE_END_OF_FILE_INFO end{};
    if (SetFileInformationByHandle(file.get(), FileEndOfFileInfo, &end, sizeof(end)) == FALSE)
    {
        return std::unexpected(std::format("{} can't be emptied: {}", Core::pathToUtf8(path), lastError()));
    }
    return Opened{.path = path, .sink = std::make_shared<HandleFileSink>(std::move(file))};
}
#endif

} // namespace

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
std::expected<void, std::string> secureFolder(const std::filesystem::path& folder, std::string_view sddl)
{
    const std::string name = Core::pathToUtf8(folder);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring wideSddl(sddl.begin(), sddl.end()); // SDDL is ASCII
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(wideSddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
    {
        return std::unexpected(std::format("its security can't be built: {}", lastError()));
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> ownedDescriptor(descriptor, &::LocalFree);
    SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = descriptor, .bInheritHandle = FALSE};
    const bool made = CreateDirectoryW(folder.c_str(), &attributes) != FALSE;
    if (!made && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return std::unexpected(std::format("{} can't be made: {}", name, lastError()));
    }
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    PACL dacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) == FALSE ||
        GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted) == FALSE || daclPresent == FALSE)
    {
        return std::unexpected(std::string("its security names no DACL"));
    }
    // The folder itself, never what a junction or link there names; held while it is checked and secured.
    const OwnedHandle handle = own(CreateFileW(folder.c_str(),
                                               READ_CONTROL | WRITE_DAC | (owner != nullptr ? WRITE_OWNER : 0) | FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                               nullptr));
    if (handle == nullptr)
    {
        return std::unexpected(std::format("{} can't be opened: {}", name, lastError()));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle.get(), &info) == FALSE)
    {
        return std::unexpected(std::format("{} can't be read: {}", name, lastError()));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return std::unexpected(std::format("{} is a junction or link", name));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return std::unexpected(std::format("{} isn't a folder", name));
    }
    if (!isAt(handle.get(), folder))
    {
        return std::unexpected(std::format("{} is reached through a link", name));
    }
    if (!made)
    {
        // There already, perhaps made by someone else (Users may create folders in %ProgramData%): taken over, so only
        // Administrators and SYSTEM may change it from now on.
        SECURITY_INFORMATION what = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
        if (owner != nullptr)
        {
            what |= OWNER_SECURITY_INFORMATION;
        }
        if (const DWORD error = SetSecurityInfo(handle.get(), SE_FILE_OBJECT, what, owner, nullptr, dacl, nullptr); error != ERROR_SUCCESS)
        {
            return std::unexpected(std::format("{} can't be secured: {}", name, std::system_category().message(static_cast<int>(error))));
        }
    }
    return {};
}
#endif

std::expected<Opened, std::string> open([[maybe_unused]] bool lessPrivilegedEnvironment, const Functions& functions)
{
    try
    {
#ifdef _WIN32
        if (lessPrivilegedEnvironment)
        {
            return openElevated(functions);
        }
#endif
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
