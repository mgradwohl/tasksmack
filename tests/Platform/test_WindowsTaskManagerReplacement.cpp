/// @file test_WindowsTaskManagerReplacement.cpp
/// @brief Replace Task Manager on Windows (#3072): WindowsIfeoRegistry's calls against a scratch key under
/// HKEY_CURRENT_USER\Software\TaskSmack.Tests (never the real taskmgr.exe IFEO key, HKLM's or HKCU's), and
/// what TaskSmackHelper.exe does with a request: only the TaskSmack.exe beside it, by name and by file. And
/// when an elevated TaskSmack may write the key itself (#3280): Program Files from the known folders, and
/// only for a copy no one but an administrator can change, the helper's check.

#include "Mocks/FakeIfeoRegistry.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsTaskManagerInstall.h"
#include "Platform/Windows/WindowsTaskManagerReplacement.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <knownfolders.h>
#include <objbase.h> // CoTaskMemAlloc
#include <shlobj.h>

namespace Platform::Windows
{
namespace
{

namespace TMR = Platform::TaskManagerReplacement;

/// A key of the test's own under HKEY_CURRENT_USER\Software\TaskSmack.Tests, deleted with everything in it
/// when the test ends. Its taskmgr.exe subkey stands in for the IFEO one.
class ScratchKey
{
  public:
    ScratchKey()
    {
        std::random_device random;
        m_Root = std::format(L"Software\\TaskSmack.Tests\\ifeo-{}-{}", GetCurrentProcessId(), random());
    }
    ~ScratchKey()
    {
        static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, m_Root.c_str()));
        static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, m_Root.c_str()));
    }
    ScratchKey(const ScratchKey&) = delete;
    ScratchKey& operator=(const ScratchKey&) = delete;
    ScratchKey(ScratchKey&&) = delete;
    ScratchKey& operator=(ScratchKey&&) = delete;

    [[nodiscard]] std::wstring taskmgr() const
    {
        return m_Root + L"\\taskmgr.exe";
    }

  private:
    std::wstring m_Root;
};

[[nodiscard]] bool keyExists(const std::wstring& subKey)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status == ERROR_SUCCESS)
    {
        RegCloseKey(key);
    }
    return status == ERROR_SUCCESS;
}

constexpr std::string_view INSTALLED = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";

TEST(WindowsIfeoRegistryTest, AKeyThatIsntThereReadsAsOff)
{
    const ScratchKey scratch;
    WindowsIfeoRegistry registry(HKEY_CURRENT_USER, scratch.taskmgr());
    const auto debugger = registry.readDebugger();
    ASSERT_TRUE(debugger.has_value()) << debugger.error();
    EXPECT_FALSE(debugger->has_value());
    const auto marked = registry.readCreatedMarker();
    ASSERT_TRUE(marked.has_value());
    EXPECT_FALSE(*marked);
    EXPECT_TRUE(registry.deleteDebugger().has_value()) << "nothing to remove is no error";
    EXPECT_TRUE(registry.deleteCreatedMarker().has_value());
    const auto deleted = registry.deleteKeyIfEmpty();
    ASSERT_TRUE(deleted.has_value());
    EXPECT_FALSE(*deleted);
    EXPECT_FALSE(registry.writeDebugger("x").has_value()) << "no key to write in";
}

TEST(WindowsIfeoRegistryTest, OnAndOffRoundTripThroughTheRegistry)
{
    const ScratchKey scratch;
    WindowsIfeoRegistry registry(HKEY_CURRENT_USER, scratch.taskmgr());
    const ProcessActionResult on = TMR::applySet(registry, INSTALLED, false);
    ASSERT_TRUE(on.success) << on.errorMessage;
    const auto debugger = registry.readDebugger();
    ASSERT_TRUE(debugger.has_value());
    EXPECT_EQ(debugger->value_or(""), TMR::debuggerFor(INSTALLED));
    EXPECT_EQ(registry.readCreatedMarker().value_or(false), true);
    EXPECT_EQ(TMR::classify(*debugger, INSTALLED).state, TMR::State::ThisCopy);

    const ProcessActionResult off = TMR::applyClear(registry);
    ASSERT_TRUE(off.success) << off.errorMessage;
    EXPECT_FALSE(keyExists(scratch.taskmgr())) << "TaskSmack made it, and it is empty";
}

