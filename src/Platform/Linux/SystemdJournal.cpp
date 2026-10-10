#include "SystemdJournal.h"

#include "Platform/Linux/LinuxCoredumps.h"

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

#include <systemd/sd-journal.h>
#endif

namespace Platform::SystemdJournal
{

#ifdef TASKSMACK_HAS_SDBUS

namespace
{

/// systemd-coredump's message ID (catalog entry "Process dumped core").
constexpr const char* COREDUMP_MATCH = "MESSAGE_ID=fc2e22bc6ee647b6b90729ab34a250b1";

/// The fields read, without their COREDUMP_ prefix.
constexpr std::array FIELDS{"PID", "UID", "SIGNAL", "SIGNAL_NAME", "TIMESTAMP", "COMM", "EXE", "FILENAME"};

/// Longest field value kept: an executable path or file name is far shorter.
constexpr std::size_t FIELD_LIMIT = 4096;

/// Owns an sd_journal.
class Journal
{
  public:
    Journal() = default;
    ~Journal()
    {
        sd_journal_close(m_Journal);
    }
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) = delete;
    Journal& operator=(Journal&&) = delete;

    [[nodiscard]] sd_journal** out() noexcept
    {
        return &m_Journal;
    }

    [[nodiscard]] sd_journal* get() const noexcept
    {
        return m_Journal;
    }

  private:
    sd_journal* m_Journal = nullptr;
};

[[nodiscard]] std::string errnoText(int result)
{
    return std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - read on one worker thread
}

LinuxCoredumps::JournalRead readCoredumpEntries(std::uint64_t sinceUsec, std::size_t maxEntries)
{
    LinuxCoredumps::JournalRead read;
    Journal journal;
    if (const int result = sd_journal_open(journal.out(), SD_JOURNAL_LOCAL_ONLY); result < 0)
    {
        read.error = "The journal couldn't be opened: " + errnoText(result);
        return read;
    }
    sd_journal* handle = journal.get();
    // Keeps sd_journal_get_data() from decompressing more of a field than is kept: the COREDUMP field
    // holding a core stored in the journal is never asked for, but this bounds the others too.
    (void) sd_journal_set_data_threshold(handle, FIELD_LIMIT);
    if (const int result = sd_journal_add_match(handle, COREDUMP_MATCH, 0); result < 0)
    {
        read.error = "The journal couldn't be searched: " + errnoText(result);
        return read;
    }
    if (const int result = sd_journal_seek_tail(handle); result < 0)
    {
        read.error = "The journal couldn't be read: " + errnoText(result);
        return read;
    }
    read.opened = true;
    while (read.entries.size() < maxEntries && sd_journal_previous(handle) > 0)
    {
        std::uint64_t usec = 0;
        if (sd_journal_get_realtime_usec(handle, &usec) >= 0 && usec < sinceUsec)
        {
            break; // Newest first: everything from here on is older
        }
        LinuxCoredumps::JournalFields fields;
        for (const char* field : FIELDS)
        {
            const std::string name = std::string("COREDUMP_") + field;
            const void* data = nullptr;
            std::size_t length = 0;
            if (sd_journal_get_data(handle, name.c_str(), &data, &length) < 0 || data == nullptr)
            {
                continue;
            }
            // "COREDUMP_PID=4242": the value follows the name and its '='.
            const std::string_view entry(static_cast<const char*>(data), length);
            if (entry.size() > name.size() && entry[name.size()] == '=')
            {
                fields.emplace_back(field, std::string(entry.substr(name.size() + 1)));
            }
        }
        read.entries.push_back(std::move(fields));
    }
    return read;
}

} // namespace

LinuxCoredumps::JournalReader makeCoredumpJournalReader()
{
    return &readCoredumpEntries;
}

#else

LinuxCoredumps::JournalReader makeCoredumpJournalReader()
{
    return [](std::uint64_t, std::size_t)
    {
        LinuxCoredumps::JournalRead read;
        read.error = "This build has no systemd support (it was built without libsystemd)";
        return read;
    };
}

#endif

} // namespace Platform::SystemdJournal
