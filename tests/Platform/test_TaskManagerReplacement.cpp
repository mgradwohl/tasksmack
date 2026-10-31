/// @file test_TaskManagerReplacement.cpp
/// @brief Replace Task Manager's logic (#3072), on every platform: the paths the helper takes and refuses,
/// how a Debugger value is read and sorted, the set and clear steps against a fake registry (another
/// program's Debugger never replaced without the user's confirmation, never removed; the key deleted only
/// when TaskSmack made it), and the service that reads and changes it through a fake helper. No test here
/// touches a real registry.

#include "Mocks/FakeBroker.h"
#include "Mocks/FakeIfeoRegistry.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/TaskManagerReplacementService.h"

#include <gtest/gtest.h>

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform::TaskManagerReplacement
{
namespace
{

constexpr std::string_view INSTALLED = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";

// ---------------------------------------------------------------------------------------------------------
// validateTaskSmackPath(): what the helper (and an elevated TaskSmack) will write
// ---------------------------------------------------------------------------------------------------------

TEST(TaskManagerReplacementPathTest, AcceptsAbsoluteDrivePathsToTaskSmackExe)
{
    for (const std::string_view path : {
             INSTALLED,
             std::string_view(R"(D:\Tools\TaskSmack.exe)"),
             std::string_view(R"(c:\program files\tasksmack\TASKSMACK.EXE)"), // letters ignoring case, as NTFS
             std::string_view(R"(C:\TaskSmack.exe)"),
             std::string_view(R"(C:\Users\Zoë\Apps\TaskSmack 1.0\TaskSmack.exe)"), // spaces and UTF-8 inside a folder
         })
    {
        EXPECT_TRUE(validateTaskSmackPath(path).has_value()) << path;
    }
}

struct RefusedPath
{
    std::string_view path;
    std::string_view why;
};

TEST(TaskManagerReplacementPathTest, RefusesEverythingElse)
{
    const std::string nul = std::string(R"(C:\TaskSmack)") + '\0' + R"(\TaskSmack.exe)";
    const std::string tooLong = R"(C:\)" + std::string(MAX_PATH_BYTES, 'a') + R"(\TaskSmack.exe)";
    const std::array refused{
        RefusedPath{.path = "", .why = "empty"},
        RefusedPath{.path = "TaskSmack.exe", .why = "relative"},
        RefusedPath{.path = R"(TaskSmack\TaskSmack.exe)", .why = "relative folder"},
        RefusedPath{.path = R"(\TaskSmack.exe)", .why = "rooted, no drive"},
        RefusedPath{.path = R"(C:TaskSmack.exe)", .why = "drive-relative"},
        RefusedPath{.path = R"(\\server\share\TaskSmack.exe)", .why = "UNC"},
        RefusedPath{.path = R"(\\?\C:\TaskSmack\TaskSmack.exe)", .why = "extended-length prefix"},
        RefusedPath{.path = R"(\\.\C:\TaskSmack\TaskSmack.exe)", .why = "device path"},
        RefusedPath{.path = R"(C:/Program Files/TaskSmack/TaskSmack.exe)", .why = "forward slashes"},
        RefusedPath{.path = R"(C:\Windows\System32\cmd.exe)", .why = "another program"},
        RefusedPath{.path = R"(C:\Tools\procexp64.exe)", .why = "another program"},
        RefusedPath{.path = R"(C:\TaskSmack\TaskSmack.exe.bat)", .why = "another extension"},
        RefusedPath{.path = R"(C:\TaskSmack\NotTaskSmack.exe)", .why = "another name ending the same"},
        RefusedPath{.path = R"(C:\TaskSmack\TaskSmack.exe.)", .why = "trailing dot Windows drops"},
        RefusedPath{.path = R"(C:\TaskSmack\TaskSmack.exe )", .why = "trailing space Windows drops"},
        RefusedPath{.path = R"(C:\TaskSmack.\TaskSmack.exe)", .why = "folder with a trailing dot"},
        RefusedPath{.path = R"(C:\TaskSmack\..\TaskSmack.exe)", .why = "parent folder"},
        RefusedPath{.path = R"(C:\.\TaskSmack.exe)", .why = "current folder"},
        RefusedPath{.path = R"(C:\TaskSmack\\TaskSmack.exe)", .why = "empty folder"},
        RefusedPath{.path = R"(C:\TaskSmack\)", .why = "a folder"},
        RefusedPath{.path = R"(C:\TaskSmack\TaskSmack.exe:evil.exe)", .why = "alternate data stream"},
        RefusedPath{.path = R"(C:\Evil.exe:TaskSmack.exe)", .why = "stream named TaskSmack.exe"},
        RefusedPath{.path = R"(C:\TaskSmack.exe" C:\evil.exe ")", .why = "a quote, to end the Debugger and add an argument"},
        RefusedPath{.path = R"("C:\TaskSmack\TaskSmack.exe")", .why = "quoted already"},
        RefusedPath{.path = R"(C:\TaskSmack\TaskSmack.exe & calc.exe)", .why = "a command after it (not TaskSmack.exe)"},
        RefusedPath{.path = R"(C:\a|b\TaskSmack.exe)", .why = "a pipe"},
        RefusedPath{.path = R"(C:\a<b\TaskSmack.exe)", .why = "a redirection"},
        RefusedPath{.path = R"(C:\%TEMP%\TaskSmack.exe)", .why = "an environment variable"},
        RefusedPath{.path = R"(C:\a*\TaskSmack.exe)", .why = "a wildcard"},
        RefusedPath{.path = "C:\\Task\tSmack\\TaskSmack.exe", .why = "a control character"},
        RefusedPath{.path = "C:\\TaskSmack\\TaskSmack.exe\r\n", .why = "a line break"},
        RefusedPath{.path = R"( C:\TaskSmack\TaskSmack.exe)", .why = "leading blank"},
        RefusedPath{.path = R"(C:\ TaskSmack\TaskSmack.exe)", .why = "folder with a leading blank"},
        RefusedPath{.path = nul, .why = "an embedded NUL"},
        RefusedPath{.path = tooLong, .why = "too long"},
    };
    for (const RefusedPath& path : refused)
    {
        const auto result = validateTaskSmackPath(path.path);
        EXPECT_FALSE(result.has_value()) << path.why << ": " << path.path;
        if (!result.has_value())
        {
            EXPECT_FALSE(result.error().empty()) << path.why;
        }
    }
}

TEST(TaskManagerReplacementPathTest, TheDebuggerIsTheQuotedPath)
{
    EXPECT_EQ(debuggerFor(INSTALLED), R"("C:\Program Files\TaskSmack\TaskSmack.exe")");
}

// ---------------------------------------------------------------------------------------------------------
// programOf() and classify(): a Debugger value read back
// ---------------------------------------------------------------------------------------------------------

struct ProgramCase
{
    std::string_view value;
    std::string_view program;
};

TEST(TaskManagerReplacementReadTest, TheProgramIsTheValuesFirstWord)
{
    for (const ProgramCase& c : {
             ProgramCase{.value = "", .program = ""},
             ProgramCase{.value = "   ", .program = ""},
             ProgramCase{.value = R"("C:\Program Files\TaskSmack\TaskSmack.exe")", .program = INSTALLED},
             ProgramCase{.value = R"("C:\Program Files\TaskSmack\TaskSmack.exe" /extra)", .program = INSTALLED},
             ProgramCase{.value = R"("C:\unterminated\TaskSmack.exe)", .program = R"(C:\unterminated\TaskSmack.exe)"},
             ProgramCase{.value = R"(C:\Tools\procexp64.exe)", .program = R"(C:\Tools\procexp64.exe)"},
             ProgramCase{.value = R"(C:\Program Files\Sysinternals\procexp64.exe /e)",
                         .program = R"(C:\Program Files\Sysinternals\procexp64.exe)"},
             ProgramCase{.value = R"(C:\Tools\PROCEXP.EXE)", .program = R"(C:\Tools\PROCEXP.EXE)"},
             ProgramCase{.value = R"(C:\Tools\a.exefile.exe)", .program = R"(C:\Tools\a.exefile.exe)"}, // ".exe" mid-word isn't the end
             ProgramCase{.value = R"(C:\Tools\script.cmd /x)", .program = R"(C:\Tools\script.cmd)"},
             ProgramCase{.value = "\t"
                                  R"(C:\Tools\x.exe)"
                                  "\t",
                         .program = R"(C:\Tools\x.exe)"},
         })
    {
        EXPECT_EQ(programOf(c.value), c.program) << c.value;
    }
}

TEST(TaskManagerReplacementReadTest, ClassifySortsOffThisCopyAnotherTaskSmackAndAnotherProgram)
{
    EXPECT_EQ(classify(std::nullopt, INSTALLED).state, State::Off);
    EXPECT_EQ(classify(std::string("  "), INSTALLED).state, State::Off);

    const Status mine = classify(debuggerFor(INSTALLED), INSTALLED);
    EXPECT_EQ(mine.state, State::ThisCopy);
    EXPECT_EQ(mine.program, INSTALLED);
    EXPECT_EQ(classify(std::string(R"("C:\PROGRAM FILES\TASKSMACK\TASKSMACK.EXE")"), INSTALLED).state, State::ThisCopy);

    const Status moved = classify(std::string(R"("D:\Old\TaskSmack.exe")"), INSTALLED);
    EXPECT_EQ(moved.state, State::OtherTaskSmack);
    EXPECT_EQ(moved.program, R"(D:\Old\TaskSmack.exe)");
    EXPECT_EQ(classify(debuggerFor(INSTALLED), "").state, State::OtherTaskSmack) << "no self path: never this copy";

    const Status procexp = classify(std::string(R"("C:\Tools\procexp64.exe")"), INSTALLED);
    EXPECT_EQ(procexp.state, State::OtherProgram);
    EXPECT_EQ(procexp.program, R"(C:\Tools\procexp64.exe)");
}

TEST(TaskManagerReplacementReadTest, AnInstalledCopyIsUnderProgramFiles)
{
    const std::array<std::string_view, 3> folders{R"(C:\Program Files)", R"(C:\Program Files (x86)\)", ""};
    EXPECT_TRUE(isInstalledCopy(INSTALLED, folders));
    EXPECT_TRUE(isInstalledCopy(R"(c:\program files\TaskSmack\TaskSmack.exe)", folders));
    EXPECT_TRUE(isInstalledCopy(R"(C:\Program Files (x86)\TaskSmack\TaskSmack.exe)", folders));
    EXPECT_FALSE(isInstalledCopy(R"(C:\Program Files Evil\TaskSmack.exe)", folders)) << "a folder whose name only starts the same";
    EXPECT_FALSE(isInstalledCopy(R"(C:\Users\me\Downloads\TaskSmack\TaskSmack.exe)", folders));
    EXPECT_FALSE(isInstalledCopy(R"(C:\Program Files)", folders));
    EXPECT_FALSE(isInstalledCopy("", folders));
    EXPECT_FALSE(isInstalledCopy(INSTALLED, {}));
}

TEST(TaskManagerReplacementReadTest, TheStartupCheckOffersARepairOnlyWhenItShould)
{
    EXPECT_TRUE(offersRepair(State::OtherTaskSmack, /*otherProgramExists=*/false, /*installedCopy=*/false)) << "moved or deleted";
    EXPECT_TRUE(offersRepair(State::OtherTaskSmack, true, true)) << "the installed copy should be the one";
    EXPECT_FALSE(offersRepair(State::OtherTaskSmack, true, false)) << "a portable copy doesn't nag while the installed one has it";
    for (const State state : {State::Off, State::ThisCopy, State::OtherProgram})
    {
        EXPECT_FALSE(offersRepair(state, false, true));
        EXPECT_FALSE(offersRepair(state, true, true));
    }
}

// ---------------------------------------------------------------------------------------------------------
// applySet() and applyClear() against a fake registry: the state machine
// ---------------------------------------------------------------------------------------------------------

TEST(TaskManagerReplacementApplyTest, TurningOnMakesAndMarksTheKey)
{
    Mocks::FakeIfeoRegistry registry;
    const ProcessActionResult on = applySet(registry, INSTALLED, false);
    EXPECT_TRUE(on.success) << on.errorMessage;
    EXPECT_EQ(registry.debugger(), debuggerFor(INSTALLED));
    EXPECT_TRUE(registry.marked()) << "TaskSmack made the key, so it may remove it";
    EXPECT_EQ(classify(*registry.readDebugger(), INSTALLED).state, State::ThisCopy);
}

TEST(TaskManagerReplacementApplyTest, TurningOffRemovesTheValueAndTheKeyTaskSmackMade)
{
    Mocks::FakeIfeoRegistry registry;
    ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
    const ProcessActionResult off = applyClear(registry);
    EXPECT_TRUE(off.success) << off.errorMessage;
    EXPECT_FALSE(registry.keyExists);
    EXPECT_EQ(classify(*registry.readDebugger(), INSTALLED).state, State::Off);
}

TEST(TaskManagerReplacementApplyTest, AKeyAnotherToolMadeStaysWithItsValues)
{
    Mocks::FakeIfeoRegistry registry;
    registry.keyExists = true;
    registry.values["GlobalFlag"] = "0x200"; // another tool's, before TaskSmack
    ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
    EXPECT_FALSE(registry.marked()) << "the key was there: not TaskSmack's";
    ASSERT_TRUE(applyClear(registry).success);
    EXPECT_TRUE(registry.keyExists);
    EXPECT_EQ(registry.values.size(), 1U);
    EXPECT_EQ(registry.values["GlobalFlag"], "0x200");
}

TEST(TaskManagerReplacementApplyTest, AnEmptyKeyAnotherToolMadeIsntDeleted)
{
    Mocks::FakeIfeoRegistry registry;
    registry.keyExists = true; // empty, and no mark
    ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
    ASSERT_TRUE(applyClear(registry).success);
    EXPECT_TRUE(registry.keyExists);
    EXPECT_TRUE(registry.values.empty());
}

TEST(TaskManagerReplacementApplyTest, AValueAddedSinceKeepsTaskSmacksKey)
{
    Mocks::FakeIfeoRegistry registry;
    ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
    registry.values["MitigationOptions"] = "1"; // set by another tool while TaskSmack had it
    ASSERT_TRUE(applyClear(registry).success);
    EXPECT_TRUE(registry.keyExists);
    EXPECT_FALSE(registry.marked());
    EXPECT_FALSE(registry.debugger().has_value());
    EXPECT_EQ(registry.values.size(), 1U);
}

TEST(TaskManagerReplacementApplyTest, AnotherProgramIsNeverReplacedWithoutConfirmation)
{
    Mocks::FakeIfeoRegistry registry;
    Mocks::FakeIfeoRegistry::seed(registry, R"("C:\Tools\procexp64.exe")");
    const ProcessActionResult refused = applySet(registry, INSTALLED, false);
    EXPECT_FALSE(refused.success);
    EXPECT_NE(refused.errorMessage.find(R"(C:\Tools\procexp64.exe)"), std::string::npos) << refused.errorMessage;
    EXPECT_EQ(registry.debugger(), R"("C:\Tools\procexp64.exe")") << "untouched";
    for (const std::string& call : registry.calls)
    {
        EXPECT_NE(call, "writeDebugger");
        EXPECT_NE(call, "createKey");
    }

    const ProcessActionResult replaced = applySet(registry, INSTALLED, true);
    EXPECT_TRUE(replaced.success) << replaced.errorMessage;
    EXPECT_EQ(registry.debugger(), debuggerFor(INSTALLED));
    EXPECT_FALSE(registry.marked()) << "Process Explorer's key, not TaskSmack's";
}

TEST(TaskManagerReplacementApplyTest, AnotherProgramsDebuggerIsNeverRemoved)
{
    Mocks::FakeIfeoRegistry registry;
    Mocks::FakeIfeoRegistry::seed(registry, R"(C:\Tools\procexp64.exe /e)");
    const ProcessActionResult refused = applyClear(registry);
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(registry.debugger(), R"(C:\Tools\procexp64.exe /e)");
}

TEST(TaskManagerReplacementApplyTest, RepairPointsAnotherTaskSmackAtThisCopyWithoutConfirmation)
{
    Mocks::FakeIfeoRegistry registry;
    ASSERT_TRUE(applySet(registry, R"(D:\Old\TaskSmack.exe)", false).success);
    const ProcessActionResult repaired = applySet(registry, INSTALLED, false);
    EXPECT_TRUE(repaired.success) << repaired.errorMessage;
    EXPECT_EQ(registry.debugger(), debuggerFor(INSTALLED));
    EXPECT_TRUE(registry.marked()) << "still the key TaskSmack made";
}

TEST(TaskManagerReplacementApplyTest, TurningOffRemovesAnotherTaskSmacksDebuggerToo)
{
    Mocks::FakeIfeoRegistry registry;
    ASSERT_TRUE(applySet(registry, R"(D:\Old\TaskSmack.exe)", false).success);
    EXPECT_TRUE(applyClear(registry).success);
    EXPECT_FALSE(registry.keyExists);
}

TEST(TaskManagerReplacementApplyTest, TurningOffWhenOffSucceedsAndTidiesAnEmptyMarkedKey)
{
    Mocks::FakeIfeoRegistry none;
    EXPECT_TRUE(applyClear(none).success);
    EXPECT_FALSE(none.keyExists);

    Mocks::FakeIfeoRegistry leftOver; // a mark left behind with no Debugger
    leftOver.keyExists = true;
    leftOver.values[std::string(CREATED_MARKER_VALUE)] = "1";
    EXPECT_TRUE(applyClear(leftOver).success);
    EXPECT_FALSE(leftOver.keyExists);
}

TEST(TaskManagerReplacementApplyTest, SetRefusesAPathThatIsntTaskSmack)
{
    Mocks::FakeIfeoRegistry registry;
    const ProcessActionResult refused = applySet(registry, R"(C:\Windows\System32\cmd.exe)", true);
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(registry.calls.empty()) << "nothing read or written";
}

TEST(TaskManagerReplacementApplyTest, RegistryFailuresSayWhyAndChangeNothingFurther)
{
    {
        Mocks::FakeIfeoRegistry registry;
        registry.failures["readDebugger"] = "access denied";
        const ProcessActionResult result = applySet(registry, INSTALLED, false);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.errorMessage.find("access denied"), std::string::npos);
        EXPECT_FALSE(applyClear(registry).success);
    }
    {
        Mocks::FakeIfeoRegistry registry;
        registry.failures["createKey"] = "access denied";
        EXPECT_FALSE(applySet(registry, INSTALLED, false).success);
        EXPECT_FALSE(registry.keyExists);
    }
    {
        Mocks::FakeIfeoRegistry registry;
        registry.failures["writeDebugger"] = "disk full";
        const ProcessActionResult result = applySet(registry, INSTALLED, false);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.errorMessage.find("disk full"), std::string::npos);
    }
    {
        Mocks::FakeIfeoRegistry registry; // the mark can't be written: on anyway, and the key is only left behind
        registry.failures["writeCreatedMarker"] = "no";
        EXPECT_TRUE(applySet(registry, INSTALLED, false).success);
        EXPECT_TRUE(applyClear(registry).success);
        EXPECT_TRUE(registry.keyExists);
    }
    {
        Mocks::FakeIfeoRegistry registry;
        ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
        registry.failures["deleteDebugger"] = "access denied";
        EXPECT_FALSE(applyClear(registry).success);
        EXPECT_TRUE(registry.debugger().has_value());
    }
    {
        Mocks::FakeIfeoRegistry registry; // the mark can't be read or removed: the key stays
        ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
        registry.failures["readCreatedMarker"] = "no";
        EXPECT_TRUE(applyClear(registry).success);
        EXPECT_TRUE(registry.keyExists);
        registry.failures.clear();
        ASSERT_TRUE(applySet(registry, INSTALLED, false).success);
        registry.failures["deleteCreatedMarker"] = "no";
        EXPECT_TRUE(applyClear(registry).success);
        EXPECT_TRUE(registry.keyExists);
    }
}

} // namespace
} // namespace Platform::TaskManagerReplacement

