#pragma once

#include "Domain/Numeric.h"

#include <algorithm>
#include <functional>

#include <toml++/toml.hpp>

namespace App::UserConfigHelpers
{
template<typename T, typename ClampFn>
inline void loadAndClamp(const toml::table& config, const char* section, const char* key, T& destination, ClampFn&& clampFn)
{
    if (auto val = config[section][key].template value<T>())
    {
        destination = std::invoke(std::forward<ClampFn>(clampFn), *val);
    }
}

template<typename ClampFn>
inline void
loadAndNarrowInt64(const toml::table& config, const char* section, const char* key, int& destination, int defaultValue, ClampFn&& clampFn)
{
    if (auto val = config[section][key].template value<std::int64_t>())
    {
        destination = std::invoke(std::forward<ClampFn>(clampFn), Domain::Numeric::narrowOr<int>(*val, defaultValue));
    }
}

inline void loadAndNarrowInt(
    const toml::table& config, const char* section, const char* key, int& destination, int defaultValue, int minVal, int maxVal)
{
    if (auto val = config[section][key].template value<std::int64_t>())
    {
        const int narrowed = Domain::Numeric::narrowOr<int>(*val, defaultValue);
        destination = std::clamp(narrowed, minVal, maxVal);
    }
}

} // namespace App::UserConfigHelpers
