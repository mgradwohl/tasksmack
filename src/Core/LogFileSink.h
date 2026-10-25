#pragma once

// The log file's spdlog sink, opened at exactly the path given, whatever characters it holds (#1648).
//
// Windows: SPDLOG_WCHAR_FILENAMES is defined for every target that builds this (CMakeLists.txt's
// tasksmack_configure_app_target and the test targets), so spdlog's filename_t is std::wstring and
// the file is opened with _wfsopen on the native UTF-16 path. Without it spdlog opens a narrow
// std::string with _fsopen, which goes through the active code page. That is UTF-8 in TaskSmack.exe
// (activeCodePage in its manifest), but the system's code page in every binary without the manifest:
// TaskSmackUiTraining and the tests. There a %TEMP% under a CJK or emoji user name can't be
// represented at all, and an accented one names a different file. tests/App/test_NonAsciiRoundTrips.cpp
// opens a log under such a directory without the manifest and checks the file lands at that path.
//
// Linux: a path is bytes, and spdlog takes them as they are.

#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>

namespace Core
{

#ifdef _WIN32
static_assert(std::is_same_v<spdlog::filename_t, std::wstring>,
              "Define SPDLOG_WCHAR_FILENAMES for this target: the log file must be opened by its UTF-16 path (#1648)");
#endif

/// A file sink writing to @p path (created with its directories if missing; truncated when
/// @p truncate). Throws spdlog::spdlog_ex if the file can't be opened.
[[nodiscard]] inline std::shared_ptr<spdlog::sinks::basic_file_sink_mt> makeLogFileSink(const std::filesystem::path& path, bool truncate)
{
    // path.native() is spdlog's filename_t on both platforms: std::wstring on Windows (see above),
    // the path's bytes on Linux. No conversion, so nothing to lose.
    return std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), truncate);
}

} // namespace Core
