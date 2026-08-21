# Metrics Pipeline

The sampling and snapshot pipeline is documented in the canonical architecture guide:

**[Sampling and Publication](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#sampling-and-publication)**

In summary, Platform probes collect raw counters, Domain models calculate rates and bounded histories, and App panels render cached snapshots. Process enumeration and heavy system metrics (System, Storage, GPU) run asynchronously on a background thread via `BackgroundSampler` to ensure the UI remains responsive under load.

The detailed explanation lives in one place so threading and refresh behavior stay aligned with the implementation.
