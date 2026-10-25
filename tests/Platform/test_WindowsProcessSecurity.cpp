/// @file test_WindowsProcessSecurity.cpp
/// @brief Platform::Windows::WindowsProcessSecurityReader (#1526) against fake kernel32/advapi32 calls:
/// every token field parsed (user, session, integrity level, elevation, AppContainer, virtualization,
/// privileges with their state, groups less the integrity label), SIDs named once and cached, a long
/// account name, an unnamed SID and privilege, the denials (a refused open, a protected process's
/// token, a refused TokenUser), a reused PID, a token whose groups grow between the size query and the
/// copy (or keep growing), queries that misreport their size or answer short, unknown elevation types and odd
/// integrity labels, accounts that can't be named, the name cache starting over when full, optional queries
/// that fail, and both handles closed. Plus sidToString() against
/// ConvertSidToStringSidW, and one read of this test process through the real calls.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/Windows/WindowsProcessSecurity.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sddl.h>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};

/// A SID's bytes: revision 1, the authority's low byte (the high five are zero), the sub-authorities.
using SidBytes = std::vector<std::byte>;

[[nodiscard]] SidBytes makeSid(BYTE authority, const std::vector<DWORD>& subAuthorities)
{
    SidBytes sid(8 + (subAuthorities.size() * sizeof(DWORD)), std::byte{0});
    sid[0] = std::byte{SID_REVISION};
    sid[1] = static_cast<std::byte>(subAuthorities.size());
    sid[7] = static_cast<std::byte>(authority);
    std::memcpy(sid.data() + 8, subAuthorities.data(), subAuthorities.size() * sizeof(DWORD));
    return sid;
}

const SidBytes& userSid() // S-1-5-21-1111-2222-3333-1001
{
    static const SidBytes sid = makeSid(5, {21, 1111, 2222, 3333, 1001});
    return sid;
}

const SidBytes& everyoneSid() // S-1-1-0
{
    static const SidBytes sid = makeSid(1, {0});
    return sid;
}

const SidBytes& adminsSid() // S-1-5-32-544
{
    static const SidBytes sid = makeSid(5, {32, 544});
    return sid;
}

const SidBytes& unnamedSid() // resolves to no name
{
    static const SidBytes sid = makeSid(5, {21, 9, 9, 9, 7777});
    return sid;
}

const SidBytes& mediumLabelSid() // S-1-16-8192
{
    static const SidBytes sid = makeSid(16, {SECURITY_MANDATORY_MEDIUM_RID});
    return sid;
}

struct FakeGroup
{
    SidBytes sid;
    DWORD attributes = 0;
};

struct FakePrivilege
{
    DWORD luidLow = 0;
    DWORD attributes = 0;
};

/// The answers the fakes give, set per test.
struct FakeToken
{
    DWORD openError = 0;  // non-zero: OpenProcess fails with it
    DWORD tokenError = 0; // non-zero: OpenProcessToken fails with it
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE;                             // GetExitCodeProcess's answer
    std::map<TOKEN_INFORMATION_CLASS, DWORD> classErrors;      // a class's query fails with this error
    int growGroupsOnFirstCopy = 0;                             // groups added after the first sized TokenGroups query
    bool growGroupsOnEveryCopy = false;                        // ... and after every later one too
    bool timesFail = false;                                    // GetProcessTimes fails
    std::map<TOKEN_INFORMATION_CLASS, DWORD> sizeNeeds;        // a class's size query reports this size instead
    std::map<TOKEN_INFORMATION_CLASS, DWORD> copyErrors;       // a class's copy (not its size query) fails with this error
    std::map<TOKEN_INFORMATION_CLASS, std::size_t> truncateTo; // a class's answer cut to this many bytes
    DWORD elevationType = TokenElevationTypeLimited;
    const SidBytes* labelSid = nullptr;   // TokenIntegrityLevel's label; null: the medium label
    std::vector<std::string> refuseRetry; // SID strings whose lookup fails once it has room for the name
    std::vector<FakeGroup> groups;
    std::vector<FakePrivilege> privileges;
    std::map<std::string, std::pair<std::wstring, std::wstring>> accounts; // SID string -> (domain, name)
    std::map<DWORD, std::wstring> privilegeNames;                          // LUID low part -> name
    int accountLookups = 0;
    int groupCopies = 0;
    std::vector<HANDLE> handedOut; // every handle the open fakes returned
};

