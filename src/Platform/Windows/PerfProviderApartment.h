#pragma once

// Keeps COM alive for the performance-counter provider DLLs that PDH loads into the process (#1898).
//
// PDH keeps one HKEY_PERFORMANCE_DATA connection to the local machine for the life of the process, and
// through it advapi32 loads the registered v1 perf providers (bitsperf.dll, esentprf.dll, ...) on the
// thread that first collects. Some of them create COM objects there: bitsperf.dll holds a proxy to the
// BITS service whose code is BitsProxy.dll, a proxy/stub DLL that COM loads for the calling thread's
// apartment. When that apartment ends -- SDL_Quit's CoUninitialize on the UI thread, or a ComScope on a
// worker -- COM unloads BitsProxy.dll, but the provider still holds the proxy. The next PDH call that
// reconnects (PdhEnumObjects with bRefresh = TRUE, which WindowsDiskProbe makes) closes the providers,
// and bitsperf's CPerfMon::~CPerfMon calls Release through a vtable in the unmapped DLL: an access
// violation. A second Core::Application in one process (TaskSmackAppTests) hit it every time.
//
// PDH has no way to drop its connection, so the apartment the providers' objects live in must outlive
// it, which is the process. keepComForPerfProviders() is called by each PDH user, on its thread, after
// it has used PDH:
//  - the process-wide MTA is kept with CoIncrementMTAUsage, once, so a provider object made on a thread
//    in the MTA (explicit or implicit) never loses its apartment;
//  - a thread that is already in an STA (the UI thread, which SDL initialises) gets one more
//    CoInitializeEx reference, never released, so SDL_Quit's CoUninitialize leaves the apartment and
//    its proxy DLLs loaded. A thread with no COM, or in the MTA, gets no new apartment: COM is never
//    started where it wasn't, and a thread's apartment model is never changed.

namespace Platform::Windows
{

/// Keeps the calling thread's COM apartment, if it is an STA, and the process's MTA, for the life of the
/// process, so the PDH providers' COM objects stay usable (#1898). Cheap after the first call on a thread.
void keepComForPerfProviders() noexcept;

} // namespace Platform::Windows