// -------------------------------------------------------------------------------------------------------------
// TaskManagerReplacementService: the read, and a change through a fake helper or a fake registry
// -------------------------------------------------------------------------------------------------------------

namespace Platform
{
namespace
{

constexpr std::string_view SELF = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";

[[nodiscard]] TaskManagerReplacementSetup setupReading(std::shared_ptr<Mocks::FakeIfeoRegistry> registry)
{
    TaskManagerReplacementSetup setup;
    setup.selfPath = std::string(SELF);
    setup.installedCopy = true;
    setup.readDebugger = [registry]
    {
        return registry->readDebugger();
    };
    setup.fileExists = [](std::string_view path)
    {
        return path == SELF;
    };
    return setup;
}

TEST(TaskManagerReplacementServiceTest, ReadSortsTheValueAndSaysWhetherTheOtherProgramExists)
{
    auto registry = std::make_shared<Mocks::FakeIfeoRegistry>();
    TaskManagerReplacementService service(setupReading(registry));
    TaskManagerReplacementInfo info = service.read();
    EXPECT_EQ(info.status.state, TaskManagerReplacement::State::Off);
    EXPECT_FALSE(info.readFailed);
    EXPECT_EQ(info.selfPath, SELF);
    EXPECT_TRUE(info.installedCopy);
    EXPECT_TRUE(info.otherProgramExists);

    Mocks::FakeIfeoRegistry::seed(*registry, R"("D:\Gone\TaskSmack.exe")");
    info = service.read();
    EXPECT_EQ(info.status.state, TaskManagerReplacement::State::OtherTaskSmack);
    EXPECT_FALSE(info.otherProgramExists);

    registry->failures["readDebugger"] = "access denied";
    info = service.read();
    EXPECT_TRUE(info.readFailed);
    EXPECT_EQ(info.readError, "access denied");
    EXPECT_EQ(info.status.state, TaskManagerReplacement::State::Off);
}

TEST(TaskManagerReplacementServiceTest, ReadWithoutAReaderFails)
{
    TaskManagerReplacementService service(TaskManagerReplacementSetup{});
    EXPECT_TRUE(service.read().readFailed);
}

TEST(TaskManagerReplacementServiceTest, ChangesGoThroughTheHelperNamingThisCopy)
{
    auto registry = std::make_shared<Mocks::FakeIfeoRegistry>();
    auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/false);
    TaskManagerReplacementSetup setup = setupReading(registry);
    setup.broker = broker;
    TaskManagerReplacementService service(std::move(setup));

