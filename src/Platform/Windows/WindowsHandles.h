#pragma once

// The Windows platform layer's owners for raw Win32 handles (#1183): one kernel-HANDLE owner, one
// SC_HANDLE owner (#800) and one module owner, shared by every probe, instead of a hand-rolled wrapper per file.
// And the owners for memory and handles a call hands back that need nothing past <windows.h> (#4367): LocalAlloc'd
// memory, the environment block, WLAN handles and memory, and NetApi buffers. Owners whose release lives in a
// heavier header sit beside their users instead: WindowsCoTaskMem.h, WindowsDevInfo.h, WindowsEventLogHandle.h
// and WindowsPdhQuery.h, so this one stays cheap for everything that includes it, TaskSmackHelper.exe's sources too.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace Platform::Windows
{

/// Closes kernel object handles (processes, threads, tokens, files, devices) with CloseHandle.
///
/// Win32 has two failure sentinels for these: OpenProcess, OpenThread and OpenProcessToken return
/// null, while CreateFile returns INVALID_HANDLE_VALUE. Both count as "no handle", so whichever API
/// produced the value, an owner never passes a failure result to CloseHandle.
struct KernelHandleTraits
{
    using Type = HANDLE;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type handle) noexcept
    {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    static void close(Type handle) noexcept
    {
        CloseHandle(handle);
    }
};

/// Owns one handle, closing it exactly once on destruction, reset or move-assignment.
/// Move-only, with no-throw moves. `Traits` supplies the handle type, its "no handle" value, the
/// validity test and the close call; tests substitute a counting close.
template<typename Traits> class UniqueResource
{
  public:
    using Type = Traits::Type;

    UniqueResource() noexcept = default;
    explicit UniqueResource(Type value) noexcept : m_Value(value)
    {}

    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;

    UniqueResource(UniqueResource&& other) noexcept : m_Value(other.release())
    {}

    UniqueResource& operator=(UniqueResource&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    ~UniqueResource() noexcept
    {
        reset();
    }

    /// The handle, still owned by this object; may be a failure sentinel.
    [[nodiscard]] Type get() const noexcept
    {
        return m_Value;
    }

    /// Whether this object holds a handle it will close (neither failure sentinel).
    [[nodiscard]] bool valid() const noexcept
    {
        return Traits::isValid(m_Value);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }

    /// Give up ownership without closing; the caller must close the returned handle.
    [[nodiscard]] Type release() noexcept
    {
        return std::exchange(m_Value, Traits::invalid());
    }

    /// Close the current handle, if valid, and take ownership of `value`.
    void reset(Type value = Traits::invalid()) noexcept
    {
        const Type old = std::exchange(m_Value, value);
        if (Traits::isValid(old))
        {
            Traits::close(old);
        }
    }

    /// For APIs that return the handle through an out-parameter (e.g. OpenProcessToken's PHANDLE):
    /// closes any handle already held, then returns the address to write the new one into.
    [[nodiscard]] Type* put() noexcept
    {
        reset();
        return &m_Value;
    }

  private:
    Type m_Value = Traits::invalid();
};

/// An owned kernel object handle. Null and INVALID_HANDLE_VALUE are both "no handle".
using UniqueHandle = UniqueResource<KernelHandleTraits>;

/// Closes a kernel handle through an injected CloseHandle: kernel32's, or a test's fake for the
/// handles its fake OpenProcess hands out, so a fake handle never reaches the real one.
struct InjectedHandleCloser
{
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

    CloseHandleFn close = &::CloseHandle;

    void operator()(HANDLE handle) const noexcept
    {
        close(handle);
    }
};

/// An owned kernel handle closed through InjectedHandleCloser. Null is "no handle"; for handles from
/// calls that fail with null (OpenProcess, DuplicateHandle's out-parameter), not CreateFile's
/// INVALID_HANDLE_VALUE.
using InjectableHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, InjectedHandleCloser>;

/// Opens \\.\PhysicalDrive@p index query-only (#763): desired access 0, so no administrator rights are
/// needed and the handle can't read or write the disk, only answer IOCTLs that need no access
/// (IOCTL_DISK_PERFORMANCE, IOCTL_STORAGE_QUERY_PROPERTY, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
/// IOCTL_DISK_GET_DRIVE_LAYOUT_EX). Empty when
/// there is no such drive or it can't be opened; GetLastError() says why.
[[nodiscard]] inline UniqueHandle openPhysicalDriveQueryOnly(int index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
    return UniqueHandle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
}

/// Opens \\.\PhysicalDrive@p index for reading and writing (#2588): what IOCTL_ATA_PASS_THROUGH needs
/// (its code asks for FILE_READ_ACCESS | FILE_WRITE_ACCESS), which only an administrator gets. The Storage
/// section's ATA SMART read is the one caller; a standard user's open fails with ERROR_ACCESS_DENIED and
/// the row says so. Empty when there is no such drive or it can't be opened; GetLastError() says why.
[[nodiscard]] inline UniqueHandle openPhysicalDriveReadWrite(int index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
    return UniqueHandle(
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
}

/// Opens the volume on drive @p letter (\\.\C:) query-only (#1632), like openPhysicalDriveQueryOnly():
/// desired access 0, for IOCTL_STORAGE_GET_DEVICE_NUMBER. Empty when it can't be opened.
[[nodiscard]] inline UniqueHandle openVolumeQueryOnly(wchar_t letter)
{
    const std::wstring path = std::wstring(L"\\\\.\\") + letter + L':';
    return UniqueHandle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
}

/// Closes Service Control Manager and service handles (OpenSCManagerW, OpenServiceW) with
/// CloseServiceHandle (#800). Both report failure as null.
struct ServiceHandleTraits
{
    using Type = SC_HANDLE;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type handle) noexcept
    {
        return handle != nullptr;
    }

    static void close(Type handle) noexcept
    {
        CloseServiceHandle(handle);
    }
};

/// An owned SC_HANDLE.
using UniqueServiceHandle = UniqueResource<ServiceHandleTraits>;

/// Closes an SC_HANDLE through an injected CloseServiceHandle: advapi32's, or a test's fake for the
/// handles its fake OpenServiceW hands out (WindowsServiceConfig.h, WindowsServiceActions.h).
struct InjectedServiceHandleCloser
{
    BOOL(WINAPI* close)(SC_HANDLE) = &::CloseServiceHandle;

    void operator()(SC_HANDLE handle) const noexcept
    {
        close(handle);
    }
};

/// An owned SC_HANDLE closed through InjectedServiceHandleCloser.
using InjectableServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, InjectedServiceHandleCloser>;

/// Calls FreeLibrary on a module LoadLibrary returned.
struct ModuleDeleter
{
    void operator()(HMODULE module) const noexcept
    {
        FreeLibrary(module);
    }
};

/// A loaded module, freed when it goes out of scope, so no exit path can leak the reference.
/// LoadLibrary's only failure value is null, which unique_ptr never passes to its deleter.
using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

/// Closes registry keys RegOpenKeyExW opened, with RegCloseKey (#801). Failure leaves the out-
/// parameter untouched, so null is "no key"; the predefined roots (HKEY_CURRENT_USER, ...) are never
/// owned.
struct RegistryKeyTraits
{
    using Type = HKEY;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type key) noexcept
    {
        return key != nullptr;
    }

    static void close(Type key) noexcept
    {
        RegCloseKey(key);
    }
};

/// An owned, opened registry key.
using UniqueRegistryKey = UniqueResource<RegistryKeyTraits>;

/// Gives memory back through the C function @p Free (#4367). A template parameter, so each owner below
/// names its own and a test can substitute a counting one; unique_ptr never calls it with null.
template<auto Free> struct FreeWith
{
    template<typename T> void operator()(T* memory) const noexcept
    {
        if constexpr (std::is_pointer_v<T>)
        {
            // An array of pointers (CommandLineToArgvW's) is one block: it goes back as the void* it was allocated as.
            static_cast<void>(Free(static_cast<void*>(memory)));
        }
        else
        {
            static_cast<void>(Free(memory));
        }
    }
};

/// Memory a call LocalAlloc'd for its caller (CommandLineToArgvW's array, among others), LocalFree'd once.
template<typename T> using UniqueLocal = std::unique_ptr<T, FreeWith<&::LocalFree>>;

/// The environment block GetEnvironmentStringsW returned, given back with FreeEnvironmentStringsW. Null,
/// the call's failure, is never passed to it.
using UniqueEnvironmentStrings = std::unique_ptr<wchar_t, FreeWith<&::FreeEnvironmentStringsW>>;

/// Closes a WLAN client handle (WlanOpenHandle's) through the injected WlanCloseHandle: wlanapi.dll's,
/// resolved at run time, or a test's fake.
struct WlanHandleCloser
{
    using CloseFn = DWORD(WINAPI*)(HANDLE client, PVOID reserved);

    CloseFn close = nullptr;

    void operator()(HANDLE client) const noexcept
    {
        static_cast<void>(close(client, nullptr));
    }
};

/// An owned WLAN client handle. Null is "no handle".
using UniqueWlanHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, WlanHandleCloser>;

/// The injected WlanFreeMemory: wlanapi.dll's, or a test's fake.
using WlanFreeMemoryFn = VOID(WINAPI*)(PVOID memory);

/// Memory a WLAN call allocated (WlanEnumInterfaces' list, WlanGetNetworkBssList's), freed once with the
/// injected WlanFreeMemory.
template<typename T> using UniqueWlanMemory = std::unique_ptr<T, WlanFreeMemoryFn>;

/// A buffer a NetApi call allocated (NetGetJoinInformation's name), given back once with the injected
/// NetApiBufferFree. Unlike unique_ptr, it passes a null buffer on too: a call that succeeded owes the
/// free whatever it wrote, and NetApiBufferFree accepts null. A default or moved-from owner frees nothing.
template<typename T> class UniqueNetApiBuffer
{
  public:
    /// NetApiBufferFree's type, spelled without <lm.h> (NET_API_STATUS is DWORD).
    using FreeFn = DWORD(WINAPI*)(LPVOID buffer);

    UniqueNetApiBuffer() noexcept = default;
    UniqueNetApiBuffer(T* buffer, FreeFn freeBuffer) noexcept : m_Buffer(buffer), m_Free(freeBuffer)
    {}

    UniqueNetApiBuffer(const UniqueNetApiBuffer&) = delete;
    UniqueNetApiBuffer& operator=(const UniqueNetApiBuffer&) = delete;

    UniqueNetApiBuffer(UniqueNetApiBuffer&& other) noexcept
        : m_Buffer(std::exchange(other.m_Buffer, nullptr)), m_Free(std::exchange(other.m_Free, nullptr))
    {}

    UniqueNetApiBuffer& operator=(UniqueNetApiBuffer&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Buffer = std::exchange(other.m_Buffer, nullptr);
            m_Free = std::exchange(other.m_Free, nullptr);
        }
        return *this;
    }

    ~UniqueNetApiBuffer() noexcept
    {
        reset();
    }

    /// The buffer, still owned by this object; may be null.
    [[nodiscard]] T* get() const noexcept
    {
        return m_Buffer;
    }

    /// Whether this object will free its buffer (null or not) when it goes.
    [[nodiscard]] bool owns() const noexcept
    {
        return m_Free != nullptr;
    }

    /// Free the buffer now, if this object owns one, and own nothing.
    void reset() noexcept
    {
        auto* const freeBuffer = std::exchange(m_Free, nullptr);
        T* const buffer = std::exchange(m_Buffer, nullptr);
        if (freeBuffer != nullptr)
        {
            static_cast<void>(freeBuffer(buffer));
        }
    }

  private:
    T* m_Buffer = nullptr;
    FreeFn m_Free = nullptr;
};

static_assert(std::is_nothrow_move_constructible_v<UniqueHandle> && std::is_nothrow_move_assignable_v<UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<UniqueModule> && std::is_nothrow_move_assignable_v<UniqueModule>);
static_assert(std::is_nothrow_move_constructible_v<UniqueNetApiBuffer<wchar_t>> &&
              std::is_nothrow_move_assignable_v<UniqueNetApiBuffer<wchar_t>>);

} // namespace Platform::Windows

#endif // _WIN32
