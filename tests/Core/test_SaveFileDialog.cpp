/// @file test_SaveFileDialog.cpp
/// @brief Core::showSaveFileDialog() and Core::defaultSaveFolder() (#1866, #1891): with no dialog
/// driver SDL can use, the callback gets SDL's reason once, at once; the default folder is Documents,
/// else home, as SDL finds them.

#include "Core/SaveFileDialog.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

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
