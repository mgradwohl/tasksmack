# Metrics Pipeline

The sampling and snapshot pipeline is documented in the canonical architecture guide:

**[Sampling and Publication](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#sampling-and-publication)**

In summary, Platform probes collect raw counters, Domain models calculate rates and bounded histories, and App panels render cached publications. Process enumeration and heavy system metrics use separate `BackgroundSampler` workers. System, Storage, and GPU atomically publish immutable versioned snapshot-and-history generations, while process consumers use `ProcessModel` snapshot versions, keeping render loops free of probe I/O, model locks, and redundant history copies.

The detailed explanation lives in one place so threading and refresh behavior stay aligned with the implementation.
