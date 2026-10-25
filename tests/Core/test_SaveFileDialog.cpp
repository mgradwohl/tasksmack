/// @file test_SaveFileDialog.cpp
/// @brief Core::showSaveFileDialog() and Core::defaultSaveFolder() (#1866, #1891): with no dialog
/// driver SDL can use, the callback gets SDL's reason once, at once; the default folder is Documents,
/// else home, as SDL finds them. A callback that throws never unwinds into SDL's C frames (#2162), and
/// an allocation failure building the result still calls back, with an error (#2415).

#include "Core/SaveFileDialog.h"
#include "Domain/AllocationFailureHook.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#if !defined(TASKSMACK_NO_ALLOCATOR_HOOK)
#include <cstdint> // used only by the allocation-failure test, compiled out where the hook is
#include <new>
#endif

namespace
{

TEST(SaveFileDialogTest, WithoutADialogDriverTheCallbackGetsWhyAtOnce)
{
    // No real dialog can open: SDL knows only "portal" and "zenity", so any other driver name fails
    // before it looks for either, and SDL hands the callback its error straight away.
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none-for-tests"));
    int calls = 0;
    std::optional<Core::SaveFileResult> got;
    Core::showSaveFileDialog(nullptr,
                             {{.name = "Text", .pattern = "txt"}, {.name = "JSON", .pattern = "json"}},
                             "/tmp/report.txt",
                             [&](Core::SaveFileResult result)
                             {
                                 ++calls;
                                 got = std::move(result);
                             });
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(got.has_value());
    const Core::SaveFileResult result = got.value_or(Core::SaveFileResult{});
    EXPECT_TRUE(result.path.empty());
    EXPECT_EQ(result.filter, -1);
    EXPECT_TRUE(result.error.contains("File dialog driver unsupported")) << result.error;
    static_cast<void>(SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER));
}

TEST(SaveFileDialogTest, AnEmptyCallbackAndNoFiltersAreFine)
{
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none-for-tests"));
    Core::showSaveFileDialog(nullptr, {}, "", {}); // nothing to call back; the request is still freed
    static_cast<void>(SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER));
}

TEST(SaveFileDialogTest, ACallbackThatThrowsDoesNotEscapeIntoSdl)
{
    // With no driver, SDL runs onDialogClosed() at once, on this thread, from inside its own C code: an
    // exception from the callback would unwind through SDL's frames (UB, or std::terminate()) and out of
    // showSaveFileDialog(). It must be caught in the callback trampoline instead (#2162).
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none-for-tests"));
    int calls = 0;
    EXPECT_NO_THROW(Core::showSaveFileDialog(nullptr,
                                             {{.name = "Text", .pattern = "txt"}},
                                             "",
                                             [&](const Core::SaveFileResult& /*result*/)
                                             {
                                                 ++calls;
                                                 throw std::runtime_error("callback failed");
                                             }));
    EXPECT_EQ(calls, 1);
    static_cast<void>(SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER));
}

TEST(SaveFileDialogTest, AFolderCallbackThatThrowsDoesNotEscapeIntoSdl)
{
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none-for-tests"));
    int calls = 0;
    EXPECT_NO_THROW(Core::showFolderDialog(nullptr,
                                           "",
                                           [&](const Core::SaveFileResult& /*result*/)
                                           {
                                               ++calls;
                                               throw 42; // NOLINT(hicpp-exception-baseclass): any throw, not only std::exception
                                           }));
    EXPECT_EQ(calls, 1);
    static_cast<void>(SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER));
}

TEST(SaveFileDialogTest, AnAllocationFailureBuildingTheResultStillCallsBackOnceWithAnError)
{
    // Callers clear their busy state (Save greyed out, the dump's folder choice) only in the callback, so
    // it must come even when copying SDL's error into the result throws std::bad_alloc (#2415). Fails
    // each allocation of the call in turn: one before SDL holds the request escapes showSaveFileDialog()
    // with no callback (nothing was shown); every later one must still call back exactly once.
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none-for-tests"));
    int resultFailures = 0;
    for (std::int64_t failAt = 0; failAt < 64; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        int calls = 0;
        std::optional<Core::SaveFileResult> got;
        bool threw = false;
        bool pending = false;
        {
            const TestSupport::FailAllocationAfter failure(failAt);
            try
            {
                Core::showSaveFileDialog(nullptr,
                                         {},
                                         "",
                                         [&](Core::SaveFileResult result)
                                         {
                                             ++calls;
                                             got = std::move(result);
                                         });
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
            pending = TestSupport::allocationFailurePending();
        }

        if (threw)
        {
            EXPECT_EQ(calls, 0);
        }
        else
        {
            EXPECT_EQ(calls, 1);
            const Core::SaveFileResult result = got.value_or(Core::SaveFileResult{});
            EXPECT_TRUE(result.path.empty());
            EXPECT_FALSE(result.error.empty());
            if (!pending && !result.error.contains("File dialog driver unsupported"))
            {
                ++resultFailures; // the failed allocation was SDL's error string
            }
        }
        if (pending)
        {
            break; // the call made fewer allocations than failAt: every one of them has been failed
        }
    }
    EXPECT_GT(resultFailures, 0);
    static_cast<void>(SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER));
#endif
}

TEST(SaveFileDialogTest, TheDefaultFolderIsDocumentsElseHome)
{
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    std::string expected;
    if (documents != nullptr && *documents != '\0')
    {
        expected = documents;
    }
    else if (home != nullptr && *home != '\0')
    {
        expected = home;
    }
    EXPECT_EQ(Core::defaultSaveFolder(), expected);
}

} // namespace