    const ProcessActionResult on = service.change(Broker::TaskManagerChange::Set, true, std::stop_token{});
    EXPECT_TRUE(on.success) << on.errorMessage;
    EXPECT_EQ(broker->connectCount(), 1) << "the helper started first: its UAC prompt";
    const std::vector<Broker::Request> first = broker->requests();
    ASSERT_EQ(first.size(), 1U);
    const auto* request = std::get_if<Broker::TaskManagerReplacementRequest>(&first.front());
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->change, Broker::TaskManagerChange::Set);
    EXPECT_EQ(request->path, SELF);
    EXPECT_TRUE(request->replaceOther);

    const ProcessActionResult off = service.change(Broker::TaskManagerChange::Clear, true, std::stop_token{});
    EXPECT_TRUE(off.success);
    EXPECT_EQ(broker->connectCount(), 1) << "already running";
    const std::vector<Broker::Request> both = broker->requests();
    ASSERT_EQ(both.size(), 2U);
    const auto* clear = std::get_if<Broker::TaskManagerReplacementRequest>(&both.back());
    ASSERT_NE(clear, nullptr);
    EXPECT_EQ(clear->change, Broker::TaskManagerChange::Clear);
    EXPECT_TRUE(clear->path.empty()) << "a Clear carries no path";
    EXPECT_FALSE(clear->replaceOther);
    EXPECT_TRUE(registry->calls.empty()) << "the helper writes, not TaskSmack";
}