TEST(WindowsIfeoRegistryTest, AnotherToolsKeyAndValuesStay)
{
    const ScratchKey scratch;
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                              scratch.taskmgr().c_str(),
                              0,
                              nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_SET_VALUE | KEY_WOW64_64KEY,
                              nullptr,
                              &key,
                              nullptr),
              ERROR_SUCCESS);
    const DWORD flag = 0x200;
    ASSERT_EQ(RegSetValueExW(key, L"GlobalFlag", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&flag), sizeof(flag)), ERROR_SUCCESS);
    const std::wstring procexp = LR"(C:\Tools\procexp64.exe)";
    ASSERT_EQ(RegSetValueExW(key,
                             L"Debugger",
                             0,
                             REG_EXPAND_SZ,
                             reinterpret_cast<const BYTE*>(procexp.c_str()),
                             static_cast<DWORD>((procexp.size() + 1) * sizeof(wchar_t))),
              ERROR_SUCCESS);
    RegCloseKey(key);

    WindowsIfeoRegistry registry(HKEY_CURRENT_USER, scratch.taskmgr());
    EXPECT_EQ(registry.readDebugger().value_or(std::nullopt).value_or(""), R"(C:\Tools\procexp64.exe)") << "REG_EXPAND_SZ read as written";
    EXPECT_FALSE(TMR::applySet(registry, INSTALLED, false).success) << "another program's, unconfirmed";
    EXPECT_FALSE(TMR::applyClear(registry).success) << "another program's, never removed";
    ASSERT_TRUE(TMR::applySet(registry, INSTALLED, true).success);
    EXPECT_FALSE(registry.readCreatedMarker().value_or(true)) << "not TaskSmack's key";
    ASSERT_TRUE(TMR::applyClear(registry).success);
    EXPECT_TRUE(keyExists(scratch.taskmgr())) << "GlobalFlag is still in it";
    EXPECT_FALSE(registry.readDebugger().value_or(std::string("?")).has_value());
}

TEST(WindowsIfeoRegistryTest, ADebuggerThatIsntTextIsAnotherProgram)
{
    const ScratchKey scratch;
    WindowsIfeoRegistry registry(HKEY_CURRENT_USER, scratch.taskmgr());
    ASSERT_TRUE(registry.createKey().value_or(false));
    HKEY key = nullptr;
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, scratch.taskmgr().c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key), ERROR_SUCCESS);
    const DWORD number = 1;
    ASSERT_EQ(RegSetValueExW(key, L"Debugger", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&number), sizeof(number)), ERROR_SUCCESS);
    RegCloseKey(key);
    const auto debugger = registry.readDebugger();
    ASSERT_TRUE(debugger.has_value());
    ASSERT_TRUE(debugger->has_value());
    EXPECT_EQ(TMR::classify(*debugger, INSTALLED).state, TMR::State::OtherProgram);
    EXPECT_FALSE(registry.createKey().value_or(true)) << "already there";
}

// ---------------------------------------------------------------------------------------------------------
// handleHelperRequest(): the helper's validation
// ---------------------------------------------------------------------------------------------------------

bool alwaysSame(const std::wstring& /*first*/, const std::wstring& /*second*/)
{
    return true;
}
bool neverSame(const std::wstring& /*first*/, const std::wstring& /*second*/)
{
    return false;
}

[[nodiscard]] Broker::TaskManagerReplacementRequest setTo(std::string path, bool replaceOther = false)
{
    return {.change = Broker::TaskManagerChange::Set, .path = std::move(path), .replaceOther = replaceOther};
}

