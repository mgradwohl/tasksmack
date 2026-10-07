// Fuzzes config.toml parsing: toml++ ingestion plus TaskSmack's settings schema (readSettings),
// through UserConfig::parseSettings -- the code UserConfig::load() runs on the file's contents.

#include "App/UserConfig.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) // NOLINT(readability-identifier-naming)
{
    // readSettings() logs rejected values; at fuzzing rates that is all the fuzzer would do.
    spdlog::set_level(spdlog::level::off);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    // Any object representation may be inspected through char pointers.
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    App::UserSettings settings;
    (void) App::UserConfig::parseSettings(text, settings);
    return 0;
}
