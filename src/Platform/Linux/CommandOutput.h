#pragma once

/// @file CommandOutput.h
/// @brief Running a short read-only command and keeping what it prints (#3425: `crontab -l`, which only the setgid
/// crontab program can read for an ordinary user).

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace Platform
{

/// What runForOutput() saw.
struct CommandOutput
{
    bool started = false;  ///< It ran at all (false: not found, or no process could be made)
    bool timedOut = false; ///< Killed for outliving the timeout (still running then, even with its output closed);
                           ///< output holds what it printed by then
    int exitCode = -1;     ///< Its exit status, or -1 when it didn't exit by itself
    std::string output;    ///< Its standard output, up to the byte limit
};

/// Runs @p argv (the program looked up on PATH) with stdin and stderr on /dev/null and no descriptor of TaskSmack's,
/// and returns its standard output. Kills it (SIGKILL) if it hasn't finished within @p timeout, and keeps at most
/// @p maxBytes of output. A child that closes its output but keeps running is still killed at the timeout, and the
/// child is killed and reaped on every way out, an exception included. Blocking, for at most about @p timeout: call
/// it from a sampler or worker thread, never the UI thread.
[[nodiscard]] CommandOutput runForOutput(const std::vector<std::string>& argv, std::chrono::milliseconds timeout, std::size_t maxBytes);

} // namespace Platform