FakeToken& fake()
{
    static FakeToken instance;
    return instance;
}

HANDLE realHandle()
{
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // a real handle the reader closes
    fake().handedOut.push_back(handle);
    return handle;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    EXPECT_EQ(access, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION));
    EXPECT_EQ(pid, static_cast<DWORD>(TARGET.pid));
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    return realHandle();
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE /*process*/, LPDWORD code)
{
    *code = fake().exitCode;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    if (fake().timesFail)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeOpenProcessToken(HANDLE /*process*/, DWORD access, PHANDLE token)
{
    EXPECT_EQ(access, static_cast<DWORD>(TOKEN_QUERY));
    if (fake().tokenError != 0)
    {
        SetLastError(fake().tokenError);
        return FALSE;
    }
    *token = realHandle();
    return TRUE;
}

/// Lays out a header of @p headerBytes and the SIDs after it, the way the token does: each pointer
/// slot (at the given header offset) points at its SID inside @p out.
[[nodiscard]] std::vector<std::byte>
layOut(const std::vector<std::byte>& header, const std::vector<std::pair<std::size_t, const SidBytes*>>& sids, const std::byte* out)
{
    std::vector<std::byte> answer = header;
    for (const auto& [slot, sid] : sids)
    {
        const std::size_t offset = answer.size();
        answer.insert(answer.end(), sid->begin(), sid->end());
        // Address arithmetic, not pointer arithmetic: @p out is null for a size query.
        const auto pointer = reinterpret_cast<std::uintptr_t>(out) + offset;
        std::memcpy(answer.data() + slot, &pointer, sizeof(pointer));
    }
    return answer;
}

template<typename T> [[nodiscard]] std::vector<std::byte> bytesOf(const T& value)
{
    std::vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

/// The answer to one GetTokenInformation class, its SID pointers aimed into @p out.
[[nodiscard]] std::vector<std::byte> answerFor(TOKEN_INFORMATION_CLASS infoClass, const std::byte* out)
{
    switch (infoClass)
    {
    case TokenUser:
        return layOut(bytesOf(TOKEN_USER{}), {{offsetof(TOKEN_USER, User) + offsetof(SID_AND_ATTRIBUTES, Sid), &userSid()}}, out);
    case TokenIntegrityLevel:
    {
        TOKEN_MANDATORY_LABEL label{};
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        const SidBytes* sid = fake().labelSid != nullptr ? fake().labelSid : &mediumLabelSid();
        return layOut(bytesOf(label), {{offsetof(TOKEN_MANDATORY_LABEL, Label) + offsetof(SID_AND_ATTRIBUTES, Sid), sid}}, out);
    }
    case TokenGroups:
    {
        const std::vector<FakeGroup>& groups = fake().groups;
        std::vector<std::byte> header(offsetof(TOKEN_GROUPS, Groups) + (groups.size() * sizeof(SID_AND_ATTRIBUTES)), std::byte{0});
        const auto count = static_cast<DWORD>(groups.size());
        std::memcpy(header.data(), &count, sizeof(count));
        std::vector<std::pair<std::size_t, const SidBytes*>> sids;
        std::size_t index = 0;
        while (index < groups.size())
        {
            const std::size_t entry = offsetof(TOKEN_GROUPS, Groups) + (index * sizeof(SID_AND_ATTRIBUTES));
            std::memcpy(header.data() + entry + offsetof(SID_AND_ATTRIBUTES, Attributes), &groups[index].attributes, sizeof(DWORD));
            sids.emplace_back(entry + offsetof(SID_AND_ATTRIBUTES, Sid), &groups[index].sid);
            ++index;
        }
        return layOut(header, sids, out);
    }
    case TokenPrivileges:
    {
        const std::vector<FakePrivilege>& privileges = fake().privileges;
        std::vector<std::byte> answer(offsetof(TOKEN_PRIVILEGES, Privileges) + (privileges.size() * sizeof(LUID_AND_ATTRIBUTES)),
                                      std::byte{0});
        const auto count = static_cast<DWORD>(privileges.size());
        std::memcpy(answer.data(), &count, sizeof(count));
        std::size_t index = 0;
        while (index < privileges.size())
        {
            LUID_AND_ATTRIBUTES entry{};
            entry.Luid.LowPart = privileges[index].luidLow;
            entry.Attributes = privileges[index].attributes;
            std::memcpy(answer.data() + offsetof(TOKEN_PRIVILEGES, Privileges) + (index * sizeof(entry)), &entry, sizeof(entry));
            ++index;
        }
        return answer;
    }
    case TokenElevation:
        return bytesOf(TOKEN_ELEVATION{.TokenIsElevated = 0});
    case TokenElevationType:
        return bytesOf(fake().elevationType);
    case TokenIsAppContainer:
        return bytesOf(DWORD{0});
    case TokenVirtualizationEnabled:
        return bytesOf(DWORD{1});
    case TokenSessionId:
        return bytesOf(DWORD{3});
    default:
        ADD_FAILURE() << "unexpected token class " << static_cast<int>(infoClass);
        return {};
    }
}

BOOL WINAPI fakeGetTokenInformation(HANDLE /*token*/, TOKEN_INFORMATION_CLASS infoClass, LPVOID buffer, DWORD bytes, PDWORD needed)
{
    if (const auto error = fake().classErrors.find(infoClass); error != fake().classErrors.end())
    {
        SetLastError(error->second);
        return FALSE;
    }
    if (const auto error = fake().copyErrors.find(infoClass); error != fake().copyErrors.end() && buffer != nullptr)
    {
        SetLastError(error->second);
        return FALSE;
    }
    if (infoClass == TokenGroups && buffer != nullptr && (++fake().groupCopies == 1 || fake().growGroupsOnEveryCopy))
    {
        const int count = fake().growGroupsOnEveryCopy ? 1 : fake().growGroupsOnFirstCopy;
        int added = 0;
        while (added < count)
        {
            fake().groups.push_back({.sid = makeSid(5, {21, 5, 5, 5, static_cast<DWORD>(2000 + added)}), .attributes = SE_GROUP_ENABLED});
            ++added;
        }
    }
    auto* out = static_cast<std::byte*>(buffer);
    std::vector<std::byte> answer = answerFor(infoClass, out);
    if (const auto cut = fake().truncateTo.find(infoClass); cut != fake().truncateTo.end())
    {
        answer.resize(std::min(answer.size(), cut->second));
    }
    if (const auto size = fake().sizeNeeds.find(infoClass); size != fake().sizeNeeds.end() && buffer == nullptr)
    {
        *needed = size->second;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *needed = static_cast<DWORD>(answer.size());
    if (buffer == nullptr || bytes < answer.size())
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(out, answer.data(), answer.size());
    return TRUE;
}

/// Writes @p text into a caller's buffer of *@p chars, as LookupAccountSidW does: false (and the size
/// needed, terminator included) when it does not fit; else the length written.
bool fitName(const std::wstring& text, LPWSTR buffer, LPDWORD chars)
{
    if (*chars <= text.size())
    {
        *chars = static_cast<DWORD>(text.size() + 1);
        return false;
    }
    std::memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    *chars = static_cast<DWORD>(text.size());
    return true;
}

BOOL WINAPI
fakeLookupAccountSid(LPCWSTR system, PSID sid, LPWSTR name, LPDWORD nameChars, LPWSTR domain, LPDWORD domainChars, PSID_NAME_USE use)
{
    EXPECT_EQ(system, nullptr);
    ++fake().accountLookups;
    const auto found = fake().accounts.find(sidToString(sid));
    if (found == fake().accounts.end())
    {
        SetLastError(ERROR_NONE_MAPPED);
        return FALSE;
    }
    const auto& [domainText, nameText] = found->second;
    const DWORD nameRoom = *nameChars;
    const DWORD domainRoom = *domainChars;
    if (nameRoom <= nameText.size() || domainRoom <= domainText.size())
    {
        *nameChars = static_cast<DWORD>(nameText.size() + 1);
        *domainChars = static_cast<DWORD>(domainText.size() + 1);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (std::ranges::contains(fake().refuseRetry, found->first))
    {
        SetLastError(ERROR_NONE_MAPPED);
        return FALSE;
    }
    EXPECT_TRUE(fitName(nameText, name, nameChars));
    EXPECT_TRUE(fitName(domainText, domain, domainChars));
    *use = SidTypeUser;
    return TRUE;
}

BOOL WINAPI fakeLookupPrivilegeName(LPCWSTR /*system*/, PLUID luid, LPWSTR name, LPDWORD chars)
{
    const auto found = fake().privilegeNames.find(luid->LowPart);
    if (found == fake().privilegeNames.end())
    {
        SetLastError(ERROR_NO_SUCH_PRIVILEGE);
        return FALSE;
    }
    if (!fitName(found->second, name, chars))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

ProcessSecurityFunctions fakeApi()
{
    return {.openProcess = &fakeOpenProcess,
            .getProcessTimes = &fakeGetProcessTimes,
            .getExitCodeProcess = &fakeGetExitCodeProcess,
            .openProcessToken = &fakeOpenProcessToken,
            .getTokenInformation = &fakeGetTokenInformation,
            .lookupAccountSid = &fakeLookupAccountSid,
            .lookupPrivilegeName = &fakeLookupPrivilegeName};
}

class WindowsProcessSecurityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
        fake().groups = {{.sid = everyoneSid(), .attributes = SE_GROUP_ENABLED | SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT},
                         {.sid = adminsSid(), .attributes = SE_GROUP_USE_FOR_DENY_ONLY},
                         {.sid = unnamedSid(), .attributes = 0},
                         {.sid = mediumLabelSid(), .attributes = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED}};
        fake().privileges = {{.luidLow = 19, .attributes = 0},
                             {.luidLow = 23, .attributes = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT}};
        fake().accounts = {{"S-1-5-21-1111-2222-3333-1001", {L"DESKTOP", L"matt"}},
                           {"S-1-1-0", {L"", L"Everyone"}},
                           {"S-1-5-32-544", {L"BUILTIN", L"Administrators"}}};
        fake().privilegeNames = {{19, L"SeShutdownPrivilege"}, {23, L"SeChangeNotifyPrivilege"}};
    }

    void TearDown() override
    {
        // Every handle the reader was given is closed by the time the read returns.
        for (HANDLE handle : fake().handedOut)
        {
            DWORD flags = 0;
            EXPECT_EQ(GetHandleInformation(handle, &flags), FALSE) << "a handle was left open";
        }
    }
};

TEST_F(WindowsProcessSecurityTest, ReadsEveryTokenField)
{
    WindowsProcessSecurityReader reader(fakeApi());
    EXPECT_TRUE(reader.hasSecurity());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    EXPECT_TRUE(result.detail.empty());
    const ProcessSecurity& security = result.security;

    ASSERT_TRUE(security.tokenUser.has_value());
    const SecurityAccount expectedUser{.sid = "S-1-5-21-1111-2222-3333-1001", .name = "DESKTOP\\matt"};
    EXPECT_EQ(security.tokenUser, std::optional{expectedUser});
    EXPECT_EQ(security.sessionId, std::optional<std::uint32_t>{3});
    EXPECT_EQ(security.integrityLevel, std::optional<std::uint32_t>{SECURITY_MANDATORY_MEDIUM_RID});
    EXPECT_EQ(security.elevated, std::optional<bool>{false});
    EXPECT_EQ(security.elevationType, std::optional<TokenElevationKind>{TokenElevationKind::Limited});
    EXPECT_EQ(security.appContainer, std::optional<bool>{false});
    EXPECT_EQ(security.virtualizationEnabled, std::optional<bool>{true});

    const std::vector<TokenPrivilege> privileges{{.name = "SeShutdownPrivilege", .enabled = false},
                                                 {.name = "SeChangeNotifyPrivilege", .enabled = true}};
    EXPECT_EQ(security.privileges, std::optional{privileges});

    // The integrity label is not listed as a group; an unnamed SID keeps its string form only.
    const std::vector<TokenGroup> groups{
        {.account = {.sid = "S-1-1-0", .name = "Everyone"}, .enabled = true, .denyOnly = false},
        {.account = {.sid = "S-1-5-32-544", .name = "BUILTIN\\Administrators"}, .enabled = false, .denyOnly = true},
        {.account = {.sid = "S-1-5-21-9-9-9-7777", .name = ""}, .enabled = false, .denyOnly = false},
    };
    EXPECT_EQ(security.tokenGroups, std::optional{groups});

    // None of Linux's fields are made up.
    EXPECT_FALSE(security.users.has_value());
    EXPECT_FALSE(security.seccomp.has_value());
    EXPECT_TRUE(security.securityLabel.empty());
    EXPECT_EQ(fake().handedOut.size(), 2U); // the process and its token
}

TEST_F(WindowsProcessSecurityTest, NamesEachSidOnceAcrossReads)
{
    WindowsProcessSecurityReader reader(fakeApi());
    ASSERT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::Ok);
    const int lookups = fake().accountLookups;
    EXPECT_EQ(lookups, 4); // the user and three groups; an unnamed SID is cached as unnamed too
    EXPECT_EQ(reader.cachedAccountCount(), 4U);

    const SecurityReadResult again = reader.readSecurity(TARGET);
    ASSERT_EQ(again.status, SecurityReadStatus::Ok);
    EXPECT_EQ(fake().accountLookups, lookups);
    ASSERT_TRUE(again.security.tokenUser.has_value());
    EXPECT_EQ(again.security.tokenUser.value_or(SecurityAccount{}).name, "DESKTOP\\matt");
}

TEST_F(WindowsProcessSecurityTest, RetriesALookupWithRoomForALongName)
{
    const std::wstring longName(300, L'x');
    fake().accounts["S-1-5-21-1111-2222-3333-1001"] = {L"CORP", longName};
    fake().privilegeNames[19] = std::wstring(100, L'p');
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    ASSERT_TRUE(result.security.tokenUser.has_value());
    EXPECT_EQ(result.security.tokenUser.value_or(SecurityAccount{}).name, "CORP\\" + std::string(300, 'x'));
    ASSERT_TRUE(result.security.privileges.has_value());
    EXPECT_EQ(result.security.privileges.value_or(std::vector<TokenPrivilege>{}).front().name, std::string(100, 'p'));
}

TEST_F(WindowsProcessSecurityTest, AnUnknownPrivilegeIsShownByItsLuid)
{
    fake().privileges = {{.luidLow = 0xABC, .attributes = SE_PRIVILEGE_ENABLED}};
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    const std::vector<TokenPrivilege> expected{{.name = "LUID 0x0:00000ABC", .enabled = true}};
    EXPECT_EQ(result.security.privileges, std::optional{expected});
}

TEST_F(WindowsProcessSecurityTest, GroupsThatGrowBeforeTheCopyAreReadAgain)
{
    fake().growGroupsOnFirstCopy = 3;
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    ASSERT_TRUE(result.security.tokenGroups.has_value());
    EXPECT_EQ(result.security.tokenGroups.value_or(std::vector<TokenGroup>{}).size(), 6U); // three named and unnamed, three added; no label
    EXPECT_EQ(result.security.tokenGroups.value_or(std::vector<TokenGroup>{}).back().account.sid, "S-1-5-21-5-5-5-2002");
    EXPECT_EQ(fake().groupCopies, 2);
}

TEST_F(WindowsProcessSecurityTest, OptionalQueriesThatFailLeaveTheirFieldsEmpty)
{
    for (const TOKEN_INFORMATION_CLASS infoClass : {TokenIntegrityLevel,
                                                    TokenElevation,
                                                    TokenElevationType,
                                                    TokenPrivileges,
                                                    TokenGroups,
                                                    TokenIsAppContainer,
                                                    TokenVirtualizationEnabled,
                                                    TokenSessionId})
    {
        fake().classErrors[infoClass] = ERROR_INVALID_PARAMETER;
    }
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    const ProcessSecurity& security = result.security;
    EXPECT_TRUE(security.tokenUser.has_value());
    EXPECT_FALSE(security.integrityLevel.has_value());
    EXPECT_FALSE(security.elevated.has_value());
    EXPECT_FALSE(security.elevationType.has_value());
    EXPECT_FALSE(security.privileges.has_value());
    EXPECT_FALSE(security.tokenGroups.has_value());
    EXPECT_FALSE(security.appContainer.has_value());
    EXPECT_FALSE(security.virtualizationEnabled.has_value());
    EXPECT_FALSE(security.sessionId.has_value());
}

TEST_F(WindowsProcessSecurityTest, RefusesTargetsItCannotConfirm)
{
    WindowsProcessSecurityReader reader(fakeApi());
    EXPECT_EQ(reader.readSecurity({.pid = 0, .startTimeTicks = START_TICKS}).status, SecurityReadStatus::ProcessExited);
    EXPECT_EQ(reader.readSecurity({.pid = TARGET.pid, .startTimeTicks = 0}).status, SecurityReadStatus::IdentityUnknown);
    EXPECT_TRUE(fake().handedOut.empty()); // neither was opened

    fake().creationTicks = START_TICKS + 1; // the PID now names another process
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::ProcessExited);

    fake().creationTicks = START_TICKS;
    fake().exitCode = 0; // exited, its object still held open somewhere (#2206)
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::ProcessExited);
}

