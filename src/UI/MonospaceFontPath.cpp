#include "MonospaceFontPath.h"

#include <array>
#include <system_error>

namespace UI
{

std::filesystem::path selectMonospaceFontPath(std::span<const std::filesystem::path> candidates,
                                              const std::function<bool(const std::filesystem::path&)>& probeExists)
{
    for (const auto& candidate : candidates)
    {
        if (probeExists(candidate))
        {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path findMonospaceFontPath()
{
#ifdef _WIN32
    // Prefer Consolas, fall back to Cascadia Mono if available.
    static const std::array<std::filesystem::path, 2> CANDIDATES = {
        std::filesystem::path(L"C:/Windows/Fonts/consola.ttf"),
        std::filesystem::path(L"C:/Windows/Fonts/CascadiaMono.ttf"),
    };
#else
    // Prefer DejaVu Sans Mono, fall back to Liberation Mono.
    static const std::array<std::filesystem::path, 2> CANDIDATES = {
        std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"),
        std::filesystem::path("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"),
    };
#endif

    // The installed-font set doesn't change over the process lifetime, so cache the resolved
    // path rather than re-probing the filesystem on every call.
    static const std::filesystem::path RESOLVED = selectMonospaceFontPath(CANDIDATES,
                                                                          [](const std::filesystem::path& p)
                                                                          {
                                                                              // Use the error_code overload so filesystem errors (e.g.
                                                                              // permission denied) are treated as a non-match rather than
                                                                              // propagating as exceptions.
                                                                              std::error_code ec;
                                                                              return std::filesystem::exists(p, ec);
                                                                          });
    return RESOLVED;
}

} // namespace UI
