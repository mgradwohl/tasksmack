// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// The helper's per-process UDP byte counter (#3366), the Linux side of "Count UDP traffic per process" (Windows
// counts with ETW). It sums the bytes each process sends and receives over UDP into a map keyed by tgid, and the
// helper reads the map. It reads no kernel structure, only the functions' arguments and return values, so it needs
// no vmlinux.h and builds the same for x86_64 and ARM64 (maintainer, 2026-11-07).
//
// Two attach modes, loaded one at a time by UdpByteCounter.cpp:
//   - fentry/fexit (5.17+ with BTF): bpf_get_func_ret and bpf_get_func_arg_cnt work whatever the function's
//     arity, so the 6-argument udp_recvmsg of older kernels and the 5-argument one since 5.19 both work.
//   - kprobe/kretprobe, where fexit can't attach: the return register, and the flags argument stashed at entry
//     from the position RECV_FLAGS_ARG names (the loader sets it from the kernel's version).
//
// What it counts once and what it doesn't:
//   - A return is an int: it's read as one, so a negative errno is never taken for bytes.
//   - udpv6_sendmsg hands a send to an IPv4-mapped address to udp_sendmsg: that inner send isn't counted again.
//   - A MSG_PEEK receive leaves the datagram queued: it isn't counted; the receive that takes it is.
// The kernel accepts a tracing program only under a GPL-compatible license string, hence the dual license.

// The kernel's uapi types first: libbpf's headers use __u32/__u64 and struct pt_regs without including them.
// clang-format off
#include <linux/bpf.h>
#include <linux/ptrace.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
// clang-format on

#define TASKSMACK_MSG_PEEK 2 // <sys/socket.h>'s MSG_PEEK, which the BPF target doesn't include

// The 0-based position of udp_recvmsg's and udpv6_recvmsg's flags argument, for the kprobe mode: 3 since 5.19
// (sk, msg, len, flags, addr_len), 4 before (sk, msg, len, noblock, flags, addr_len). Set by the loader.
const volatile int RECV_FLAGS_ARG = 3;

// Matches Platform::UdpByteCounts: the map's value layout is the user-space contract.
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

// The threads inside udpv6_sendmsg, keyed by pid_tgid: an udp_sendmsg under one is its IPv4-mapped send.
struct
{
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, __u8);
} in_udpv6_send SEC(".maps");

// The kprobe mode's receive flags, from entry to return, keyed by pid_tgid.
struct
{
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, __u32);
} recv_flags SEC(".maps");

static __always_inline void count(int ret, int is_tx)
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

static __always_inline void enterUdpv6Send(void)
{
    const __u64 thread = bpf_get_current_pid_tgid();
    const __u8 one = 1;
    bpf_map_update_elem(&in_udpv6_send, &thread, &one, BPF_ANY);
}

static __always_inline void exitUdpv6Send(int ret)
{
    const __u64 thread = bpf_get_current_pid_tgid();
    bpf_map_delete_elem(&in_udpv6_send, &thread);
    count(ret, 1);
}

static __always_inline void exitUdpSend(int ret)
{
    const __u64 thread = bpf_get_current_pid_tgid();
    if (bpf_map_lookup_elem(&in_udpv6_send, &thread))
    {
        return; // udpv6_sendmsg's IPv4-mapped send: counted when udpv6_sendmsg returns
    }
    count(ret, 1);
}

// fentry/fexit: the return value, and for a receive its flags, whichever arity the kernel's function has.
static __always_inline int fexitRet(void* ctx)
{
    __u64 ret = 0;
    return bpf_get_func_ret(ctx, &ret) == 0 ? (int) ret : -1;
}

static __always_inline void fexitReceive(void* ctx)
{
    __u64 flags = 0;
    const __u64 flagsArg = bpf_get_func_arg_cnt(ctx) >= 6 ? 4 : 3;
    if (bpf_get_func_arg(ctx, (__u32) flagsArg, &flags) != 0 || (flags & TASKSMACK_MSG_PEEK) != 0)
    {
        return; // a peek leaves the datagram queued
    }
    count(fexitRet(ctx), 0);
}

SEC("fentry/udpv6_sendmsg")
int udpv6_sendmsg_entry(void* ctx)
{
    enterUdpv6Send();
    return 0;
}

SEC("fexit/udpv6_sendmsg")
int udpv6_sendmsg_exit(void* ctx)
{
    exitUdpv6Send(fexitRet(ctx));
    return 0;
}

SEC("fexit/udp_sendmsg")
int udp_sendmsg_exit(void* ctx)
{
    exitUdpSend(fexitRet(ctx));
    return 0;
}

SEC("fexit/udp_recvmsg")
int udp_recvmsg_exit(void* ctx)
{
    fexitReceive(ctx);
    return 0;
}

SEC("fexit/udpv6_recvmsg")
int udpv6_recvmsg_exit(void* ctx)
{
    fexitReceive(ctx);
    return 0;
}

// kprobe/kretprobe: the flags are only in the registers at entry, so they're kept for the return.
static __always_inline void kprobeReceive(struct pt_regs* ctx)
{
    const __u64 thread = bpf_get_current_pid_tgid();
    // Both registers read at their fixed offsets, then one chosen: left to itself the compiler turns the choice
    // into one load at a computed offset into the context, which the verifier refuses (EACCES).
    __u64 fourth = PT_REGS_PARM4(ctx);
    __u64 fifth = PT_REGS_PARM5(ctx);
    barrier_var(fourth);
    barrier_var(fifth);
    const __u32 flags = (__u32) (RECV_FLAGS_ARG == 4 ? fifth : fourth);
    bpf_map_update_elem(&recv_flags, &thread, &flags, BPF_ANY);
}

static __always_inline void kretprobeReceive(int ret)
{
    const __u64 thread = bpf_get_current_pid_tgid();
    const __u32* flags = bpf_map_lookup_elem(&recv_flags, &thread);
    const int peek = flags && (*flags & TASKSMACK_MSG_PEEK) != 0;
    bpf_map_delete_elem(&recv_flags, &thread);
    if (!peek)
    {
        count(ret, 0);
    }
}

SEC("kprobe/udpv6_sendmsg")
int BPF_KPROBE(udpv6_sendmsg_enter)
{
    enterUdpv6Send();
    return 0;
}

SEC("kretprobe/udpv6_sendmsg")
int BPF_KRETPROBE(udpv6_sendmsg_ret, int ret)
{
    exitUdpv6Send(ret);
    return 0;
}

SEC("kretprobe/udp_sendmsg")
int BPF_KRETPROBE(udp_sendmsg_ret, int ret)
{
    exitUdpSend(ret);
    return 0;
}

SEC("kprobe/udp_recvmsg")
int BPF_KPROBE(udp_recvmsg_enter)
{
    kprobeReceive(ctx);
    return 0;
}

SEC("kretprobe/udp_recvmsg")
int BPF_KRETPROBE(udp_recvmsg_ret, int ret)
{
    kretprobeReceive(ret);
    return 0;
}

SEC("kprobe/udpv6_recvmsg")
int BPF_KPROBE(udpv6_recvmsg_enter)
{
    kprobeReceive(ctx);
    return 0;
}

SEC("kretprobe/udpv6_recvmsg")
int BPF_KRETPROBE(udpv6_recvmsg_ret, int ret)
{
    kretprobeReceive(ret);
    return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";
