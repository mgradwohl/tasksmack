/// @file test_WindowsTaskManagerReplacement.cpp
/// @brief Replace Task Manager on Windows (#3072): WindowsIfeoRegistry's calls against a scratch key under
/// HKEY_CURRENT_USER\Software\TaskSmack.Tests (never the real taskmgr.exe IFEO key, HKLM's or HKCU's), and
/// what TaskSmackHelper.exe does with a request: only the TaskSmack.exe beside it, by name and by file.

#include "Mocks/FakeIfeoRegistry.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WinString.h"
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

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>

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

TEST(TaskManagerHelperRequestTest, InstallFoldersAreProgramFiles)
{
    const auto folders = installFolders();
    ASSERT_FALSE(folders.empty()) << "%ProgramFiles% is always set";
    for (const std::string& folder : folders)
    {
        EXPECT_FALSE(folder.empty());
    }
}

} // namespace
} // namespace Platform::Windows
