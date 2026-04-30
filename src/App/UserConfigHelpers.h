#pragma once

#include "Domain/Numeric.h"

#include <algorithm>
#include <filesystem>
#include <functional>

#include <toml++/toml.hpp>

namespace App::UserConfigHelpers
{
/// Loads a value from a TOML table section and applies clamping.
/// If the key is not found, the destination is left unchanged.
///
/// @tparam T The type of the value to load
/// @tparam ClampFn A callable that accepts T and returns T (clamped value)
/// @param config The TOML table to read from
/// @param section The section name (nested key)
/// @param key The key name within the section
/// @param destination Output parameter; updated if key exists
/// @param clampFn A callable that applies validation/clamping logic
template<typename T, typename ClampFn>
inline void loadAndClamp(const toml::table& config, const char* section, const char* key, T& destination, ClampFn&& clampFn)
{
    if (auto val = config[section][key].template value<T>())
    {
        destination = std::invoke(std::forward<ClampFn>(clampFn), *val);
    }
}

/// Loads an int64 value from a TOML table, narrows it to int, and applies clamping.
/// If the key is not found, the destination is left unchanged.
///
/// @param config The TOML table to read from
/// @param section The section name (nested key)
/// @param key The key name within the section
/// @param destination Output parameter; updated if key exists
/// @param defaultValue Default value if narrowing would cause underflow
/// @tparam ClampFn A callable that accepts int and returns int (clamped)
/// @param clampFn A callable that applies validation/clamping logic
template<typename ClampFn>
inline void
loadAndNarrowInt64(const toml::table& config, const char* section, const char* key, int& destination, int defaultValue, ClampFn&& clampFn)
{
    if (auto val = config[section][key].template value<std::int64_t>())
    {
        destination = std::invoke(std::forward<ClampFn>(clampFn), Domain::Numeric::narrowOr<int>(*val, defaultValue));
    }
}

/// Loads an int64 value from a TOML table, narrows it to int, and clamps to [minVal, maxVal].
/// If the key is not found, the destination is left unchanged.
///
/// @param config The TOML table to read from
/// @param section The section name (nested key)
/// @param key The key name within the section
/// @param destination Output parameter; updated if key exists
/// @param defaultValue Default value if narrowing would cause underflow
/// @param minVal Minimum allowed value (inclusive)
/// @param maxVal Maximum allowed value (inclusive)
inline void loadAndNarrowIntWithClamp(
    const toml::table& config, const char* section, const char* key, int& destination, int defaultValue, int minVal, int maxVal)
{
    if (auto val = config[section][key].template value<std::int64_t>())
    {
        const int narrowed = Domain::Numeric::narrowOr<int>(*val, defaultValue);
        destination = std::clamp(narrowed, minVal, maxVal);
    }
}

/// Validates a candidate config directory path derived from user/environment input.
/// Returns true if the path is safe to use as a config directory:
///   - must be absolute after lexical normalization
///   - must not contain any ".." traversal components after normalization
///
/// This is a purely lexical check (no filesystem access). Callers that need
/// symlink-safe validation should resolve the path with weakly_canonical() first
/// and then call this function on the result.
[[nodiscard]] inline auto isValidConfigDir(const std::filesystem::path& candidate) -> bool
{
    const auto normalized = candidate.lexically_normal();
    if (!normalized.is_absolute())
    {
        return false;
    }
    return !std::ranges::any_of(normalized, [](const auto& part) { return part == ".."; });
}

} // namespace App::UserConfigHelpers
