# Metrics Pipeline

The sampling and snapshot pipeline is documented in the canonical architecture guide:

**[Sampling and Publication](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#sampling-and-publication)**

In summary, Platform probes collect raw counters, Domain models calculate rates and bounded histories, and App panels render cached snapshots. Process enumeration and GPU refresh run on worker threads; system and storage refresh currently run from the System Overview update cadence.

The detailed explanation lives in one place so threading and refresh behavior stay aligned with the implementation.
