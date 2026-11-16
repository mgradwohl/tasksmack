#include "WindowsImageFile.h"

#include "WindowsHandles.h"
#include "WindowsNtQuery.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <psapi.h>   // GetMappedFileNameW (K32GetMappedFileNameW, in kernel32)
// clang-format on

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// Win32 error @p error as the system words it.
[[nodiscard]] std::string describe(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// SECTION_IMAGE_INFORMATION, which NtQueryInformationProcess's ProcessImageInformation (class 37) fills from the
/// image section the process was created from (#3861); winternl.h has neither. Only TransferAddress (the entry
/// point, in the process's address space, so inside its mapped image) and ImageFileSize are read.
struct SectionImageInformation
{
    void* TransferAddress = nullptr;
    ULONG ZeroBits = 0;
    SIZE_T MaximumStackSize = 0;
    SIZE_T CommittedStackSize = 0;
    ULONG SubSystemType = 0;
    ULONG SubSystemVersion = 0;
    ULONG OperatingSystemVersion = 0;
    USHORT ImageCharacteristics = 0;
    USHORT DllCharacteristics = 0;
    USHORT Machine = 0;
    BOOLEAN ImageContainsCode = 0;
    UCHAR ImageFlags = 0;
    ULONG LoaderFlags = 0;
    ULONG ImageFileSize = 0;
    ULONG CheckSum = 0;
};
static_assert(sizeof(void*) != 8 || sizeof(SectionImageInformation) == 64, "ntdll's 64-bit SECTION_IMAGE_INFORMATION");
constexpr ULONG PROCESS_IMAGE_INFORMATION_CLASS = 37;
constexpr DWORD IMAGE_NAME_CHARS = 32768; // the longest path Windows allows, with its terminator

/// The NT name of the file mapped at @p address in @p process now (GetMappedFileNameW), or its Win32 error.
[[nodiscard]] std::expected<std::wstring, DWORD> mappedFileName(const ImageFileFunctions& fns, HANDLE process, void* address)
{
    std::wstring name(IMAGE_NAME_CHARS, L'\0');
    const DWORD length = fns.getMappedFileName(process, address, name.data(), IMAGE_NAME_CHARS);
    if (length == 0)
    {
        return std::unexpected(GetLastError());
    }
    if (length >= IMAGE_NAME_CHARS - 1)
    {
        return std::unexpected(static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE)); // GetMappedFileNameW truncates silently
    }
    name.resize(length);
    return name;
}

/// The refusal when the file at the image's name isn't the image (#3861): said so, never read as if it were. Only
/// for a real mismatch (another file, another size); a check that can't be made says so instead (unconfirmed()).
[[nodiscard]] std::string imageReplaced(std::string_view notDone)
{
    return std::format("Its executable was renamed or replaced while it was read, so the file there isn't the image it runs{}{}",
                       notDone.empty() ? "" : "; ",
                       notDone);
}

/// The refusal when the image's file can't be confirmed for a reason that isn't a mismatch (#3861): @p why.
[[nodiscard]] std::string unconfirmed(std::string_view why, std::string_view notDone)
{
    return std::format("Its executable couldn't be confirmed as the image it runs ({}){}{}", why, notDone.empty() ? "" : ", so ", notDone);
}

