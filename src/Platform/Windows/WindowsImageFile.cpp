#include "WindowsImageFile.h"

#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"
#include "WindowsVersionResourceMath.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <psapi.h>   // GetMappedFileNameW (K32GetMappedFileNameW, in kernel32)
#include <winver.h>  // VerQueryValueW
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// Win32 error @p error as the system words it.
[[nodiscard]] std::string describe(DWORD error)
{
    return Platform::WinString::win32ErrorText(error);
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

/// The refusal for a protected process's image (#3861, #4057): never hashed or checked by its path here.
constexpr std::string_view PROTECTED_REFUSAL = "Its executable can't be read: Windows doesn't show which file a protected process runs";

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

/// Whether @p address in @p process is in an image mapping (VirtualQueryEx's MEM_IMAGE), or the call's Win32 error when
/// it fails: a failed query isn't an answer that the image is elsewhere. Without the call, assumed so.
[[nodiscard]] std::expected<bool, DWORD> isImageAt(const ImageFileFunctions& fns, HANDLE process, const void* address)
{
    if (fns.virtualQueryEx == nullptr)
    {
        return true;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (fns.virtualQueryEx(process, address, &region, sizeof(region)) == 0)
    {
        return std::unexpected(GetLastError());
    }
    return region.Type == MEM_IMAGE;
}

/// Whether @p process has exited, by GetExitCodeProcess (no call, or a failed one: not known to have).
[[nodiscard]] bool hasExited(const ImageFileFunctions& fns, HANDLE process)
{
    DWORD exitCode = STILL_ACTIVE;
    return fns.getExitCodeProcess != nullptr && fns.getExitCodeProcess(process, &exitCode) != 0 && exitCode != STILL_ACTIVE;
}

/// @p text, then "; @p notDone" when there is one.
[[nodiscard]] std::string withNotDone(std::string text, std::string_view notDone)
{
    if (!notDone.empty())
    {
        text += "; ";
        text += notDone;
    }
    return text;
}

/// Where @p process's image is and its file's name now (#3861): at its entry point (ProcessImageInformation's
/// TransferAddress), and only there. An image the loader mapped elsewhere (a .NET executable, one relocated from its
/// preferred base) is refused, never looked for by the name the process started with, which the process can put a
/// decoy under (#4021, #4057; see WindowsImageFile.h).
[[nodiscard]] std::expected<ImageLocation, std::string>
locateImage(const ImageFileFunctions& fns, HANDLE process, void* entryPoint, std::string_view notDone)
{
    const std::expected<bool, DWORD> isImage = isImageAt(fns, process, entryPoint);
    if (!isImage.has_value())
    {
        return std::unexpected(std::format("Its executable can't be read: {}", describe(isImage.error())));
    }
    if (!*isImage)
    {
        return std::unexpected(withNotDone("Its executable's image isn't where Windows says it is, so it couldn't be confirmed", notDone));
    }
    const std::expected<std::wstring, DWORD> name = mappedFileName(fns, process, entryPoint);
    if (name.has_value())
    {
        return ImageLocation{.address = entryPoint, .name = *name};
    }
    // Access denied is a protected process's answer, but not only theirs: openProcessImageFile() says protected only
    // when the process is one.
    return std::unexpected(std::format("Its executable can't be read: {}", describe(name.error())));
}

// Structured exception handling (__try/__except) is a Microsoft extension, which -Wpedantic warns of: it is what catches
// an in-page error reading a mapped file, so it is allowed here, and only here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"

/// The VS_VERSIONINFO block of the mapped file @p view, copied into @p out; its length, or 0 when it has none or it
/// doesn't fit. Never inlined, so an in-page error raised while it reads the mapping reaches copyVersionResource()'s
/// handler through this call. Not noexcept, so nothing stands between that exception and the handler.
[[nodiscard, clang::noinline]] std::size_t
copyVersionResourceUnguarded(const std::byte* view, std::size_t size, BYTE* out, std::size_t room)
{
    const std::optional<std::span<const std::byte>> block = VersionResourceMath::findVersionResource(std::span(view, size));
    if (!block.has_value() || block->size() > room)
    {
        return 0;
    }
    std::memcpy(out, block->data(), block->size());
    return block->size();
}

/// copyVersionResourceUnguarded(), where an in-page error reading the mapping (a network file that went away, a read
/// error on the disk) is no block rather than a crash. Nothing here needs unwinding, as __try requires.
[[nodiscard]] std::size_t copyVersionResource(const std::byte* view, std::size_t size, BYTE* out, std::size_t room)
{
    std::size_t copied = 0;
    __try
    {
        copied = copyVersionResourceUnguarded(view, size, out, room);
    }
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        copied = 0;
    }
    return copied;
}

#pragma clang diagnostic pop

/// Unmaps a view of a file mapping.
struct ViewUnmapper
{
    void operator()(const void* view) const noexcept
    {
        UnmapViewOfFile(view);
    }
};

} // namespace