TEST_F(WindowsProcessSecurityTest, MapsOpenFailuresToStatuses)
{
    WindowsProcessSecurityReader reader(fakeApi());
    fake().openError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::PermissionDenied);
    fake().openError = ERROR_INVALID_PARAMETER; // the process is gone
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::ProcessExited);
    fake().openError = ERROR_NOT_ENOUGH_MEMORY;
    const SecurityReadResult failed = reader.readSecurity(TARGET);
    EXPECT_EQ(failed.status, SecurityReadStatus::Failed);
    EXPECT_FALSE(failed.detail.empty());
}

TEST_F(WindowsProcessSecurityTest, AProtectedProcessesTokenIsPermissionDenied)
{
    fake().tokenError = ERROR_ACCESS_DENIED;
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    EXPECT_EQ(result.status, SecurityReadStatus::PermissionDenied);
    EXPECT_EQ(result.security, ProcessSecurity{});
    EXPECT_EQ(fake().handedOut.size(), 1U); // the process handle, closed

    fake().tokenError = ERROR_INVALID_HANDLE;
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::Failed);
}

TEST_F(WindowsProcessSecurityTest, ATokenUserThatCannotBeReadFailsTheRead)
{
    WindowsProcessSecurityReader reader(fakeApi());
    fake().classErrors[TokenUser] = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::PermissionDenied);
    fake().classErrors[TokenUser] = ERROR_INVALID_PARAMETER;
    const SecurityReadResult failed = reader.readSecurity(TARGET);
    EXPECT_EQ(failed.status, SecurityReadStatus::Failed);
    EXPECT_FALSE(failed.detail.empty());
    EXPECT_FALSE(failed.security.tokenUser.has_value());
}

