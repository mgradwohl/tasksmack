// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// The helper's per-process UDP byte counter (#3366), the Linux side of "Count UDP traffic per process" (Windows
// counts with ETW). It sums the bytes each process sends and receives over UDP into a map keyed by tgid, and the
// helper reads the map. It reads no kernel structure, only each function's return value, so it needs no vmlinux.h
// and builds the same for x86_64 and ARM64 (maintainer, 2026-11-07).
//
// Two attach modes, loaded one at a time by UdpByteCounter.cpp:
//   - fexit (5.17+ with BTF): bpf_get_func_ret gives the return value whatever the function's arity, so the
//     6-argument udp_recvmsg of older kernels and the 5-argument one since 5.19 both work.
//   - kretprobe, where fexit can't attach: the return register.
// The kernel accepts a tracing program only under a GPL-compatible license string, hence the dual license.

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/bpf.h>
#include <linux/ptrace.h>

// Matches Platform::UdpByteCounts in UdpByteCounter.h: the map's value layout is the user-space contract.
struct udp_bytes
{
    __u64 tx;
    __u64 rx;
};

// LRU-bounded, so processes that exited age out without the helper deleting them.
struct
{
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, struct udp_bytes);
} udp_bytes_by_tgid SEC(".maps");

static __always_inline void count(long ret, int is_tx)
{
    if (ret <= 0)
    {
        return; // an error, or nothing moved
    }
    const __u32 tgid = (__u32) (bpf_get_current_pid_tgid() >> 32);
    struct udp_bytes* bytes = bpf_map_lookup_elem(&udp_bytes_by_tgid, &tgid);
    if (!bytes)
    {
        const struct udp_bytes zero = {0, 0};
        // BPF_NOEXIST: another CPU may have added this tgid since the lookup; its entry is the one to add to.
        bpf_map_update_elem(&udp_bytes_by_tgid, &tgid, &zero, BPF_NOEXIST);
        bytes = bpf_map_lookup_elem(&udp_bytes_by_tgid, &tgid);
        if (!bytes)
        {
            return;
        }
    }
    if (is_tx)
    {
        __sync_fetch_and_add(&bytes->tx, (__u64) ret);
    }
    else
    {
        __sync_fetch_and_add(&bytes->rx, (__u64) ret);
    }
}

static __always_inline void countFexit(void* ctx, int is_tx)
{
    __u64 ret = 0;
    if (bpf_get_func_ret(ctx, &ret) == 0)
    {
        count((long) ret, is_tx);
    }
}

SEC("fexit/udp_sendmsg")
int udp_sendmsg_exit(void* ctx)
{
    countFexit(ctx, 1);
    return 0;
}

SEC("fexit/udpv6_sendmsg")
int udpv6_sendmsg_exit(void* ctx)
{
    countFexit(ctx, 1);
    return 0;
}

SEC("fexit/udp_recvmsg")
int udp_recvmsg_exit(void* ctx)
{
    countFexit(ctx, 0);
    return 0;
}

SEC("fexit/udpv6_recvmsg")
int udpv6_recvmsg_exit(void* ctx)
{
    countFexit(ctx, 0);
    return 0;
}

SEC("kretprobe/udp_sendmsg")
int BPF_KRETPROBE(udp_sendmsg_ret, long ret)
{
    count(ret, 1);
    return 0;
}

SEC("kretprobe/udpv6_sendmsg")
int BPF_KRETPROBE(udpv6_sendmsg_ret, long ret)
{
    count(ret, 1);
    return 0;
}

SEC("kretprobe/udp_recvmsg")
int BPF_KRETPROBE(udp_recvmsg_ret, long ret)
{
    count(ret, 0);
    return 0;
}

SEC("kretprobe/udpv6_recvmsg")
int BPF_KRETPROBE(udpv6_recvmsg_ret, long ret)
{
    count(ret, 0);
    return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";