/// Opens the NT name @p ntName (\Device\HarddiskVolumeN\..., as GetMappedFileNameW writes it) through the Win32
/// call, by \\?\GLOBALROOT, with @p access and @p share.
[[nodiscard]] UniqueHandle openNtName(const std::wstring& ntName, DWORD access, DWORD share)
{
    const std::wstring path = L"\\\\?\\GLOBALROOT" + ntName;
    return UniqueHandle(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}

/// Where @p process's image is mapped and its file's NT name now.
struct ImageLocation
{
    void* address = nullptr;
    std::wstring name;
};

/// Whether @p address in @p process is in an image mapping (VirtualQueryEx's MEM_IMAGE). Without the call, assumed so.
[[nodiscard]] bool isImageAt(const ImageFileFunctions& fns, HANDLE process, const void* address)
{
    if (fns.virtualQueryEx == nullptr)
    {
        return true;
    }
    MEMORY_BASIC_INFORMATION region{};
    return fns.virtualQueryEx(process, address, &region, sizeof(region)) != 0 && region.Type == MEM_IMAGE;
}

/// The image mapped at an allocation base in @p process whose file is still named @p ntName, scanned with
/// VirtualQueryEx (#3861), or nullopt. The names are compared ignoring case, as NTFS does; which file it is, is
/// checked by identity afterwards.
[[nodiscard]] std::optional<ImageLocation> findImageNamed(const ImageFileFunctions& fns, HANDLE process, const std::wstring& ntName)
{
    MEMORY_BASIC_INFORMATION region{};
    const std::byte* address = nullptr;
    while (fns.virtualQueryEx(process, address, &region, sizeof(region)) != 0)
    {
        if (region.Type == MEM_IMAGE && region.BaseAddress == region.AllocationBase)
        {
            const std::expected<std::wstring, DWORD> name = mappedFileName(fns, process, region.BaseAddress);
            if (name.has_value() &&
                CompareStringOrdinal(
                    name->c_str(), static_cast<int>(name->size()), ntName.c_str(), static_cast<int>(ntName.size()), TRUE) == CSTR_EQUAL)
            {
                return ImageLocation{.address = region.BaseAddress, .name = *name};
            }
        }
        const std::byte* const next = static_cast<const std::byte*>(region.BaseAddress) + region.RegionSize;
        if (next <= address)
        {
            break; // the top of the address space, or a region that doesn't advance
        }
        address = next;
    }
    return std::nullopt;
}

/// Where @p process's image is and its file's name now (#3861). At the entry point (ProcessImageInformation's
/// TransferAddress) for most images. An image the loader mapped elsewhere (a .NET executable, one relocated from its
/// preferred base) has nothing there: then it is the image whose file still has the name the process started with
/// (QueryFullProcessImageNameW's native name). One both mapped elsewhere and renamed can't be found, and says so.
[[nodiscard]] std::expected<ImageLocation, std::string>
locateImage(const ImageFileFunctions& fns, HANDLE process, void* entryPoint, std::string_view notDone, bool entryPointOnly)
{
    if (isImageAt(fns, process, entryPoint))
    {
        const std::expected<std::wstring, DWORD> name = mappedFileName(fns, process, entryPoint);
        if (name.has_value())
        {
            return ImageLocation{.address = entryPoint, .name = *name};
        }
        if (name.error() == ERROR_ACCESS_DENIED)
        {
            // A protected process, or one whose memory map Windows won't show this user.
            return std::unexpected(std::string("Its executable can't be read: Windows doesn't show which file a protected process runs"));
        }
        return std::unexpected(std::format("Its executable can't be read: {}", describe(name.error())));
    }
    if (entryPointOnly)
    {
        // Found by name, the image could be any file mapped under that name: a process can rename its own image away,
        // copy a signed file to the old name, map it as a resource and pad itself to its size (#4021 review). So the
        // signature check doesn't take it.
        return std::unexpected(
            std::string("Its executable couldn't be confirmed: its image isn't where Windows says it is (its entry point)"));
    }
    if (fns.queryFullProcessImageName == nullptr) // isImageAt() said no, so virtualQueryEx is there
    {
        return std::unexpected(std::string("Its executable can't be read: its image isn't at its entry point"));
    }
    std::wstring started(IMAGE_NAME_CHARS, L'\0');
    DWORD size = IMAGE_NAME_CHARS;
    if (fns.queryFullProcessImageName(process, PROCESS_NAME_NATIVE, started.data(), &size) == 0)
    {
        return std::unexpected(std::format("Its executable can't be read: {}", describe(GetLastError())));
    }
    started.resize(size);
    if (std::optional<ImageLocation> found = findImageNamed(fns, process, started))
    {
        return std::move(*found);
    }
    return std::unexpected(std::format("Its executable can't be found: its image isn't at its entry point or under the name it "
                                       "started with, so it may have been renamed{}{}",
                                       notDone.empty() ? "" : "; ",
                                       notDone));
}

} // namespace

ImageFileFunctions systemImageFileFunctions() noexcept
{
    return {
        .ntQueryInformationProcess = ntQueryInformationProcess(),
        .getMappedFileName = &GetMappedFileNameW,
        .virtualQueryEx = &VirtualQueryEx,
        .queryFullProcessImageName = &QueryFullProcessImageNameW,
        .getExitCodeProcess = &GetExitCodeProcess,
    };
}

bool FileIdentity::operator==(const FileIdentity& other) const noexcept
{
    return volume == other.volume && std::memcmp(id.Identifier, other.id.Identifier, sizeof(id.Identifier)) == 0;
}

