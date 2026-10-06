// Fuzzes theme file parsing: toml++ ingestion plus every colour lookup and conversion the theme
// loader performs, through ThemeLoader::loadThemeFromString -- the code loadTheme() runs on a file.

#include "UI/ThemeLoader.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
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

    (void) UI::ThemeLoader::loadThemeFromString(text, "fuzz-input.toml");
    return 0;
}