TEST(TaskManagerHelperRequestTest, SetsOnlyTheTaskSmackBesideTheHelper)
{
    const std::wstring beside = WinString::utf8ToWide(INSTALLED);
    Mocks::FakeIfeoRegistry registry;
    const Broker::ActionResult on = handleHelperRequest(setTo(std::string(INSTALLED)), beside, registry, &alwaysSame);
    EXPECT_TRUE(on.success) << on.errorMessage;
    EXPECT_EQ(registry.debugger(), TMR::debuggerFor(INSTALLED));

    const Broker::ActionResult caseDiffers =
        handleHelperRequest(setTo(R"(c:\program files\tasksmack\tasksmack.exe)"), beside, registry, &alwaysSame);
    EXPECT_TRUE(caseDiffers.success) << "letters ignoring case, as NTFS";
}

TEST(TaskManagerHelperRequestTest, RefusesAnythingButThatFile)
{
    const std::wstring beside = WinString::utf8ToWide(INSTALLED);
    struct Refused
    {
        std::string path;
        std::wstring beside;
        SameFileFn sameFile;
        std::string_view why;
    };
    for (const Refused& r : {
             Refused{
                 .path = R"(C:\Users\me\TaskSmack.exe)", .beside = beside, .sameFile = &alwaysSame, .why = "another path, a link to it"},
             Refused{.path = std::string(INSTALLED), .beside = beside, .sameFile = &neverSame, .why = "the name, but not the file"},
             Refused{.path = std::string(INSTALLED), .beside = L"", .sameFile = &alwaysSame, .why = "no TaskSmack beside the helper"},
             Refused{.path = std::string(INSTALLED), .beside = beside, .sameFile = nullptr, .why = "no file check"},
             Refused{.path = R"(C:\Windows\System32\cmd.exe)",
                     .beside = L"C:\\Windows\\System32\\cmd.exe",
                     .sameFile = &alwaysSame,
                     .why = "not TaskSmack.exe"},
             Refused{.path = R"(\\server\share\TaskSmack.exe)",
                     .beside = L"\\\\server\\share\\TaskSmack.exe",
                     .sameFile = &alwaysSame,
                     .why = "UNC"},
             Refused{.path = R"(C:\TaskSmack.exe" C:\evil.exe ")", .beside = beside, .sameFile = &alwaysSame, .why = "quote injection"},
             Refused{.path = "TaskSmack.exe", .beside = L"TaskSmack.exe", .sameFile = &alwaysSame, .why = "relative"},
         })
    {
        Mocks::FakeIfeoRegistry registry;
        const Broker::ActionResult result = handleHelperRequest(setTo(r.path, true), r.beside, registry, r.sameFile);
        EXPECT_FALSE(result.success) << r.why;
        EXPECT_FALSE(result.errorMessage.empty()) << r.why;
        EXPECT_TRUE(registry.calls.empty()) << r.why << ": nothing read or written";
    }
}

TEST(TaskManagerHelperRequestTest, AClearRemovesOnlyATaskSmacksDebugger)
{
    const std::wstring beside = WinString::utf8ToWide(INSTALLED);
    const Broker::TaskManagerReplacementRequest clear{.change = Broker::TaskManagerChange::Clear, .path = {}, .replaceOther = false};
    Mocks::FakeIfeoRegistry procexp;
    Mocks::FakeIfeoRegistry::seed(procexp, R"(C:\Tools\procexp64.exe)");
    EXPECT_FALSE(handleHelperRequest(clear, beside, procexp, &alwaysSame).success);
    EXPECT_TRUE(procexp.debugger().has_value());

    Mocks::FakeIfeoRegistry mine;
    ASSERT_TRUE(handleHelperRequest(setTo(std::string(INSTALLED)), beside, mine, &alwaysSame).success);
    EXPECT_TRUE(handleHelperRequest(clear, beside, mine, &neverSame).success) << "a Clear names no file";
    EXPECT_FALSE(mine.keyExists);
}

