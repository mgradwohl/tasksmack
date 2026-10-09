#pragma once

// The Windows platform layer's owners for raw Win32 handles (#1183): one kernel-HANDLE owner, one
// SC_HANDLE owner (#800) and one module owner, shared by every probe, instead of a hand-rolled wrapper per file.

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

static_assert(std::is_nothrow_move_constructible_v<UniqueHandle> && std::is_nothrow_move_assignable_v<UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<UniqueModule> && std::is_nothrow_move_assignable_v<UniqueModule>);

} // namespace Platform::Windows

#endif // _WIN32
