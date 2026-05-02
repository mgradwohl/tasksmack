#include "PathService.h"

#include "Platform/Factory.h"

#include <spdlog/spdlog.h>

#include <string>
#include <system_error>

namespace Core
{

namespace
{

// Normalize a raw path from a platform provider to an absolute, cleaned path.
// Uses the error_code overload to avoid throwing.
//
// Fallback order when absolute() fails:
//   1. If raw is relative, try (current_path() / raw).lexically_normal().
//   2. Return raw.lexically_normal() as a last resort.
//
// NOTE: the final fallback can return a relative path only when BOTH
// std::filesystem::absolute() AND std::filesystem::current_path() fail
// simultaneously. This can only occur in a severely broken environment (e.g.,
// an unmounted filesystem or a chroot where the kernel refuses all CWD queries).
// In that state the application cannot locate any of its assets or config, so
// the relaxed invariant is acceptable. A warning is emitted so the condition is
// at least observable at runtime.
std::filesystem::path toAbsolute(const std::filesystem::path& raw)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(raw, ec);
    if (!ec)
    {
        return abs.lexically_normal();
    }

    // absolute() failed — try composing with current_path() for relative inputs.
    if (raw.is_relative())
    {
        std::error_code cwdEc;
        std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
        if (!cwdEc)
        {
            return (cwd / raw).lexically_normal();
        }
    }

    // Both absolute() and current_path() failed.  The returned path may not be
    // absolute, violating the normal contract of PathService.  Log a warning so
    // this degenerate condition is visible; the caller should not rely on the
    // result being absolute.
    //
    // All logging below is best-effort: raw.string() can throw when the path
    // contains characters that cannot be represented in the current locale
    // encoding, and spdlog::warn itself can throw.  Each warn call is wrapped in
    // its own try/catch so that no exception can escape from this fallback path.
    try
    {
        const std::string rawStr = raw.empty() ? "<empty>" : raw.string();
        try
        {
            spdlog::warn("PathService: could not resolve absolute path for '{}'; "
                         "both absolute() and current_path() failed. "
                         "Returning lexically-normalized raw path.",
                         rawStr);
        }
        catch (...)
        {}
    }
    catch (...)
    {
        try
        {
            spdlog::warn("PathService: could not resolve absolute path (path contains "
                         "non-representable characters); both absolute() and current_path() failed. "
                         "Returning lexically-normalized raw path.");
        }
        catch (...)
        {}
    }
    return raw.lexically_normal();
}

} // namespace

PathService::PathService()
{
    auto provider = Platform::makePathProvider();
    m_ExecutableDir = toAbsolute(provider->getExecutableDir());
    m_UserConfigDir = toAbsolute(provider->getUserConfigDir());
}

const std::filesystem::path& PathService::executableDir() const noexcept
{
    return m_ExecutableDir;
}

const std::filesystem::path& PathService::userConfigDir() const noexcept
{
    return m_UserConfigDir;
}

} // namespace Core