TEST(TaskManagerHelperRequestTest, TheRealFileCheckTakesTheFileItself)
{
    std::error_code ec;
    std::random_device random;
    const std::filesystem::path folder = std::filesystem::temp_directory_path(ec) / std::format("tasksmack-ifeo-{}", random());
    ASSERT_TRUE(std::filesystem::create_directories(folder, ec)) << ec.message();
    const std::filesystem::path exe = folder / "TaskSmack.exe";
    {
        std::ofstream file(exe, std::ios::binary);
        file << "not really";
    }
    const std::string path = WinString::wideToUtf8(exe.wstring());
    Mocks::FakeIfeoRegistry registry;
    const Broker::ActionResult on = handleHelperRequest(setTo(path), exe.wstring(), registry, &BrokerHelper::sameFile);
    EXPECT_TRUE(on.success) << on.errorMessage;
    EXPECT_EQ(registry.debugger(), TMR::debuggerFor(path));

    std::filesystem::remove(exe, ec);
    Mocks::FakeIfeoRegistry gone;
    EXPECT_FALSE(handleHelperRequest(setTo(path), exe.wstring(), gone, &BrokerHelper::sameFile).success) << "no file there now";
    std::filesystem::remove_all(folder, ec);

    EXPECT_FALSE(fileExists(path));
    EXPECT_TRUE(fileExists(WinString::wideToUtf8(BrokerHelper::ownImage().value_or(L""))));
}

// #3280: an elevated TaskSmack writes the Debugger itself only for a copy no one but an administrator
// can change, Program Files read from the known folders, never from the environment. Nothing here is
// elevated or writes the IFEO key: fake known folders and checks, and the real check over a scratch
// folder under %TEMP% (this user's to change) and System32 (no one's but an administrator's).

constexpr std::wstring_view FAKE_PROGRAM_FILES = LR"(C:\Fake Program Files)";
constexpr std::wstring_view FAKE_PROGRAM_FILES_X86 = LR"(C:\Fake Program Files (x86))";

