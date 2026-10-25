// Fuzzes theme file parsing: toml++ ingestion plus every colour lookup and conversion the theme
// loader performs, through ThemeLoader::loadThemeFromString -- the code loadTheme() runs on a file.

#include "UI/Theme.h"
#include "UI/ThemeLoader.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) // NOLINT(readability-identifier-naming)
{
    // The loader warns once per missing or malformed colour; silence it so the fuzzer fuzzes.
    spdlog::set_level(spdlog::level::off);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    // Any object representation may be inspected through char pointers.
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const std::optional<UI::ColorScheme> scheme = UI::ThemeLoader::loadThemeFromString(text, "fuzz-input.toml");
    if (!scheme.has_value())
    {
        return 0;
    }

    // Invariant (#2318): every colour the loader returns is finite and in [0, 1], so ImGui's
    // float-to-byte conversion is defined for it. The accents and semantic colours read straight
    // from [r, g, b, a] arrays (both of the loader's array paths); the derived colours follow them.
    const auto checkColour = [](const ImVec4& colour)
    {
        for (const float component : {colour.x, colour.y, colour.z, colour.w})
        {
            if (!std::isfinite(component) || component < 0.0F || component > 1.0F)
            {
                std::abort();
            }
        }
    };
    for (const ImVec4& accent : scheme->accents)
    {
        checkColour(accent);
    }
    for (const ImVec4& colour : {scheme->textPrimary,
                                 scheme->textDisabled,
                                 scheme->textMuted,
                                 scheme->textError,
                                 scheme->textWarning,
                                 scheme->textSuccess,
                                 scheme->textInfo,
                                 scheme->chartCpu,
                                 scheme->chartMemory,
                                 scheme->chartIo,
                                 scheme->windowBg})
    {
        checkColour(colour);
    }
    return 0;
}