TEST_F(WindowsProcessSecurityTest, AShortTokenUserOrUnreadableTimesFailTheRead)
{
    WindowsProcessSecurityReader reader(fakeApi());
    fake().truncateTo[TokenUser] = sizeof(DWORD); // too short for a TOKEN_USER
    const SecurityReadResult shortUser = reader.readSecurity(TARGET);
    EXPECT_EQ(shortUser.status, SecurityReadStatus::Failed);
    EXPECT_FALSE(shortUser.detail.empty());

    fake().timesFail = true; // the identity can't be confirmed
    const SecurityReadResult noTimes = reader.readSecurity(TARGET);
    EXPECT_EQ(noTimes.status, SecurityReadStatus::Failed);
    EXPECT_FALSE(noTimes.detail.empty());
}

TEST_F(WindowsProcessSecurityTest, VariableSizeQueriesThatMisreportTheirSize)
{
    fake().sizeNeeds[TokenUser] = 4096;                           // more than it writes: the answer is trimmed to what it wrote
    fake().sizeNeeds[TokenGroups] = 0;                            // no size at all
    fake().sizeNeeds[TokenPrivileges] = 2U << 20U;                // past the 1 MB the reader takes
    fake().copyErrors[TokenIntegrityLevel] = ERROR_ACCESS_DENIED; // sized, then refused
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    EXPECT_EQ(result.security.tokenUser.value_or(SecurityAccount{}).name, "DESKTOP\\matt");
    EXPECT_FALSE(result.security.tokenGroups.has_value());
    EXPECT_FALSE(result.security.privileges.has_value());
    EXPECT_FALSE(result.security.integrityLevel.has_value());
    EXPECT_TRUE(result.security.elevated.has_value()); // the fixed-size queries are unaffected
}