/// @p text in memory from CoTaskMemAlloc, as SHGetKnownFolderPath gives it; S_OK unless that fails.
[[nodiscard]] HRESULT knownFolderResult(std::wstring_view text, PWSTR* path)
{
    *path = static_cast<PWSTR>(CoTaskMemAlloc((text.size() + 1) * sizeof(wchar_t)));
    if (*path == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    const std::span<wchar_t> out(*path, text.size() + 1);
    std::ranges::copy(text, out.begin());
    out.back() = L'\0';
    return S_OK;
}

/// SHGetKnownFolderPath's stand-in: FAKE_PROGRAM_FILES for Program Files (both views), FAKE_PROGRAM_FILES_X86
/// for the x86 one, and nothing else.
HRESULT WINAPI fakeKnownFolder(REFKNOWNFOLDERID id, DWORD /*flags*/, HANDLE /*token*/, PWSTR* path)
{
    *path = nullptr;
    if (id == FOLDERID_ProgramFiles || id == FOLDERID_ProgramFilesX64)
    {
        return knownFolderResult(FAKE_PROGRAM_FILES, path);
    }
    if (id == FOLDERID_ProgramFilesX86)
    {
        return knownFolderResult(FAKE_PROGRAM_FILES_X86, path);
    }
    return E_INVALIDARG;
}

/// A SHGetKnownFolderPath that knows no folder.
HRESULT WINAPI noKnownFolder(REFKNOWNFOLDERID /*id*/, DWORD /*flags*/, HANDLE /*token*/, PWSTR* path)
{
    *path = nullptr;
    return E_FAIL;
}

/// This process's %ProgramFiles% set to @p value until the end of the scope, then put back.
class ProgramFilesVariable
{
  public:
    explicit ProgramFilesVariable(const std::wstring& value)
    {
        std::array<wchar_t, MAX_PATH> old{};
        const DWORD length = GetEnvironmentVariableW(NAME, old.data(), static_cast<DWORD>(old.size()));
        if (length > 0 && length < old.size())
        {
            m_Old = std::wstring(old.data(), length);
        }
        EXPECT_NE(SetEnvironmentVariableW(NAME, value.c_str()), FALSE);
    }
    ~ProgramFilesVariable()
    {
        static_cast<void>(SetEnvironmentVariableW(NAME, m_Old.has_value() ? m_Old->c_str() : nullptr));
    }
    ProgramFilesVariable(const ProgramFilesVariable&) = delete;
    ProgramFilesVariable& operator=(const ProgramFilesVariable&) = delete;
    ProgramFilesVariable(ProgramFilesVariable&&) = delete;
    ProgramFilesVariable& operator=(ProgramFilesVariable&&) = delete;

  private:
    static constexpr const wchar_t* NAME = L"ProgramFiles";
    std::optional<std::wstring> m_Old;
};

TEST(TaskManagerInstallTest, InstallFoldersAreTheKnownFoldersNotTheEnvironment)
{
    const std::filesystem::path planted = std::filesystem::temp_directory_path() / L"TaskSmack.Tests.NotProgramFiles";
    const ProgramFilesVariable variable(planted.wstring());
    const std::string plantedText = WinString::wideToUtf8(planted.wstring());

    const auto fake = installFolders(&fakeKnownFolder);
    const std::string programFiles = WinString::wideToUtf8(std::wstring(FAKE_PROGRAM_FILES));
    const std::string programFilesX86 = WinString::wideToUtf8(std::wstring(FAKE_PROGRAM_FILES_X86));
    EXPECT_EQ(fake, (std::vector<std::string>{programFiles, programFiles, programFilesX86})) << "each known folder, in order";
    EXPECT_TRUE(installFolders(&noKnownFolder).empty()) << "no known folder is no install folder, whatever %ProgramFiles% says";
    EXPECT_TRUE(installFolders(nullptr).empty());

    // The real known folders: Program Files, wherever %ProgramFiles% points this process.
    const auto real = installFolders();
    ASSERT_FALSE(real.empty()) << "Program Files is always known";
    for (const std::string& folder : real)
    {
        EXPECT_FALSE(folder.empty());
        EXPECT_FALSE(TMR::samePath(folder, plantedText)) << folder;
    }
    const std::string portable = WinString::wideToUtf8((planted / L"TaskSmack.exe").wstring());
    const std::vector<std::string_view> views(real.begin(), real.end());
    EXPECT_FALSE(TMR::isInstalledCopy(portable, views)) << "a copy under the planted %ProgramFiles% isn't installed";
}

/// The paths the fake checks were asked about, in order.
[[nodiscard]] std::vector<std::wstring>& checkedPaths()
{
    static std::vector<std::wstring> paths;
    return paths;
}

[[nodiscard]] std::expected<void, std::string> allowsAny(const std::wstring& exe)
{
    checkedPaths().push_back(exe);
    return {};
}

[[nodiscard]] std::expected<void, std::string> refusesAny(const std::wstring& exe)
{
    checkedPaths().push_back(exe);
    return std::unexpected(std::string("TaskSmack's user can change it"));
}

TEST(TaskManagerInstallTest, ADirectWriteNeedsAnInstalledCopyThatPassesTheCheck)
{
    const std::vector<std::string> folders = installFolders(&fakeKnownFolder);
    const std::string installed = WinString::wideToUtf8(std::wstring(FAKE_PROGRAM_FILES) + L"\\TaskSmack\\TaskSmack.exe");
    checkedPaths().clear();

    const auto portable = checkDirectWrite(R"(C:\Users\me\Downloads\TaskSmack\TaskSmack.exe)", folders, &allowsAny);
    ASSERT_FALSE(portable.has_value()) << "a copy outside Program Files";
    EXPECT_TRUE(portable.error().contains("isn't installed")) << portable.error();
    EXPECT_TRUE(checkedPaths().empty()) << "not even checked";

    const auto refused = checkDirectWrite(installed, folders, &refusesAny);
    ASSERT_FALSE(refused.has_value()) << "under Program Files isn't enough when the check refuses it";
    EXPECT_TRUE(refused.error().contains("can change")) << refused.error();
    ASSERT_EQ(checkedPaths().size(), 1U);
    EXPECT_EQ(checkedPaths().front(), WinString::utf8ToWide(installed)) << "the copy itself is checked";

    EXPECT_TRUE(checkDirectWrite(installed, folders, &allowsAny).has_value());
    EXPECT_FALSE(checkDirectWrite(installed, folders, nullptr).has_value()) << "no check is a refusal";
    EXPECT_FALSE(checkDirectWrite(installed, {}, &allowsAny).has_value()) << "no Program Files";
    checkedPaths().clear();
}

/// Why this test's user's folder rights aren't a standard install's, or nothing when they are: Windows
/// Sandbox's only user, the built-in WDAGUtilityAccount, isn't a user account the broker serves (#2618).
[[nodiscard]] std::optional<std::string> notAStandardUser()
{
    UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::nullopt;
    }
    const auto sid = BrokerPipe::userSidOfToken(token.get());
    if (!sid.has_value())
    {
        return std::nullopt;
    }
    const auto checked = BrokerPipe::checkUserSid(*sid);
    return checked.has_value() ? std::nullopt : std::optional<std::string>(checked.error());
}

