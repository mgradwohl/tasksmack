#pragma once

// Pure list operations behind Theme::loadThemes(), kept free of ImGui so they can be unit-tested
// (Theme.cpp itself is not linked into the tests).

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace UI::ThemeCatalog
{

/// Index of the theme with `id` in `themes`, if any.
template<typename Info> [[nodiscard]] std::optional<std::size_t> indexOfId(const std::vector<Info>& themes, std::string_view id)
{
    const auto it = std::ranges::find_if(themes, [id](const Info& theme) { return theme.id == id; });
    if (it == themes.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - themes.begin());
}

/// Merges `incoming` themes (and their parallel `incomingSchemes`) into `themes`/`schemes`: a theme
/// whose id is already present replaces it, any other is added. The result is sorted by name, as
/// discovery sorts a single directory. User themes are layered over the built-ins this way instead of
/// replacing the whole list, which hid every built-in theme as soon as one user theme existed (#1127).
template<typename Info, typename Scheme>
void mergeById(std::vector<Info>& themes, std::vector<Scheme>& schemes, std::vector<Info> incoming, std::vector<Scheme> incomingSchemes)
{
    for (std::size_t i = 0; i < incoming.size() && i < incomingSchemes.size(); ++i)
    {
        if (const auto existing = indexOfId(themes, incoming[i].id))
        {
            themes[*existing] = std::move(incoming[i]);
            schemes[*existing] = std::move(incomingSchemes[i]);
        }
        else
        {
            themes.push_back(std::move(incoming[i]));
            schemes.push_back(std::move(incomingSchemes[i]));
        }
    }

    std::vector<std::size_t> order(themes.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::stable_sort(order, [&themes](std::size_t a, std::size_t b) { return themes[a].name < themes[b].name; });

    std::vector<Info> sortedThemes;
    std::vector<Scheme> sortedSchemes;
    sortedThemes.reserve(themes.size());
    sortedSchemes.reserve(schemes.size());
    for (const std::size_t index : order)
    {
        sortedThemes.push_back(std::move(themes[index]));
        sortedSchemes.push_back(std::move(schemes[index]));
    }
    themes = std::move(sortedThemes);
    schemes = std::move(sortedSchemes);
}

} // namespace UI::ThemeCatalog
