#include "RenderMetricsOverride.h"

#include "Core/EnvRead.h"
#include "Core/Utf8Path.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace App::RenderMetricsOverride
{

std::expected<void, std::string> writeCsv(const std::filesystem::path& path, const std::string_view csv)
{
    // The path itself, not a narrowed copy: std::ofstream opens it by its native encoding (Utf8Path.h).
    // Binary, so the CSV's "\n" line ends are the clipboard export's, byte for byte, on Windows too; an
    // ofstream's default `out` mode truncates, so an existing file is replaced.
    std::ofstream file(path, std::ios::binary);
    if (file)
    {
        file.write(csv.data(), static_cast<std::streamsize>(csv.size()));
        file.close();
    }
    if (!file)
    {
        const int error = errno;
        return std::unexpected(std::format(
            "couldn't write {}: {}", Core::pathToUtf8(path), error != 0 ? std::generic_category().message(error) : "write failed"));
    }
    return {};
}

const std::optional<std::filesystem::path>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<std::filesystem::path> path = []() -> std::optional<std::filesystem::path>
    {
        std::optional<std::filesystem::path> parsed = parse(Core::readEnv(ENV_VAR));
        if (parsed)
        {
            spdlog::info("{} is set: the Render Metrics overlay is shown from the first frame and its CSV is written to {} at exit",
                         ENV_VAR,
                         Core::pathToUtf8(*parsed));
        }
        return parsed;
    }();
    return path;
}

} // namespace App::RenderMetricsOverride
