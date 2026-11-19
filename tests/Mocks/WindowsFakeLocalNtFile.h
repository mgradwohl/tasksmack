/// @file WindowsFakeLocalNtFile.h
/// @brief One fake of the calls Windows::openLocalNtFile() makes (#4373, #4482): NtCreateFile,
/// NtQueryVolumeInformationFile and GetFileInformationByHandleEx, shared by its own tests, the Modules reader's and
/// the image file's (Publisher, Look up on VirusTotal).
///
/// Each open is recorded (the name, the object attributes, the security asked for, the access, sharing, disposition
/// and options), and answered as the state says: a failure status, or a handle with the attributes, reparse tag,
/// device type and characteristics set here. With forward set, each call goes on to the real one after it is
/// recorded, so a caller that reads the file (a hash, an identity) gets the real file, and the state's answers are
/// left to the real system; otherwise the handle is an event, a real handle that its owner closes and that can't be
/// read.

#pragma once

#ifdef _WIN32

#include "Platform/Windows/WindowsLocalNtFile.h"

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
#include <winternl.h> // OBJECT_ATTRIBUTES
// clang-format on

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace TestMocks
{

/// What one NtCreateFile call was asked.
struct LocalNtOpen
{
    std::wstring name;
    ULONG objectAttributes = 0;
    bool rooted = false;           ///< a RootDirectory was given (a relative open)
    bool qualityOfService = false; ///< a SECURITY_QUALITY_OF_SERVICE was given
    SECURITY_IMPERSONATION_LEVEL impersonation = SecurityDelegation;
    BOOLEAN effectiveOnly = FALSE;
    ACCESS_MASK access = 0;
    ULONG share = 0;
    ULONG disposition = 0;
    ULONG options = 0;
};

/// The fake's state: the answers, and every open made.
struct FakeLocalNtFileState
{
    bool forward = false; ///< after recording, call the real functions
    NTSTATUS openStatus = 0;
    DWORD attributes = FILE_ATTRIBUTE_ARCHIVE;
    DWORD reparseTag = 0;
    DWORD deviceType = FILE_DEVICE_DISK;
    ULONG characteristics = 0;
    bool remote = false; ///< FileRemoteProtocolInfo answers, as only a redirector does
    std::vector<LocalNtOpen> opens;
};

[[nodiscard]] inline FakeLocalNtFileState& localNtFile()
{
    static FakeLocalNtFileState instance;
    return instance;
}

inline NTSTATUS NTAPI fakeNtCreateFile(PHANDLE handle,
                                       ACCESS_MASK access,
                                       POBJECT_ATTRIBUTES attributes,
                                       PIO_STATUS_BLOCK status,
                                       PLARGE_INTEGER allocationSize,
                                       ULONG fileAttributes,
                                       ULONG share,
                                       ULONG disposition,
                                       ULONG options,
                                       PVOID eaBuffer,
                                       ULONG eaLength)
{
    FakeLocalNtFileState& state = localNtFile();
    LocalNtOpen open{};
    open.name.assign(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    open.objectAttributes = attributes->Attributes;
    open.rooted = attributes->RootDirectory != nullptr;
    if (attributes->SecurityQualityOfService != nullptr)
    {
        const auto* qos = static_cast<const SECURITY_QUALITY_OF_SERVICE*>(attributes->SecurityQualityOfService);
        open.qualityOfService = true;
        open.impersonation = qos->ImpersonationLevel;
        open.effectiveOnly = qos->EffectiveOnly;
    }
    open.access = access;
    open.share = share;
    open.disposition = disposition;
    open.options = options;
    state.opens.push_back(std::move(open));
    if (state.forward)
    {
        return Platform::Windows::systemLocalFileFunctions().ntCreateFile(
            handle, access, attributes, status, allocationSize, fileAttributes, share, disposition, options, eaBuffer, eaLength);
    }
    if (state.openStatus < 0)
    {
        return state.openStatus;
    }
    *handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // a real handle, so its owner closes something real
    return 0;
}

inline NTSTATUS NTAPI fakeNtQueryVolumeInformationFile(HANDLE file, PIO_STATUS_BLOCK status, PVOID information, ULONG length, ULONG what)
{
    if (localNtFile().forward)
    {
        return Platform::Windows::systemLocalFileFunctions().ntQueryVolumeInformationFile(file, status, information, length, what);
    }
    EXPECT_EQ(what, 4U); // FileFsDeviceInformation
    EXPECT_GE(length, 2 * sizeof(ULONG));
    const std::array<ULONG, 2> device{localNtFile().deviceType, localNtFile().characteristics};
    std::memcpy(information, device.data(), sizeof(device));
    return 0;
}

inline BOOL WINAPI fakeGetFileInformationByHandleEx(HANDLE file, FILE_INFO_BY_HANDLE_CLASS what, LPVOID information, DWORD bytes)
{
    if (localNtFile().forward)
    {
        return GetFileInformationByHandleEx(file, what, information, bytes);
    }
    if (what == FileAttributeTagInfo)
    {
        auto* tag = static_cast<FILE_ATTRIBUTE_TAG_INFO*>(information);
        tag->FileAttributes = localNtFile().attributes;
        tag->ReparseTag = localNtFile().reparseTag;
        return TRUE;
    }
    EXPECT_EQ(what, FileRemoteProtocolInfo);
    return localNtFile().remote ? TRUE : FALSE;
}

/// The fake's calls.
inline constexpr Platform::Windows::LocalFileFunctions FAKE_LOCAL_NT_FILE{
    .ntCreateFile = &fakeNtCreateFile,
    .ntQueryVolumeInformationFile = &fakeNtQueryVolumeInformationFile,
    .getFileInformationByHandleEx = &fakeGetFileInformationByHandleEx,
};

/// OBJ_DONT_REPARSE, which user-mode headers don't define.
inline constexpr ULONG OBJ_DONT_REPARSE_FLAG = 0x00001000U;

} // namespace TestMocks

#endif // _WIN32