[[nodiscard]] std::filesystem::path systemFolder()
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    return {std::wstring(system.data(), length)};
}

/// A folder of the test's own under %TEMP%, standing in for Program Files, with LaxApp\TaskSmack.exe in
/// it: a folder an installer let its users write. Deleted with everything in it at the end.
class WritableProgramFiles
{
  public:
    WritableProgramFiles()
    {
        std::random_device random;
        m_Root = std::filesystem::temp_directory_path() / std::format(L"TaskSmack.Tests.PF-{}-{}", GetCurrentProcessId(), random());
        std::error_code ec;
        std::filesystem::create_directories(m_Root / L"LaxApp", ec);
        EXPECT_FALSE(ec) << ec.message();
        const std::filesystem::path written = m_Root / L"LaxApp" / L"TaskSmack.exe";
        std::ofstream(written, std::ios::binary) << "not really";
        // Named as Windows names it, so a %TEMP% with an 8.3 short name (a runner's) is no link.
        m_Exe = BrokerHelper::finalPath(written.wstring()).value_or(written.wstring());
    }
    ~WritableProgramFiles()
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Root, ec);
        }
        catch (...) // NOLINT(bugprone-empty-catch): a scratch folder left behind fails no test
        {}
    }
    WritableProgramFiles(const WritableProgramFiles&) = delete;
    WritableProgramFiles& operator=(const WritableProgramFiles&) = delete;
    WritableProgramFiles(WritableProgramFiles&&) = delete;
    WritableProgramFiles& operator=(WritableProgramFiles&&) = delete;

    /// The stand-in for Program Files.
    [[nodiscard]] std::string root() const
    {
        return WinString::wideToUtf8(m_Exe.parent_path().parent_path().wstring());
    }
    [[nodiscard]] const std::filesystem::path& exe() const
    {
        return m_Exe;
    }

  private:
    std::filesystem::path m_Root;
    std::filesystem::path m_Exe;
};