TEST_F(WindowsProcessSecurityTest, GroupsThatKeepGrowingAreGivenUpOn)
{
    fake().growGroupsOnEveryCopy = true;
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    EXPECT_FALSE(result.security.tokenGroups.has_value());
    EXPECT_EQ(fake().groupCopies, 3); // three tries, then no groups rather than a failed read
}

TEST_F(WindowsProcessSecurityTest, OddTokenAnswersLeaveTheirFieldsEmpty)
{
    const SidBytes invalidLabel = []
    {
        SidBytes sid = mediumLabelSid();
        sid[0] = std::byte{9}; // not revision 1
        return sid;
    }();
    const SidBytes emptyLabel = makeSid(16, {}); // no sub-authority to take the RID from
    // A privilege count past the buffer: the header cut short, then the second entry cut off.
    const std::size_t oneEntry = offsetof(TOKEN_PRIVILEGES, Privileges) + sizeof(LUID_AND_ATTRIBUTES);
    for (const std::size_t privilegeBytes : {std::size_t{2}, oneEntry})
    {
        fake().truncateTo[TokenPrivileges] = privilegeBytes;
        for (const SidBytes* label : {&invalidLabel, &emptyLabel})
        {
            fake().labelSid = label;
            WindowsProcessSecurityReader reader(fakeApi());
            const SecurityReadResult result = reader.readSecurity(TARGET);
            ASSERT_EQ(result.status, SecurityReadStatus::Ok);
            EXPECT_FALSE(result.security.privileges.has_value()) << privilegeBytes;
            EXPECT_FALSE(result.security.integrityLevel.has_value());
        }
    }

    const std::vector<std::pair<DWORD, std::optional<TokenElevationKind>>> types{
        {TokenElevationTypeDefault, TokenElevationKind::Default}, {TokenElevationTypeFull, TokenElevationKind::Full}, {99, std::nullopt}};
    for (const auto& [type, kind] : types)
    {
        fake().elevationType = type;
        WindowsProcessSecurityReader reader(fakeApi());
        EXPECT_EQ(reader.readSecurity(TARGET).security.elevationType, kind) << type;
    }
}