std::optional<FileIdentity> fileIdentity(HANDLE file)
{
    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info)) != 0)
    {
        return FileIdentity{.volume = info.VolumeSerialNumber, .id = info.FileId};
    }
    BY_HANDLE_FILE_INFORMATION basic{};
    if (GetFileInformationByHandle(file, &basic) == 0)
    {
        return std::nullopt;
    }
    FileIdentity identity{.volume = basic.dwVolumeSerialNumber, .id = {}};
    const ULONGLONG index = (static_cast<ULONGLONG>(basic.nFileIndexHigh) << 32U) | basic.nFileIndexLow;
    std::memcpy(identity.id.Identifier, &index, sizeof(index));
    return identity;
}

std::expected<UniqueHandle, std::string>
openProcessImageFile(const ImageFileFunctions& fns, HANDLE process, std::int32_t pid, std::string_view notDone, bool entryPointOnly)
{
    if (fns.ntQueryInformationProcess == nullptr || fns.getMappedFileName == nullptr)
    {
        return std::unexpected(std::string("Its executable can't be read: Windows' calls to find it aren't available"));
    }
    SectionImageInformation info{};
    ULONG written = 0;
    const LONG status = fns.ntQueryInformationProcess(process, PROCESS_IMAGE_INFORMATION_CLASS, &info, sizeof(info), &written);
    if (status < 0)
    {
        // The kernel's own processes (Registry, Memory Compression, Secure System) have no image file.
        return std::unexpected(
            std::format("Its executable can't be read: its image can't be found (NTSTATUS 0x{:08X})", static_cast<ULONG>(status)));
    }
    if (info.TransferAddress == nullptr)
    {
        return std::unexpected(std::string("Its executable can't be read: Windows reports no image for it"));
    }
    const std::expected<ImageLocation, std::string> image = locateImage(fns, process, info.TransferAddress, notDone, entryPointOnly);
    if (!image.has_value())
    {
        return std::unexpected(image.error());
    }
    UniqueHandle file = openNtName(image->name, GENERIC_READ, FILE_SHARE_READ);
    if (!file)
    {
        return std::unexpected(std::format("Its executable can't be read: {}", describe(GetLastError())));
    }
    const std::expected<std::wstring, DWORD> now = mappedFileName(fns, process, image->address);
    if (!now.has_value())
    {
        DWORD exitCode = STILL_ACTIVE;
        if (fns.getExitCodeProcess != nullptr && fns.getExitCodeProcess(process, &exitCode) != 0 && exitCode != STILL_ACTIVE)
        {
            return std::unexpected(std::format("Process {} has exited", pid));
        }
        return std::unexpected(unconfirmed(std::format("its name can't be read again: {}", describe(now.error())), notDone));
    }
    const std::optional<FileIdentity> held = fileIdentity(file.get());
    if (!held.has_value())
    {
        return std::unexpected(unconfirmed("the file's identity can't be read", notDone));
    }
    const UniqueHandle there = openNtName(*now, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (!there)
    {
        return std::unexpected(
            unconfirmed(std::format("the file under its name now can't be opened: {}", describe(GetLastError())), notDone));
    }
    const std::optional<FileIdentity> named = fileIdentity(there.get());
    if (!named.has_value())
    {
        return std::unexpected(unconfirmed("the identity of the file under its name now can't be read", notDone));
    }
    if (*held != *named)
    {
        return std::unexpected(imageReplaced(notDone));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == 0)
    {
        return std::unexpected(unconfirmed(std::format("its size can't be read: {}", describe(GetLastError())), notDone));
    }
    // ImageFileSize is 0 where Windows doesn't fill it in (it isn't documented), and then there is no size to compare.
    if (info.ImageFileSize != 0 && std::cmp_not_equal(size.QuadPart, info.ImageFileSize))
    {
        return std::unexpected(imageReplaced(notDone));
    }
    return file;
}

std::wstring heldFileName(HANDLE file)
{
    const auto finalName = [file](DWORD flags) -> std::wstring
    {
        std::wstring name(IMAGE_NAME_CHARS, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(file, name.data(), IMAGE_NAME_CHARS, flags);
        if (length == 0 || length >= IMAGE_NAME_CHARS)
        {
            return {};
        }
        name.resize(length);
        return name;
    };
    if (const std::wstring dos = finalName(FILE_NAME_NORMALIZED | VOLUME_NAME_DOS); !dos.empty())
    {
        return dos;
    }
    if (const std::wstring nt = finalName(FILE_NAME_NORMALIZED | VOLUME_NAME_NT); !nt.empty())
    {
        return L"\\\\?\\GLOBALROOT" + nt;
    }
    return {};
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
