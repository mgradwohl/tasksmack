/// @file test_WindowsLocalNtFile.cpp
/// @brief Platform::Windows::openLocalNtFile() (#4373, #4482), the one open the Modules section's version read and the
/// image file the Publisher column and Look up on VirusTotal read share. Through fakes: the kernel's NT name itself
/// (no \\?\GLOBALROOT), OBJ_DONT_REPARSE, FILE_OPEN_REPARSE_POINT, identification-level security, the access and
/// sharing the caller asks for; a network share's or a pipe's name never opened; and a link, a handle off a local disk,
/// a remote device or a redirector's file refused after the open, each saying why. On the real system: a file opened
/// by its NT name, and the same file through a junction refused, never followed.

#include "Mocks/WindowsFakeLocalNtFile.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsLocalNtFile.h"
#include "Support/Junction.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h> // FILE_DEVICE_DISK and the other device types
#include <winternl.h> // FILE_OPEN and the open options
// clang-format on

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::Windows
{
namespace
{

using TestMocks::FAKE_LOCAL_NT_FILE;
using TestMocks::localNtFile;

constexpr std::wstring_view LOCAL_NAME = L"\\Device\\HarddiskVolume3\\Windows\\System32\\notepad.exe";
constexpr LocalFileAccess READ_ONLY{.access = GENERIC_READ, .share = FILE_SHARE_READ, .sequential = false};
constexpr ULONG SEQUENTIAL_ONLY = 0x00000004U; // FILE_SEQUENTIAL_ONLY

/// The refusal openLocalNtFile() gives @p name with the fake as it stands, or nullopt when it opened it.
[[nodiscard]] std::optional<LocalFileRefusal> refusalOf(std::wstring_view name = LOCAL_NAME)
{
    const std::expected<UniqueHandle, LocalFileError> file = openLocalNtFile(name, READ_ONLY, FAKE_LOCAL_NT_FILE);
    if (file.has_value())
    {
        return std::nullopt;
    }
    return file.error().reason;
}

class WindowsLocalNtFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        localNtFile() = {};
    }
};

TEST_F(WindowsLocalNtFileTest, OpensTheKernelsNameWithoutFollowingAnyReparseAndForIdentificationOnly)
{
    const std::expected<UniqueHandle, LocalFileError> file = openLocalNtFile(LOCAL_NAME, READ_ONLY, FAKE_LOCAL_NT_FILE);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(file->get() != nullptr);
    ASSERT_EQ(localNtFile().opens.size(), 1U);
    const TestMocks::LocalNtOpen& open = localNtFile().opens[0];
    EXPECT_EQ(open.name, LOCAL_NAME) << R"(the NT name itself, not through \\?\GLOBALROOT)";
    EXPECT_FALSE(open.rooted);
    EXPECT_NE(open.objectAttributes & TestMocks::OBJ_DONT_REPARSE_FLAG, 0U) << "OBJ_DONT_REPARSE";
    EXPECT_NE(open.objectAttributes & static_cast<ULONG>(OBJ_CASE_INSENSITIVE), 0U);
    EXPECT_NE(open.options & static_cast<ULONG>(FILE_OPEN_REPARSE_POINT), 0U);
    EXPECT_NE(open.options & static_cast<ULONG>(FILE_NON_DIRECTORY_FILE), 0U);
    EXPECT_NE(open.options & static_cast<ULONG>(FILE_SYNCHRONOUS_IO_NONALERT), 0U);
    EXPECT_EQ(open.options & SEQUENTIAL_ONLY, 0U);
    EXPECT_TRUE(open.qualityOfService);
    EXPECT_EQ(open.impersonation, SecurityIdentification);
    EXPECT_EQ(open.effectiveOnly, TRUE);
    EXPECT_EQ(open.disposition, static_cast<ULONG>(FILE_OPEN));
    EXPECT_EQ(open.access, static_cast<ACCESS_MASK>(GENERIC_READ | SYNCHRONIZE));
    EXPECT_EQ(open.share, static_cast<ULONG>(FILE_SHARE_READ));
}

