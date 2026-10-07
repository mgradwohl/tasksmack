#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace Domain
{

/// The handoff point for one model's immutable, versioned publications (#868).
///
/// The writer builds each new generation completely, outside any lock readers take, then calls
/// commit(); load() and version() are all a reader needs. The lock here guards only the pointer
/// swap, a few instructions, so a UI-thread reader never waits for a writer to copy its histories.
/// A short mutex rather than std::atomic<std::shared_ptr>: that is not lock-free on libc++ or MSVC.
///
/// T must expose a std::uint64_t `version` member. Generation numbering is the caller's job: commit
/// generations in increasing version order from one writer at a time (the models serialise their
/// writers on their own writer mutex), and readers never see a torn or regressed generation.
template<typename T> class PublicationSlot
{
  public:
    PublicationSlot() = default;
    ~PublicationSlot() = default;

    PublicationSlot(const PublicationSlot&) = delete;
    PublicationSlot& operator=(const PublicationSlot&) = delete;
    PublicationSlot(PublicationSlot&&) = delete;
    PublicationSlot& operator=(PublicationSlot&&) = delete;

    /// The current generation: a shared_ptr copy under the swap lock, never a deep copy.
    [[nodiscard]] std::shared_ptr<const T> load() const noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Current;
    }

    /// The version of the current generation, without locking. A reader that sees version N here
    /// gets generation N or a later one from a following load(), never an earlier one.
    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Version.load(std::memory_order_acquire);
    }

    /// Publish a fully built generation. The pointer is swapped first and the version stored last,
    /// both under the lock, so the version never runs ahead of the generation load() returns. The
    /// outgoing generation is released after the lock, so freeing its histories (when this drops the
    /// last reference) never happens while a reader waits.
    void commit(std::shared_ptr<const T> next) noexcept
    {
        const std::uint64_t nextVersion = next->version;
        std::shared_ptr<const T> retired; // destroyed at return, after the lock below is released
        {
            const std::scoped_lock lock(m_Mutex);
            retired = std::exchange(m_Current, std::move(next));
            m_Version.store(nextVersion, std::memory_order_release);
        }
    }

  private:
    mutable std::mutex m_Mutex;
    std::shared_ptr<const T> m_Current = std::make_shared<const T>();
    std::atomic<std::uint64_t> m_Version{0};
};

} // namespace Domain