TEST_F(WindowsProcessSecurityTest, AccountsThatCannotBeNamed)
{
    fake().accounts["S-1-5-21-1111-2222-3333-1001"] = {L"CORP", std::wstring(300, L'x')};
    fake().refuseRetry = {"S-1-5-21-1111-2222-3333-1001"}; // too long at first, then not found
    fake().accounts["S-1-5-32-544"] = {L"BUILTIN", L""};   // resolves to an empty name
    SidBytes invalid = everyoneSid();
    invalid[0] = std::byte{9};
    fake().groups = {{.sid = adminsSid(), .attributes = SE_GROUP_ENABLED}, {.sid = invalid, .attributes = SE_GROUP_ENABLED}};
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    const SecurityAccount user{.sid = "S-1-5-21-1111-2222-3333-1001", .name = ""};
    EXPECT_EQ(result.security.tokenUser, std::optional{user});
    const std::vector<TokenGroup> groups{{.account = {.sid = "S-1-5-32-544", .name = ""}, .enabled = true, .denyOnly = false},
                                         {.account = {.sid = "", .name = ""}, .enabled = true, .denyOnly = false}};
    EXPECT_EQ(result.security.tokenGroups, std::optional{groups}); // an invalid SID is neither shown as text nor looked up
    EXPECT_EQ(fake().accountLookups, 3);                           // the user twice, Administrators once
}