// The access, sharing and sequential hint are the caller's: the image file's hash open and its identity open.
TEST_F(WindowsLocalNtFileTest, TheAccessAndSharingAreTheCallers)
{
    ASSERT_TRUE(openLocalNtFile(LOCAL_NAME, {.access = GENERIC_READ, .share = FILE_SHARE_READ, .sequential = true}, FAKE_LOCAL_NT_FILE)
                    .has_value());
    ASSERT_TRUE(openLocalNtFile(
                    LOCAL_NAME,
                    {.access = FILE_READ_ATTRIBUTES, .share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, .sequential = false},
                    FAKE_LOCAL_NT_FILE)
                    .has_value());
    ASSERT_EQ(localNtFile().opens.size(), 2U);
    EXPECT_NE(localNtFile().opens[0].options & SEQUENTIAL_ONLY, 0U);
    EXPECT_EQ(localNtFile().opens[1].access, static_cast<ACCESS_MASK>(FILE_READ_ATTRIBUTES | SYNCHRONIZE));
    EXPECT_EQ(localNtFile().opens[1].share, static_cast<ULONG>(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    EXPECT_EQ(localNtFile().opens[1].options & SEQUENTIAL_ONLY, 0U);
}

TEST_F(WindowsLocalNtFileTest, AFailedOpenSaysWhyInWin32Terms)
{
    localNtFile().openStatus = static_cast<NTSTATUS>(0xC0000034L); // STATUS_OBJECT_NAME_NOT_FOUND
    const std::expected<UniqueHandle, LocalFileError> file = openLocalNtFile(LOCAL_NAME, READ_ONLY, FAKE_LOCAL_NT_FILE);
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().reason, LocalFileRefusal::OpenFailed);
    EXPECT_EQ(file.error().status, static_cast<NTSTATUS>(0xC0000034L));
    EXPECT_EQ(file.error().win32Error, static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
    EXPECT_EQ(describeLocalFileError(file.error()), Platform::WinString::win32ErrorText(ERROR_FILE_NOT_FOUND));

    localNtFile().openStatus = static_cast<NTSTATUS>(0xC000050BL); // STATUS_REPARSE_POINT_ENCOUNTERED: a link on the way
    EXPECT_EQ(refusalOf(), LocalFileRefusal::OpenFailed);
}

// A network share's name, an in-box redirector's and a pipe's are never opened: opening one is the connection.
TEST_F(WindowsLocalNtFileTest, AShareARedirectorOrAPipeIsNeverOpened)
{
    EXPECT_EQ(refusalOf(L"\\Device\\Mup\\attacker\\share\\x.exe"), LocalFileRefusal::NotLocal);
    EXPECT_EQ(refusalOf(L"\\device\\MUP\\attacker\\share\\x.exe"), LocalFileRefusal::NotLocal) << "case aside";
    EXPECT_EQ(refusalOf(L"\\Device\\LanmanRedirector\\;Z:0000000000012345\\server\\share\\x.exe"), LocalFileRefusal::NotLocal);
    EXPECT_EQ(refusalOf(L"\\Device\\WebDavRedirector\\host\\x.exe"), LocalFileRefusal::NotLocal);
    EXPECT_EQ(refusalOf(L"\\Device\\NamedPipe\\x"), LocalFileRefusal::NotLocal);
    EXPECT_TRUE(localNtFile().opens.empty());
    EXPECT_EQ(describeLocalFileError({.reason = LocalFileRefusal::NotLocal}), "it isn't on a drive TaskSmack can check");
    // A name that only starts like one isn't one.
    EXPECT_EQ(refusalOf(L"\\Device\\MupX\\x.exe"), std::nullopt);
}

TEST_F(WindowsLocalNtFileTest, ALinkAHandleOffADiskOrARedirectorsFileIsRefused)
{
    localNtFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    localNtFile().reparseTag = IO_REPARSE_TAG_SYMLINK;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::Link) << "a symlink, opened as itself";
    localNtFile().reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::Link) << "a junction";
    EXPECT_EQ(describeLocalFileError({.reason = LocalFileRefusal::Link}),
              "it is a symbolic link or a junction, which TaskSmack doesn't follow");

    localNtFile() = {};
    localNtFile().deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal);
    localNtFile().deviceType = FILE_DEVICE_NAMED_PIPE;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal);
    localNtFile().deviceType = FILE_DEVICE_DISK;
    localNtFile().characteristics = 0x00000010; // FILE_REMOTE_DEVICE
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal);

    localNtFile() = {};
    localNtFile().remote = true;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal);
}

// A reparse point that isn't a name surrogate is the file itself (a CompactOS system file's WOF tag), so it is kept;
// so are a CD-ROM's and a virtual disk's files.
TEST_F(WindowsLocalNtFileTest, ACompressedFileACdRomAndAVirtualDiskAreKept)
{
    localNtFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    localNtFile().reparseTag = IO_REPARSE_TAG_WOF;
    EXPECT_EQ(refusalOf(), std::nullopt);
    localNtFile() = {};
    localNtFile().deviceType = FILE_DEVICE_CD_ROM;
    EXPECT_EQ(refusalOf(), std::nullopt);
    localNtFile().deviceType = FILE_DEVICE_VIRTUAL_DISK;
    EXPECT_EQ(refusalOf(), std::nullopt);
}