TEST(TaskManagerInstallTest, ACopyInAProgramFilesFolderItsUserCanWriteIsRefused)
{
    const WritableProgramFiles programFiles;
    const std::vector<std::string> folders{programFiles.root()};
    const std::string self = WinString::wideToUtf8(programFiles.exe().wstring());
    const std::vector<std::string_view> views(folders.begin(), folders.end());
    ASSERT_TRUE(TMR::isInstalledCopy(self, views)) << "it passes the path check";
    const auto checked = checkDirectWrite(self, folders);
    ASSERT_FALSE(checked.has_value()) << "but not the check of who can change it";
    EXPECT_TRUE(checked.error().contains("can change")) << checked.error();
}

TEST(TaskManagerInstallTest, ACopyOnlyAnAdministratorCanChangeIsAllowed)
{
    if (const auto why = notAStandardUser())
    {
        GTEST_SKIP() << "this user's folder rights aren't a standard install's: " << *why;
    }
    // System32 stands in for Program Files\TaskSmack: no standard user may change cmd.exe, its folder or
    // any folder above it.
    const std::filesystem::path system = systemFolder();
    const std::vector<std::string> folders{WinString::wideToUtf8(system.parent_path().wstring())};
    const std::string self = WinString::wideToUtf8((system / L"cmd.exe").wstring());
    const auto checked = checkDirectWrite(self, folders);
    EXPECT_TRUE(checked.has_value()) << checked.error();

    // The same file named another way is refused: the path written must be the file checked.
    const std::string roundabout = WinString::wideToUtf8((system / L".." / system.filename() / L"cmd.exe").wstring());
    const auto other = checkDirectWrite(roundabout, folders);
    ASSERT_FALSE(other.has_value());
    EXPECT_TRUE(other.error().contains("is really")) << other.error();
}

TEST(TaskManagerInstallTest, TheElevatedRouteAndTheHelperReachTheSameVerdict)
{
    // The helper checks against its parent's token, and its parent is always an unelevated TaskSmack
    // (an elevated one writes the key itself). This process stands in for that parent, and the elevated
    // route checks against the token it would run unelevated with: the same user only while this
    // process is unelevated itself.
    const auto sameVerdict = [](const std::wstring& exe)
    {
        const std::array<std::wstring, 1> files{exe};
        const auto helper = BrokerHelper::checkUnchangeableByParent(GetCurrentProcess(), files);
        const auto elevated = checkUnchangeableByUnelevatedUser(exe);
        EXPECT_EQ(helper.has_value(), elevated.has_value())
            << WinString::wideToUtf8(exe) << ": the helper says " << (helper.has_value() ? "unchangeable" : helper.error())
            << ", the elevated route " << (elevated.has_value() ? "unchangeable" : elevated.error());
    };
    // A copy this user can change: an administrator can too, so both refuse it either way.
    const WritableProgramFiles programFiles;
    sameVerdict(programFiles.exe().wstring());
    if (isProcessElevated())
    {
        // An elevated administrator (a CI runner, UAC off) may change System32 itself, so this process
        // isn't a parent the helper would ever serve: there, the helper's verdict is an administrator's
        // and the elevated route's a standard user's. ACopyOnlyAnAdministratorCanChangeIsAllowed and
        // BrokerHelperTest.AnInstallInAFolderOnlyAdministratorsChangePasses check each on its own.
        GTEST_SKIP() << "this process runs elevated, and the helper serves only an unelevated TaskSmack";
    }
    sameVerdict((systemFolder() / L"cmd.exe").wstring());
}

TEST(TaskManagerInstallTest, TheUnelevatedTokenIsNoAdministrators)
{
    UniqueHandle own;
    ASSERT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, own.put()), FALSE);
    const auto token = unelevatedTokenOf(own.get());
    ASSERT_TRUE(token.has_value()) << token.error();
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators{};
    auto size = static_cast<DWORD>(administrators.size());
    ASSERT_NE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size), FALSE);
    BOOL member = TRUE;
    ASSERT_NE(CheckTokenMembership(token->get(), administrators.data(), &member), FALSE) << GetLastError();
    EXPECT_EQ(member, FALSE) << "Administrators is deny-only, or not there at all";
}

} // namespace
} // namespace Platform::Windows