TEST_F(WindowsProcessSecurityTest, TheNameCacheStartsOverWhenFull)
{
    fake().groups.clear();
    constexpr DWORD GROUPS = 1100;
    for (DWORD rid = 0; rid < GROUPS; ++rid)
    {
        fake().groups.push_back({.sid = makeSid(5, {21, 7, 7, 7, 10000 + rid}), .attributes = SE_GROUP_ENABLED});
    }
    WindowsProcessSecurityReader reader(fakeApi());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    EXPECT_EQ(result.security.tokenGroups.value_or(std::vector<TokenGroup>{}).size(), GROUPS);
    // The user and 1,023 groups filled it; the 1,025th SID emptied it, and the last 76 groups came after.
    EXPECT_EQ(reader.cachedAccountCount(), (1U + GROUPS) - 1024U);
}

/// ConvertSidToStringSidW's text for @p sid.
std::string systemSidString(PSID sid)
{
    LPWSTR wide = nullptr;
    EXPECT_NE(ConvertSidToStringSidW(sid, &wide), FALSE);
    std::string text;
    for (const wchar_t c : std::wstring_view(wide))
    {
        text += static_cast<char>(c); // a SID string is ASCII
    }
    LocalFree(wide);
    return text;
}

TEST(WindowsSidToStringTest, MatchesConvertSidToStringSid)
{
    for (const SidBytes& bytes : {userSid(), everyoneSid(), adminsSid(), mediumLabelSid(), makeSid(0, {})})
    {
        SidBytes copy = bytes;
        EXPECT_EQ(sidToString(copy.data()), systemSidString(copy.data()));
    }
    // An authority past 32 bits is written in hex.
    SidBytes wide = makeSid(0, {7});
    wide[2] = std::byte{0x01};
    wide[7] = std::byte{0xAB};
    EXPECT_EQ(sidToString(wide.data()), systemSidString(wide.data()));
    EXPECT_EQ(sidToString(wide.data()), "S-1-0x100000000AB-7");

    EXPECT_EQ(sidToString(nullptr), "");
    SidBytes invalid = userSid();
    invalid[0] = std::byte{9}; // not revision 1
    EXPECT_EQ(sidToString(invalid.data()), "");
}