ImageFileFunctions systemImageFileFunctions() noexcept
{
    return {
        .ntQueryInformationProcess = ntQueryInformationProcess(),
        .getMappedFileName = &GetMappedFileNameW,
        .virtualQueryEx = &VirtualQueryEx,
        .getExitCodeProcess = &GetExitCodeProcess,
        .getProcessInformation = &GetProcessInformation,
    };
}

bool isProtectedProcess(const ImageFileFunctions& fns, HANDLE process)
{
    if (fns.getProcessInformation == nullptr)
    {
        return false;
    }
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (fns.getProcessInformation(process, ProcessProtectionLevelInfo, &protection, sizeof(protection)) == 0)
    {
        return false;
    }
    return protection.ProtectionLevel != PROTECTION_LEVEL_NONE;
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
openProcessImageFile(const ImageFileFunctions& fns, HANDLE process, std::int32_t pid, std::string_view notDone)
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
    const std::expected<ImageLocation, std::string> image = locateImage(fns, process, info.TransferAddress, notDone);
    if (!image.has_value())
    {
        // A query that failed because the process has ended says so. A protected process (or PPL) may not show its
        // memory map at all: said so, only when it is one, rather than that its image is elsewhere or can't be read.
        // It is never looked up by its path instead.
        if (hasExited(fns, process))
        {
            return std::unexpected(std::format("Process {} has exited", pid));
        }
        if (isProtectedProcess(fns, process))
        {
            return std::unexpected(withNotDone(std::string(PROTECTED_REFUSAL), notDone));
        }
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
        if (hasExited(fns, process))
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

std::string versionInfoCompanyName(std::span<BYTE> block)
{
    // VerQueryValueW hands back a pointer and the length the block's own node declares, which a file can make up
    // (#4057): only bytes inside block are read, however long a value says it is.
    const BYTE* const begin = block.data();
    const BYTE* const end = begin + block.size();
    const auto inBlock = [begin, end](LPVOID value) -> std::size_t
    {
        const auto* const at = static_cast<const BYTE*>(value);
        return (at == nullptr || at < begin || at >= end) ? 0 : static_cast<std::size_t>(end - at);
    };
    const auto companyName = [block, &inBlock](const std::wstring& subBlock) -> std::string
    {
        LPVOID value = nullptr;
        UINT length = 0;
        if (VerQueryValueW(block.data(), subBlock.c_str(), &value, &length) == 0 || length == 0)
        {
            return {};
        }
        // A StringFileInfo value is a wide string, length characters with its terminator, inside block.
        const std::size_t room = inBlock(value) / sizeof(wchar_t);
        const auto* const text = static_cast<const wchar_t*>(value);
        return WinString::wideToUtf8(std::wstring_view(text, wcsnlen(text, std::min<std::size_t>(length, room))));
    };

    // Each translation is a DWORD: the language ID in its low word, the code page in its high word. Copied out, since
    // the value needn't be aligned.
    LPVOID translations = nullptr;
    UINT translationBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &translations, &translationBytes) != 0)
    {
        const std::size_t bytes = std::min<std::size_t>(translationBytes, inBlock(translations));
        const auto* const first = static_cast<const BYTE*>(translations);
        for (std::size_t at = 0; at + sizeof(DWORD) <= bytes; at += sizeof(DWORD))
        {
            DWORD entry = 0;
            std::memcpy(&entry, first + at, sizeof(entry));
            std::string name = companyName(std::format(L"\\StringFileInfo\\{:04X}{:04X}\\CompanyName", LOWORD(entry), HIWORD(entry)));
            if (!name.empty())
            {
                return name;
            }
        }
    }
    // No translation table, or none of its tables names one: US English, Unicode then Windows-1252.
    if (std::string name = companyName(L"\\StringFileInfo\\040904B0\\CompanyName"); !name.empty())
    {
        return name;
    }
    return companyName(L"\\StringFileInfo\\040904E4\\CompanyName");
}
std::string heldFilePublisher(HANDLE file)
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 || std::cmp_greater(size.QuadPart, SIZE_MAX))
    {
        return {}; // an empty file can't be mapped, and has no version info anyway
    }
    // A data mapping, read-only: nothing in it is run or relocated, and it is the held file's, whatever its name is now.
    const UniqueHandle mapping(CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping)
    {
        return {};
    }
    const std::unique_ptr<const void, ViewUnmapper> view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr)
    {
        return {};
    }
    // VerQueryValueW is given twice the largest block's room, zeroed, as GetFileVersionInfoSizeW asks for: it may write
    // after the block. Kept at that size whatever the block's own length, so a value that claims to run past the block
    // still ends inside this buffer.
    constexpr std::size_t MAX_BLOCK = 0xFFFF; // VS_VERSIONINFO's wLength is a WORD
    std::vector<BYTE> block((2 * MAX_BLOCK) + 16, BYTE{0});
    const std::size_t copied =
        copyVersionResource(static_cast<const std::byte*>(view.get()), static_cast<std::size_t>(size.QuadPart), block.data(), MAX_BLOCK);
    if (copied == 0)
    {
        return {};
    }
    return versionInfoCompanyName(block);
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
