/// @file test_ThemeCatalog.cpp
/// @brief Tests for UI::ThemeCatalog, the list merge behind Theme::loadThemes() (#1127)

#include "UI/ThemeCatalog.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace UI
{
namespace
{

struct Info
{
    std::string id;
    std::string name;
};

struct Scheme
{
    std::string marker;
};

std::vector<std::string> ids(const std::vector<Info>& themes)
{
    std::vector<std::string> out;
    out.reserve(themes.size());
    for (const auto& theme : themes)
    {
        out.push_back(theme.id);
    }
    return out;
}

TEST(ThemeCatalogTest, UserThemesAreAddedAlongsideBuiltIns)
{
    std::vector<Info> themes{{.id = "arctic-fire", .name = "Arctic Fire"}, {.id = "dracula", .name = "Dracula"}};
    std::vector<Scheme> schemes{{.marker = "builtin-arctic"}, {.marker = "builtin-dracula"}};

    ThemeCatalog::mergeById(themes, schemes, {{.id = "mine", .name = "Mine"}}, {{.marker = "user-mine"}});

    EXPECT_EQ(ids(themes), (std::vector<std::string>{"arctic-fire", "dracula", "mine"}));
    ASSERT_EQ(schemes.size(), 3U);
    EXPECT_EQ(schemes[2].marker, "user-mine");
}

TEST(ThemeCatalogTest, UserThemeOverridesBuiltInWithTheSameId)
{
    std::vector<Info> themes{{.id = "arctic-fire", .name = "Arctic Fire"}, {.id = "dracula", .name = "Dracula"}};
    std::vector<Scheme> schemes{{.marker = "builtin-arctic"}, {.marker = "builtin-dracula"}};

    ThemeCatalog::mergeById(themes, schemes, {{.id = "dracula", .name = "Dracula"}}, {{.marker = "user-dracula"}});

    ASSERT_EQ(themes.size(), 2U);
    const auto dracula = ThemeCatalog::indexOfId(themes, "dracula");
    const auto arctic = ThemeCatalog::indexOfId(themes, "arctic-fire");
    ASSERT_TRUE(dracula.has_value() && arctic.has_value());
    EXPECT_EQ(schemes[dracula.value()].marker, "user-dracula");
    EXPECT_EQ(schemes[arctic.value()].marker, "builtin-arctic");
}

TEST(ThemeCatalogTest, MergedListIsSortedByNameWithSchemesKeptParallel)
{
    std::vector<Info> themes{{.id = "b", .name = "Bravo"}, {.id = "d", .name = "Delta"}};
    std::vector<Scheme> schemes{{.marker = "b"}, {.marker = "d"}};

    ThemeCatalog::mergeById(
        themes, schemes, {{.id = "c", .name = "Charlie"}, {.id = "a", .name = "Alpha"}}, {{.marker = "c"}, {.marker = "a"}});

    EXPECT_EQ(ids(themes), (std::vector<std::string>{"a", "b", "c", "d"}));
    for (std::size_t i = 0; i < themes.size(); ++i)
    {
        EXPECT_EQ(schemes[i].marker, themes[i].id);
    }
}

TEST(ThemeCatalogTest, IndexOfIdFindsPresentAndMissingIds)
{
    const std::vector<Info> themes{{.id = "x", .name = "X"}, {.id = "y", .name = "Y"}};
    EXPECT_EQ(ThemeCatalog::indexOfId(themes, "y"), 1U);
    EXPECT_FALSE(ThemeCatalog::indexOfId(themes, "z").has_value());
}

} // namespace
} // namespace UI