TEST(WindowsProcessSecurityRealTest, ReadsThisProcesssToken)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), FALSE);
    const ProcessTarget self{.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
                             .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime};

    WindowsProcessSecurityReader reader;
    const SecurityReadResult result = reader.readSecurity(self);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok) << result.detail;
    const ProcessSecurity& security = result.security;
    ASSERT_TRUE(security.tokenUser.has_value());
    EXPECT_TRUE(security.tokenUser.value_or(SecurityAccount{}).sid.starts_with("S-1-5-"));
    EXPECT_FALSE(security.tokenUser.value_or(SecurityAccount{}).name.empty());
    EXPECT_TRUE(security.integrityLevel.has_value());
    EXPECT_TRUE(security.elevated.has_value());
    EXPECT_TRUE(security.elevationType.has_value());
    EXPECT_EQ(security.appContainer, std::optional<bool>{false});
    ASSERT_TRUE(security.privileges.has_value());
    EXPECT_TRUE(std::ranges::any_of(security.privileges.value_or(std::vector<TokenPrivilege>{}),
                                    [](const TokenPrivilege& p) { return p.name == "SeChangeNotifyPrivilege"; }));
    ASSERT_TRUE(security.tokenGroups.has_value());
    EXPECT_TRUE(std::ranges::any_of(security.tokenGroups.value_or(std::vector<TokenGroup>{}),
                                    [](const TokenGroup& g) { return g.account.sid == "S-1-1-0"; }));

    DWORD session = 0;
    ASSERT_NE(ProcessIdToSessionId(GetCurrentProcessId(), &session), FALSE);
    EXPECT_EQ(security.sessionId, std::optional<std::uint32_t>{session});
}

} // namespace
} // namespace Platform::Windows