// #4647: FAT, FAT32 and exFAT fail FileAttributeTagInfo with ERROR_INVALID_PARAMETER. A file there is kept, judged by
// its basic attributes; one that says it is a reparse point is refused as a link, since its tag can't be read.
TEST_F(WindowsLocalNtFileTest, AFileOnAVolumeWithoutReparseTagsIsKept)
{
    localNtFile().tagInfoFails = true;
    EXPECT_EQ(refusalOf(), std::nullopt) << "a FAT disk's file";
    localNtFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::Link);
    localNtFile().attributes = FILE_ATTRIBUTE_ARCHIVE;
    localNtFile().basicInfoFails = true;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal) << "nothing about the handle can be read";

    localNtFile() = {};
    localNtFile().tagInfoFails = true;
    localNtFile().deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal) << "the device check still runs";
    localNtFile().deviceType = FILE_DEVICE_DISK;
    localNtFile().remote = true;
    EXPECT_EQ(refusalOf(), LocalFileRefusal::NotLocal) << "and the redirector's";
}

TEST_F(WindowsLocalNtFileTest, NoCallsOrAnEmptyOrOverlongNameIsUnavailable)
{
    EXPECT_EQ(openLocalNtFile(LOCAL_NAME, READ_ONLY, LocalFileFunctions{}).error().reason, LocalFileRefusal::Unavailable);
    EXPECT_EQ(refusalOf(L""), LocalFileRefusal::Unavailable);
    EXPECT_EQ(refusalOf(std::wstring(0x8000, L'a')), LocalFileRefusal::Unavailable) << "past a UNICODE_STRING's length";
    EXPECT_TRUE(localNtFile().opens.empty());
    EXPECT_FALSE(describeLocalFileError({.reason = LocalFileRefusal::Unavailable}).empty());
}

/// A folder of this test's own under the temp folder, removed with everything in it at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"tasksmack-4482-{}-{}", GetCurrentProcessId(), GetTickCount64()))
    {
        std::filesystem::create_directories(m_Path);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ScratchFolder(ScratchFolder&&) = delete;
    ScratchFolder& operator=(ScratchFolder&&) = delete;
    ~ScratchFolder()
    {
        try
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_Path, ignored);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor; a scratch folder left behind is only clutter
        {}
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// The NT name of @p path ("\Device\HarddiskVolume3\..."), as GetMappedFileNameW would give it; empty when unreadable.
[[nodiscard]] std::wstring ntNameOf(const std::filesystem::path& path)
{
    const UniqueHandle handle(CreateFileW(path.c_str(),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr));
    if (!handle)
    {
        return {};
    }
    std::wstring name(32768, L'\0');
    const DWORD length =
        GetFinalPathNameByHandleW(handle.get(), name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (length == 0 || length >= name.size())
    {
        return {};
    }
    name.resize(length);
    return name;
}

// On the real system with no network: a file opened by its own NT name is kept, and is the file; the same file named
// through a junction (one a process could plant on the way since its name was read) fails the open, never followed.
TEST(WindowsLocalNtFileRealTest, ANameThroughAJunctionIsNotFollowed)
{
    const ScratchFolder scratch;
    const std::filesystem::path target = scratch.path() / L"target";
    const std::filesystem::path link = scratch.path() / L"link";
    std::filesystem::create_directory(target);
    std::filesystem::create_directory(link);
    std::ofstream(target / L"f.bin", std::ios::binary) << "abc";
    const std::optional<std::string> notMade = TestSupport::makeJunction(link, target);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});

    const std::wstring folder = ntNameOf(scratch.path());
    ASSERT_FALSE(folder.empty()) << GetLastError();
    {
        const std::expected<UniqueHandle, LocalFileError> direct = openLocalNtFile(folder + L"\\target\\f.bin", READ_ONLY);
        ASSERT_TRUE(direct.has_value()) << describeLocalFileError(direct.error());
        LARGE_INTEGER size{};
        ASSERT_NE(GetFileSizeEx(direct->get(), &size), 0);
        EXPECT_EQ(size.QuadPart, 3);
    }
    const std::expected<UniqueHandle, LocalFileError> through = openLocalNtFile(folder + L"\\link\\f.bin", READ_ONLY);
    ASSERT_FALSE(through.has_value()) << "the junction on the way is never followed";
    EXPECT_EQ(through.error().reason, LocalFileRefusal::OpenFailed);
    EXPECT_TRUE(std::filesystem::exists(link / L"f.bin")) << "a path through it that a by-name open would have followed";

    ASSERT_TRUE(TestSupport::removeJunction(link)) << "an empty folder again, so removing the scratch folder removes nothing behind it";
}

} // namespace
} // namespace Platform::Windows
