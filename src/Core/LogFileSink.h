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
//
// What is written is well-formed UTF-8 (#4326, the UTF-8 audit's D1): a message holding a raw comm cut mid-character
// or a Latin-1 path is written with U+FFFD in place of each ill-formed subpart, so a strict UTF-8 reader can open the
// log. Every sink TaskSmack makes (this one, and DebugLogFile's descriptor and handle sinks) passes its messages
// through wellFormedMessage().

#include "Domain/Utf8Sanitize.h"

#include <spdlog/common.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

namespace Core
{

#ifdef _WIN32
static_assert(std::is_same_v<spdlog::filename_t, std::wstring>,
              "Define SPDLOG_WCHAR_FILENAMES for this target: the log file must be opened by its UTF-16 path (#1648)");

#ifndef SPDLOG_UTF8_TO_WCHAR_CONSOLE
#error "Define SPDLOG_UTF8_TO_WCHAR_CONSOLE for this target: the console sink must write UTF-8 log text with WriteConsoleW (#4317)"
#endif
#endif

/// @p msg with its text as well-formed UTF-8 (#4326). Usually @p msg itself; when the text has an ill-formed
/// sequence, a copy whose text is the sanitized form, which @p storage holds and must outlive the copy.
[[nodiscard]] inline spdlog::details::log_msg wellFormedMessage(const spdlog::details::log_msg& msg, std::string& storage)
{
    const std::string_view payload(msg.payload.data(), msg.payload.size());
    if (Domain::isValidUtf8(payload))
    {
        return msg;
    }
    storage = Domain::sanitizedUtf8(payload);
    spdlog::details::log_msg copy = msg;
    copy.payload = spdlog::string_view_t(storage.data(), storage.size());
    return copy;
}

/// A file sink writing every message as well-formed UTF-8 (#4326). spdlog's basic_file_sink is final, so this is the
/// same sink over spdlog's file_helper, with the message passed through wellFormedMessage() before it is formatted.
class WellFormedFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    WellFormedFileSink(const spdlog::filename_t& path, bool truncate)
    {
        m_File.open(path, truncate);
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        std::string storage;
        spdlog::memory_buf_t formatted;
        formatter_->format(wellFormedMessage(msg, storage), formatted);
        m_File.write(formatted);
    }

    void flush_() override
    {
        m_File.flush();
    }

  private:
    spdlog::details::file_helper m_File;
};

/// A file sink writing to @p path (created with its directories if missing; truncated when
/// @p truncate), in well-formed UTF-8. Throws spdlog::spdlog_ex if the file can't be opened.
[[nodiscard]] inline spdlog::sink_ptr makeLogFileSink(const std::filesystem::path& path, bool truncate)
{
    // path.native() is spdlog's filename_t on both platforms: std::wstring on Windows (see above),
    // the path's bytes on Linux. No conversion, so nothing to lose.
    return std::make_shared<WellFormedFileSink>(path.native(), truncate);
}

} // namespace Core
