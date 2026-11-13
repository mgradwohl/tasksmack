#include "UnelevatedShell.h"

#include "ComPtr.h"
#include "ComScope.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <objbase.h>   // NOLINT(misc-include-cleaner) - CoCreateInstance, CoAllowSetForegroundWindow
#include <oleauto.h>   // NOLINT(misc-include-cleaner) - SysAllocString, SysFreeString, VariantInit
#include <exdisp.h>    // NOLINT(misc-include-cleaner) - IShellWindows, CLSID_ShellWindows, SWC_DESKTOP, SWFO_NEEDDISPATCH
#include <shldisp.h>   // NOLINT(misc-include-cleaner) - IShellFolderViewDual, IShellDispatch2
#include <shobjidl_core.h> // NOLINT(misc-include-cleaner) - IShellBrowser, IShellView, SVGIO_BACKGROUND
#include <shlguid.h>   // NOLINT(misc-include-cleaner) - SID_STopLevelBrowser
#include <servprov.h>  // NOLINT(misc-include-cleaner) - IServiceProvider
// clang-format on

#include <format>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib") // CLSID_ShellWindows, the shell IIDs and SID_STopLevelBrowser

namespace Platform::Windows::UnelevatedShell
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 and COM types come from the sub-headers above
namespace
{

[[nodiscard]] std::string failure(const char* step, HRESULT hr)
{
    return std::format("{} failed (0x{:08X})", step, static_cast<unsigned long>(hr));
}

/// A BSTR freed at the end of its scope.
class Bstr
{
  public:
    explicit Bstr(const wchar_t* text) noexcept : m_Value(SysAllocString(text))
    {}
    ~Bstr()
    {
        SysFreeString(m_Value);
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    Bstr(Bstr&&) = delete;
    Bstr& operator=(Bstr&&) = delete;

    [[nodiscard]] BSTR get() const noexcept
    {
        return m_Value;
    }

  private:
    BSTR m_Value;
};

/// An empty VARIANT (VT_EMPTY): ShellExecute's arguments left out.
[[nodiscard]] VARIANT emptyVariant() noexcept
{
    VARIANT value{};
    VariantInit(&value);
    return value;
}

// LCOV_EXCL_START - asks Explorer to start the default handler (a browser, editor or Explorer window); no test may (#2039)
/// open() and run(): IShellDispatch2::ShellExecute of @p target with @p arguments (none when empty), in Explorer.
[[nodiscard]] std::expected<void, std::string> shellExecute(const std::wstring& target, const std::wstring& arguments)
{
    // On the open worker, in an STA for the calls, as the shell's objects ask of their callers.
    const ComScope com;
    if (!com.usable())
    {
        return std::unexpected(std::string("COM couldn't be used on this thread"));
    }

    // Explorer's window list: a local server, Explorer itself. None when Explorer isn't the shell or isn't running.
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(windows.releaseAndGetAddressOf()));
    if (FAILED(hr) || !windows)
    {
        return std::unexpected(std::format("Explorer isn't running ({})", failure("CoCreateInstance(ShellWindows)", hr)));
    }

    // The desktop's window, with its automation object (S_FALSE: there is no desktop window).
    VARIANT location = emptyVariant();
    VARIANT root = emptyVariant();
    long desktopWindow = 0; // FindWindowSW's out parameter is a long
    ComPtr<IDispatch> desktopDispatch;
    hr = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopWindow, SWFO_NEEDDISPATCH, desktopDispatch.releaseAndGetAddressOf());
    if (hr != S_OK || !desktopDispatch)
    {
        return std::unexpected(std::format("Explorer's desktop isn't there ({})", failure("FindWindowSW(SWC_DESKTOP)", hr)));
    }

    ComPtr<IServiceProvider> provider;
    hr = desktopDispatch->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(provider.releaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(failure("QueryInterface(IServiceProvider)", hr));
    }
    ComPtr<IShellBrowser> browser;
    hr = provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(browser.releaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(failure("QueryService(SID_STopLevelBrowser)", hr));
    }
    ComPtr<IShellView> view;
    hr = browser->QueryActiveShellView(view.releaseAndGetAddressOf());
    if (FAILED(hr) || !view)
    {
        return std::unexpected(failure("QueryActiveShellView", hr));
    }

    // The desktop folder view's automation object, and through it Explorer's Shell.Application.
    ComPtr<IDispatch> background;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(background.releaseAndGetAddressOf()));
    if (FAILED(hr) || !background)
    {
        return std::unexpected(failure("GetItemObject(SVGIO_BACKGROUND)", hr));
    }
    ComPtr<IShellFolderViewDual> folderView;
    hr = background->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(folderView.releaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(failure("QueryInterface(IShellFolderViewDual)", hr));
    }
    ComPtr<IDispatch> application;
    hr = folderView->get_Application(application.releaseAndGetAddressOf());
    if (FAILED(hr) || !application)
    {
        return std::unexpected(failure("IShellFolderViewDual::get_Application", hr));
    }
    ComPtr<IShellDispatch2> shell;
    hr = application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(shell.releaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(failure("QueryInterface(IShellDispatch2)", hr));
    }

    // Explorer may bring the window it starts to the front: TaskSmack has the foreground, the user's click.
    static_cast<void>(CoAllowSetForegroundWindow(shell.get(), nullptr));

    const Bstr file(target.c_str());
    if (file.get() == nullptr)
    {
        return std::unexpected(std::string("out of memory"));
    }
    const Bstr verb(L"open");
    // VARIANT is the union COM's automation calls take: its type tag and the matching member are set together.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    VARIANT operation = emptyVariant();
    operation.vt = VT_BSTR;
    operation.bstrVal = verb.get(); // owned by verb, freed after the call
    VARIANT show = emptyVariant();
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    const Bstr parameters(arguments.c_str());
    if (!arguments.empty() && parameters.get() == nullptr)
    {
        return std::unexpected(std::string("out of memory"));
    }
    VARIANT argumentsVariant = emptyVariant();
    if (!arguments.empty())
    {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
        argumentsVariant.vt = VT_BSTR;
        argumentsVariant.bstrVal = parameters.get(); // owned by parameters, freed after the call
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    }
    const VARIANT directory = emptyVariant();
    hr = shell->ShellExecute(file.get(), argumentsVariant, directory, operation, show);
    if (FAILED(hr))
    {
        return std::unexpected(failure("IShellDispatch2::ShellExecute", hr));
    }
    return {};
}
// LCOV_EXCL_STOP

} // namespace

// LCOV_EXCL_START - each asks Explorer to start a program; no test may (#2039)
std::expected<void, std::string> open(const std::wstring& target)
{
    return shellExecute(target, {});
}

std::expected<void, std::string> run(const std::wstring& file, const std::wstring& arguments)
{
    return shellExecute(file, arguments);
}
// LCOV_EXCL_STOP

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows::UnelevatedShell
