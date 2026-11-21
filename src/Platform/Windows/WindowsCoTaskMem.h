#pragma once

// The owner for memory COM and the shell allocate for their caller with CoTaskMemAlloc (#4367): a known
// folder's path (SHGetKnownFolderPath), a shell item's name (IShellItem::GetDisplayName). Its own header,
// not WindowsHandles.h, so objbase.h stays out of everything that includes that one, TaskSmackHelper.exe's
// sources among them.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h> // CoTaskMemFree
// clang-format on

#include "WindowsHandles.h"

#include <memory>

namespace Platform::Windows
{

/// Memory a call CoTaskMemAlloc'd for its caller, CoTaskMemFree'd once. Fill one from an out-parameter
/// with std::out_ptr; one the call left null is never freed (CoTaskMemFree would ignore it anyway).
template<typename T> using UniqueCoTaskMem = std::unique_ptr<T, FreeWith<&::CoTaskMemFree>>;

} // namespace Platform::Windows

#endif // _WIN32
