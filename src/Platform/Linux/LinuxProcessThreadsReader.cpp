#include "LinuxProcessThreadsReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "PosixGuards.h"
#include "ProcParsing.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

[[nodiscard]] ThreadsReadResult failure(int err)
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}};
    case ENOENT:
    case ESRCH:
        return {.status = ThreadsReadStatus::ProcessExited, .threads = {}, .detail = {}};
    default:
        return {.status = ThreadsReadStatus::Failed, .threads = {}, .detail = std::system_category().message(err)};
    }
}

/// A /proc/[pid]/task entry's name as a thread ID; nullopt for ".", ".." and anything else.
[[nodiscard]] std::optional<std::int32_t> threadIdOf(std::string_view name) noexcept
{
    std::int32_t tid = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), tid);
    if (error != std::errc{} || end != name.data() + name.size() || tid <= 0)
    {
        return std::nullopt;
    }
    return tid;
}

/// Owns a DIR*.
class DirGuard
{
  public:
    explicit DirGuard(DIR* dir) noexcept : m_Dir(dir)
    {}
    ~DirGuard()
    {
        if (m_Dir != nullptr)
        {
            ::closedir(m_Dir);
        }
    }
    DirGuard(const DirGuard&) = delete;
    DirGuard& operator=(const DirGuard&) = delete;
    DirGuard(DirGuard&&) = delete;
    DirGuard& operator=(DirGuard&&) = delete;

    [[nodiscard]] DIR* get() const noexcept
    {
        return m_Dir;
    }

  private:
    DIR* m_Dir = nullptr;
};

} // namespace

bool LinuxProcessThreadsReader::hasThreads() const
{
    return true;
}

ThreadsReadResult LinuxProcessThreadsReader::readThreads(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as the other Details readers do.
        return {.status = ThreadsReadStatus::IdentityUnknown, .threads = {}, .detail = {}};
    }

    // One handle on the process's /proc directory, which keeps naming this process once open.
    const std::string dirPath = m_ProcRoot + "/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(errno);
    }
    if (ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // gone, or the PID now names another process
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const int taskFd = ::openat(dir.get(), "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (taskFd < 0)
    {
        return failure(errno);
    }
    // fdopendir() takes the descriptor over: closedir() closes it.
    const DirGuard task(::fdopendir(taskFd));
    if (task.get() == nullptr)
    {
        const int err = errno;
        ::close(taskFd);
        return failure(err);
    }

    ThreadsReadResult result{.status = ThreadsReadStatus::Ok, .threads = {}, .detail = {}};
    result.ticksPerSecond = std::max<long>(1, ::sysconf(_SC_CLK_TCK));
    std::array<char, 1024> buf{};
    errno = 0;
    while (const dirent* entry = ::readdir(task.get())) // NOLINT(concurrency-mt-unsafe) - this DIR is the worker's own
    {
        const std::optional<std::int32_t> tid = threadIdOf(entry->d_name);
        if (!tid.has_value())
        {
            continue;
        }
        if (result.threads.size() >= MAX_THREADS)
        {
            result.truncated = true;
            break;
        }
        const std::string statPath = std::to_string(*tid) + "/stat";
        const std::size_t len = ProcParsing::readProcFileOnceAt(::dirfd(task.get()), statPath.c_str(), buf.data(), buf.size());
        const std::optional<ProcParsing::StatFields> fields = ProcParsing::parseStatFields(std::string_view(buf.data(), len));
        if (!fields.has_value())
        {
            continue; // exited since the listing
        }
        result.threads.push_back({.tid = *tid,
                                  .name = std::string(fields->comm),
                                  .state = fields->state,
                                  .userTicks = fields->userTime,
                                  .systemTicks = fields->systemTime,
                                  .nice = fields->nice,
                                  .startTicks = fields->startTime});
    }
    // The process exited while it was listed (an exiting process's task directory reads empty), or
    // its PID changed hands: not shown with what was read.
    if (ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks || result.threads.empty())
    {
        return failure(ESRCH);
    }
    std::ranges::sort(result.threads, {}, &ProcessThread::tid);
    return result;
}

} // namespace Platform