TEST(TaskManagerReplacementServiceTest, ADismissedPromptOrARefusalIsTheResult)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setConnectFailure("The operation was canceled by the user");
    TaskManagerReplacementSetup setup = setupReading(std::make_shared<Mocks::FakeIfeoRegistry>());
    setup.broker = broker;
    TaskManagerReplacementService service(std::move(setup));
    const ProcessActionResult dismissed = service.change(Broker::TaskManagerChange::Set, false, std::stop_token{});
    EXPECT_FALSE(dismissed.success);
    EXPECT_NE(dismissed.errorMessage.find("canceled"), std::string::npos);
    EXPECT_TRUE(broker->requests().empty());

    broker->setConnectFailure("");
    broker->setAnswer([](const Broker::Request&) { return Broker::Response{Broker::ErrorReply{.message = "no"}}; });
    const ProcessActionResult refused = service.change(Broker::TaskManagerChange::Set, false, std::stop_token{});
    EXPECT_FALSE(refused.success);
    EXPECT_NE(refused.errorMessage.find("refused"), std::string::npos) << refused.errorMessage;
}

TEST(TaskManagerReplacementServiceTest, AnElevatedCopyWritesTheRegistryItself)
{
    auto registry = std::make_shared<Mocks::FakeIfeoRegistry>();
    TaskManagerReplacementSetup setup = setupReading(registry);
    setup.direct = registry;
    TaskManagerReplacementService service(std::move(setup));
    EXPECT_TRUE(service.change(Broker::TaskManagerChange::Set, false, std::stop_token{}).success);
    EXPECT_EQ(registry->debugger(), TaskManagerReplacement::debuggerFor(SELF));
    EXPECT_EQ(service.read().status.state, TaskManagerReplacement::State::ThisCopy);
    EXPECT_TRUE(service.change(Broker::TaskManagerChange::Clear, false, std::stop_token{}).success);
    EXPECT_EQ(service.read().status.state, TaskManagerReplacement::State::Off);
}

