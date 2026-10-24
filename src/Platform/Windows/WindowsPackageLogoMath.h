#pragma once

// Which file to read for a packaged app's logo (#1800). A package manifest names its logo as one
// file -- the Application's VisualElements Square44x44Logo, e.g. "Assets\Square44x44Logo.png" -- but
// the package holds it as resource variants beside that name, qualified by scale or target size:
//
//   Square44x44Logo.scale-200.png                      44 px at 200 %: 88 px
//   Square44x44Logo.targetsize-24.png                  24 px, drawn on the app's coloured plate
//   Square44x44Logo.targetsize-24_altform-unplated.png 24 px with no plate, as Task Manager draws it
//   Square44x44Logo.scale-100_contrast-black.png       for a high-contrast theme
//
// and the unqualified name may not exist at all. The pick is the variant nearest the size asked for
// (the smallest at least as large, else the largest), the unplated target sizes first among equals.
// High-contrast, light-theme and other variants TaskSmack doesn't draw are left out. Pure (file
// names in, an index out, no windows.h), so it is tested on every platform
// (tests/Platform/WindowsMath/test_WindowsPackageLogoMath.cpp).

#include "Platform/AsciiCase.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Platform::Windows::PackageLogo
{

/// What a candidate file is, as a variant of a logo.
struct Variant
{
    int sizePx = 0; ///< Its side in pixels: the target size, or the logo's size at the scale.
    int rank = 0;   ///< Among variants of one size, lower is better: 0 unplated target size, 1 target size, 2 scale.
};

/// The logo's size at 100 %: the first "<N>x" in its name ("Square44x44Logo" is 44); 44, the size of
/// the logo Square44x44Logo is, when there is none.
[[nodiscard]] constexpr int nominalSize(std::string_view stem) noexcept
{
    constexpr int DEFAULT_SIZE = 44;
    constexpr int MAX_SIZE = 4096;
    std::size_t pos = 0;
    while (pos < stem.size())
    {
        std::size_t end = pos;
        int value = 0;
        while (end < stem.size() && stem[end] >= '0' && stem[end] <= '9' && value <= MAX_SIZE)
        {
            value = (value * 10) + (stem[end] - '0');
            ++end;
        }
        if (end > pos && end < stem.size() && (stem[end] == 'x' || stem[end] == 'X') && value > 0 && value <= MAX_SIZE)
        {
            return value;
        }
        pos = (end > pos) ? end : pos + 1;
    }
    return DEFAULT_SIZE;
}

/// @p text in ASCII lower case: package file names compare case-insensitively.
[[nodiscard]] inline std::string lowerAscii(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), Platform::foldAscii);
    return lower;
}

/// The positive number @p text is, all digits; nullopt for anything else or a number past 4096.
[[nodiscard]] constexpr std::optional<int> parseQualifierNumber(std::string_view text) noexcept
{
    constexpr int MAX_VALUE = 4096;
    if (text.empty())
    {
        return std::nullopt;
    }
    int value = 0;
    std::size_t index = 0;
    while (index < text.size())
    {
        const char c = text[index];
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
        if (value > MAX_VALUE)
        {
            return std::nullopt;
        }
        ++index;
    }
    return value > 0 ? std::optional<int>(value) : std::nullopt;
}

/// What @p candidate is as a variant of logo @p logoFileName (both file names, no folders): the
/// logo itself is its size at 100 %; "<stem>.<qualifiers><ext>" with a scale or a target size, and
/// optionally altform-unplated, is that size. nullopt for another file, and for a variant with any
/// other qualifier (contrast-*, altform-lightunplated, theme-*, lang-* and the like).
[[nodiscard]] inline std::optional<Variant> variantOf(std::string_view logoFileName, std::string_view candidate)
{
    const std::string logo = lowerAscii(logoFileName);
    const std::string file = lowerAscii(candidate);
    const std::size_t dot = logo.rfind('.');
    if (dot == std::string::npos || dot == 0)
    {
        return std::nullopt;
    }
    const std::string_view stem = std::string_view(logo).substr(0, dot);
    const std::string_view extension = std::string_view(logo).substr(dot);
    const int nominal = nominalSize(stem);
    if (file == logo)
    {
        return Variant{.sizePx = nominal, .rank = 2};
    }
    const std::size_t prefix = stem.size() + 1;
    if (file.size() <= prefix + extension.size() || !file.starts_with(stem) || file[stem.size()] != '.' || !file.ends_with(extension))
    {
        return std::nullopt;
    }

    constexpr std::string_view SCALE = "scale-";
    constexpr std::string_view TARGET_SIZE = "targetsize-";
    constexpr int PERCENT = 100;
    const std::string_view qualifiers = std::string_view(file).substr(prefix, file.size() - prefix - extension.size());
    std::optional<int> scale;
    std::optional<int> targetSize;
    bool unplated = false;
    std::size_t start = 0;
    while (start <= qualifiers.size())
    {
        std::size_t end = qualifiers.find('_', start);
        if (end == std::string_view::npos)
        {
            end = qualifiers.size();
        }
        const std::string_view qualifier = qualifiers.substr(start, end - start);
        if (qualifier.starts_with(SCALE) && !scale)
        {
            scale = parseQualifierNumber(qualifier.substr(SCALE.size()));
            if (!scale)
            {
                return std::nullopt;
            }
        }
        else if (qualifier.starts_with(TARGET_SIZE) && !targetSize)
        {
            targetSize = parseQualifierNumber(qualifier.substr(TARGET_SIZE.size()));
            if (!targetSize)
            {
                return std::nullopt;
            }
        }
        else if (qualifier == "altform-unplated")
        {
            unplated = true;
        }
        else
        {
            return std::nullopt; // contrast, light theme, language... or malformed
        }
        start = end + 1;
    }
    if (targetSize)
    {
        return Variant{.sizePx = *targetSize, .rank = unplated ? 0 : 1};
    }
    if (scale && !unplated)
    {
        return Variant{.sizePx = (nominal * *scale) / PERCENT, .rank = 2};
    }
    return std::nullopt;
}

/// The index in @p candidates (file names in the logo's folder) of the file to read for logo
/// @p logoFileName drawn @p sizePx square: of its variants, the smallest at least @p sizePx, else
/// the largest; among variants of one size, the better rank, then the first listed. nullopt when
/// none of them is a variant of the logo.
[[nodiscard]] inline std::optional<std::size_t> pickLogo(std::string_view logoFileName, std::span<const std::string> candidates, int sizePx)
{
    std::optional<std::size_t> best;
    Variant bestVariant;
    std::size_t index = 0;
    while (index < candidates.size())
    {
        const std::optional<Variant> variant = variantOf(logoFileName, candidates[index]);
        if (variant && variant->sizePx > 0)
        {
            bool better = !best.has_value();
            if (!better)
            {
                const bool fits = variant->sizePx >= sizePx;
                const bool bestFits = bestVariant.sizePx >= sizePx;
                if (fits != bestFits)
                {
                    better = fits;
                }
                else if (variant->sizePx != bestVariant.sizePx)
                {
                    // Both large enough: the smaller shrinks least. Both too small: the larger grows least.
                    better = fits ? (variant->sizePx < bestVariant.sizePx) : (variant->sizePx > bestVariant.sizePx);
                }
                else
                {
                    better = variant->rank < bestVariant.rank;
                }
            }
            if (better)
            {
                best = index;
                bestVariant = *variant;
            }
        }
        ++index;
    }
    return best;
}

} // namespace Platform::Windows::PackageLogo
