#pragma once

// The COM apartment of a worker thread (#2731). In Platform, the lowest layer, so Domain's
// BackgroundSampler can put its thread in the multithreaded apartment without a layering violation.
//
// The probes a sampler builds and reads on its thread create COM objects on Windows: PDH loads the
// registered performance providers on the thread that first collects, and some of them hold COM
// proxies (PerfProviderApartment.h); DXGI and WMI are COM too. Created on a thread with no apartment,
// those objects land in the implicit MTA; created on the UI thread they lived in its STA, and every
// later call to them from a sampler marshalled into the UI thread's message loop, so a busy UI frame
// stalled a sampling pass. A sampler thread enters the MTA explicitly for its whole life, so what its
// probes create is apartment-free and never waits for the UI.

namespace Platform
{

/// Puts the calling thread in the process's multithreaded COM apartment for this object's lifetime.
/// Windows only; a no-op elsewhere. One per thread, made at the top of the thread's function, so it
/// outlives everything the thread creates. Never throws.
class MultiThreadedApartment
{
  public:
    MultiThreadedApartment() noexcept;
    ~MultiThreadedApartment();

    MultiThreadedApartment(const MultiThreadedApartment&) = delete;
    MultiThreadedApartment& operator=(const MultiThreadedApartment&) = delete;
    MultiThreadedApartment(MultiThreadedApartment&&) = delete;
    MultiThreadedApartment& operator=(MultiThreadedApartment&&) = delete;

    /// Whether the thread is in the MTA because of this object: false where there is no COM (Linux),
    /// and where the thread was already in an apartment of the other kind, which is left alone.
    [[nodiscard]] bool entered() const noexcept
    {
        return m_Entered;
    }

  private:
    bool m_Entered = false;
};

} // namespace Platform