TEST(TaskManagerReplacementServiceTest, WithNeitherItSaysWhy)
{
    TaskManagerReplacementSetup setup = setupReading(std::make_shared<Mocks::FakeIfeoRegistry>());
    setup.unavailableReason = "this copy can't use the helper";
    TaskManagerReplacementService service(std::move(setup));
    const ProcessActionResult result = service.change(Broker::TaskManagerChange::Clear, false, std::stop_token{});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "this copy can't use the helper");
    EXPECT_EQ(service.read().unavailableReason, "this copy can't use the helper");

    TaskManagerReplacementService bare(setupReading(std::make_shared<Mocks::FakeIfeoRegistry>()));
    EXPECT_FALSE(bare.change(Broker::TaskManagerChange::Clear, false, std::stop_token{}).errorMessage.empty());
}

TEST(TaskManagerReplacementServiceTest, ACopyWhosePathIsntValidNeverAsks)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    TaskManagerReplacementSetup setup = setupReading(std::make_shared<Mocks::FakeIfeoRegistry>());
    setup.selfPath = R"(\\server\share\TaskSmack.exe)";
    setup.broker = broker;
    TaskManagerReplacementService service(std::move(setup));
    EXPECT_FALSE(service.change(Broker::TaskManagerChange::Set, false, std::stop_token{}).success);
    EXPECT_TRUE(broker->requests().empty());
}

} // namespace
} // namespace Platform
