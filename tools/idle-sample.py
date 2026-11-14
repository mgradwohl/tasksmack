#!/usr/bin/env python3
"""tools/idle-sample.py: measure-idle.sh's /proc sampler for one phase of an idle run (#3729).

The Linux counterpart of tools/idle-sample.ps1 (#3715). It reads, for one phase of a running TaskSmack:
  - each thread's CPU (utime+stime in /proc/<pid>/task/<tid>/stat) and wake-ups (voluntary plus
    nonvoluntary context switches in /proc/<pid>/task/<tid>/status), and the process's total CPU;
  - memory at each end and every --memory-poll-seconds: from /proc/<pid>/smaps_rollup the resident
    set (workingSetMB), its private part (privateWorkingSetMB, Private_Clean + Private_Dirty) and the
    proportional set size (pssMB), plus open file descriptors (fds) and threads;
  - the compositor's CPU and wake-ups, summed over the processes named in COMPOSITOR_NAMES that this
    user can see (gnome-shell, kwin_wayland, Xorg and so on), in place of measure-idle.ps1's dwm.exe;
    null when none is visible, as under WSLg, whose compositor runs in the system distro;
  - how busy the whole machine was, from /proc/stat, and the rest of it once TaskSmack and the
    compositor are taken out.
The phase is printed as one JSON object with measure-idle.ps1's field names (schema
tasksmack-idle-windows/2), so one summary script can read both platforms' runs. Fields Linux has no
equivalent for stay null: timerResolutionMs (no system-wide timer resolution) and a thread's module
(every Linux thread has a name, its comm). measure-idle.sh adds the frame figures.

Usage:
  idle-sample.py phases <spec>      Print "<kind> <seconds>" per phase of a --phases spec
                                    (visible:120,minimized:60,restored:60); exit 2 if it is invalid.
  idle-sample.py phase --pid <pid> --kind <kind> --seconds <n> [--memory-poll-seconds <n>]
                       [--memory-rows <csv>] [--run-start <epoch>] [--rep <n>] [--mesa-regex <re>]
                       [--minimized true|false] [--proc-root <dir>]
                                    Sample one phase and print its JSON. --memory-rows appends one CSV
                                    row per memory reading (MEMORY_CSV_COLUMNS), timed from --run-start.
                                    Exits 3 if the process goes away during the phase.
  idle-sample.py set-frames <phase.json> <summaries> <fps> <frameP95> <frameP99> <frameMax> <loopP95> <loopP99>
                                    Fill in a phase's frames from resize-perf-summary.awk's figures
                                    (n/a is written as null).
  idle-sample.py skipped <kind> <seconds> <note>
                                    Print the JSON of a phase that was not sampled, and why.
  idle-sample.py describe <phase.json>
                                    Print one line of a phase's figures, for measure-idle.sh's table.
  idle-sample.py csv-header         Print the memory series' header row.
"""

import argparse
import datetime
import json
import os
import re
import sys
import time
from pathlib import Path

PHASE_KINDS = ("visible", "minimized", "restored")

# Compositors and display servers whose CPU and wake-ups stand in for dwm.exe's. Matched on comm,
# which the kernel truncates to 15 characters.
COMPOSITOR_NAMES = frozenset(
    {
        "gnome-shell",
        "kwin_wayland",
        "kwin_x11",
        "Xorg",
        "Xwayland",
        "weston",
        "sway",
        "Hyprland",
        "labwc",
        "wayfire",
        "mutter",
        "cosmic-comp",
        "niri",
        "river",
        "marco",
        "xfwm4",
    }
)

# The memory series' columns: measure-idle.ps1's where they mean the same thing (seconds, phase,
# privateWorkingSetMB, workingSetMB, threads), then Linux's own (pssMB, fds) and the repetition.
MEMORY_CSV_COLUMNS = ("seconds", "phase", "privateWorkingSetMB", "workingSetMB", "pssMB", "fds", "threads", "rep")


class PhaseSpecError(ValueError):
    pass


class ProcessGone(RuntimeError):
    pass


def parse_phase_spec(spec):
    """'visible:120,minimized:60' -> [(kind, seconds)], in order, as ConvertFrom-IdlePhaseSpec reads it."""
    phases = []
    for part in (p.strip() for p in spec.split(",")):
        if not part:
            continue
        m = re.fullmatch(r"([a-z]+):(\d+)", part)
        if not m or m.group(1) not in PHASE_KINDS or int(m.group(2)) < 1:
            raise PhaseSpecError(
                f"Invalid phase '{part}': expected <kind>:<seconds>, with kind one of "
                f"{', '.join(PHASE_KINDS)} and seconds at least 1."
            )
        phases.append((m.group(1), int(m.group(2))))
    if not phases:
        raise PhaseSpecError("No phases given.")
    return phases


def parse_stat_ticks(text):
    """utime+stime of a /proc stat line. comm may hold spaces or ')', so fields count from the last ')'."""
    rest = text[text.rindex(")") + 2 :].split()
    # rest[0] is field 3 (state); utime and stime are fields 14 and 15.
    return int(rest[11]) + int(rest[12])


def parse_context_switches(text):
    """voluntary_ctxt_switches + nonvoluntary_ctxt_switches of a /proc status file, or None."""
    total = 0
    found = 0
    for line in text.splitlines():
        key, _, value = line.partition(":")
        if key in ("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"):
            total += int(value.strip())
            found += 1
    return total if found == 2 else None


def parse_smaps_rollup(text):
    """{Rss, Pss, Private} in kB from /proc/<pid>/smaps_rollup (Private = Private_Clean + Private_Dirty)."""
    fields = {}
    for line in text.splitlines():
        m = re.match(r"^(\w+):\s+(\d+) kB", line)
        if m:
            fields[m.group(1)] = int(m.group(2))
    return {
        "Rss": fields.get("Rss"),
        "Pss": fields.get("Pss"),
        "Private": (fields["Private_Clean"] + fields["Private_Dirty"])
        if "Private_Clean" in fields and "Private_Dirty" in fields
        else None,
    }


def parse_proc_stat_cpu(text):
    """(total, idle) jiffies of /proc/stat's aggregate cpu line; idle includes iowait. Guest time is
    already in user and nice, so only the first eight fields are summed."""
    for line in text.splitlines():
        if line.startswith("cpu "):
            values = [int(v) for v in line.split()[1:9]]
            values += [0] * (8 - len(values))
            return sum(values), values[3] + values[4]
    raise ValueError("no aggregate cpu line in /proc/stat")


def _read(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def snapshot_threads(proc, pid):
    """tid -> {ticks, name, switches} for every thread still readable; one that exits mid-read is skipped."""
    threads = {}
    try:
        tids = os.listdir(proc / str(pid) / "task")
    except OSError:
        return threads
    for tid in tids:
        task = proc / str(pid) / "task" / tid
        stat = _read(task / "stat")
        comm = _read(task / "comm")
        if stat is None or comm is None:
            continue
        status = _read(task / "status")
        try:
            ticks = parse_stat_ticks(stat)
        except (ValueError, IndexError):
            continue
        threads[int(tid)] = {
            "ticks": ticks,
            "name": comm.rstrip("\n"),
            "switches": parse_context_switches(status) if status is not None else None,
        }
    return threads


def snapshot_process(proc, pid):
    """{ticks, threads} for one process, or None when it is gone."""
    stat = _read(proc / str(pid) / "stat")
    if stat is None:
        return None
    try:
        ticks = parse_stat_ticks(stat)
    except (ValueError, IndexError):
        return None
    return {"ticks": ticks, "threads": snapshot_threads(proc, pid)}


def find_compositors(proc):
    """pid -> comm of the visible processes named in COMPOSITOR_NAMES."""
    found = {}
    try:
        entries = os.listdir(proc)
    except OSError:
        return found
    for entry in entries:
        if not entry.isdigit():
            continue
        comm = _read(proc / entry / "comm")
        if comm is not None and comm.rstrip("\n") in COMPOSITOR_NAMES:
            found[int(entry)] = comm.rstrip("\n")
    return found


def read_memory(proc, pid):
    """{privateWorkingSetMB, workingSetMB, pssMB, fds, threads}; a figure that can't be read is None."""
    rollup = _read(proc / str(pid) / "smaps_rollup")
    mem = parse_smaps_rollup(rollup) if rollup is not None else {"Rss": None, "Pss": None, "Private": None}

    def mb(kb):
        return round(kb / 1024.0, 2) if kb is not None else None

    def count(sub):
        try:
            return len(os.listdir(proc / str(pid) / sub))
        except OSError:
            return None

    return {
        "privateWorkingSetMB": mb(mem["Private"]),
        "workingSetMB": mb(mem["Rss"]),
        "pssMB": mb(mem["Pss"]),
        "fds": count("fd"),
        "threads": count("task"),
    }


def summarize_threads(before, after, seconds, clk_tck, mesa_regex):
    """The phase's figures for one process from two snapshot_process() readings: thread rows (threads
    present at both ends, highest CPU first) and the totals. Total CPU is the process's own counter,
    so threads that came and went during the phase are in it; total wake-ups are the survivors' sum,
    as measure-idle.ps1's are."""

    def percent(ticks):
        return round(ticks * 100.0 / clk_tck / seconds, 3) if seconds > 0 else 0.0

    def rate(count):
        return round(count / seconds, 2) if seconds > 0 else 0.0

    rows = []
    survivor_switches = 0
    have_switches = False
    for tid, a in after["threads"].items():
        b = before["threads"].get(tid)
        if b is None:
            continue
        wakeups = None
        if a["switches"] is not None and b["switches"] is not None:
            delta = a["switches"] - b["switches"]
            survivor_switches += delta
            have_switches = True
            wakeups = rate(delta)
        rows.append(
            {
                "tid": tid,
                "name": a["name"],
                "module": None,
                "cpu": percent(a["ticks"] - b["ticks"]),
                "wakeupsPerSec": wakeups,
                "mesa": bool(mesa_regex and re.search(mesa_regex, a["name"])),
            }
        )
    rows.sort(key=lambda r: r["cpu"], reverse=True)
    total = percent(after["ticks"] - before["ticks"])
    mesa = round(float(sum(r["cpu"] for r in rows if r["mesa"])), 3)
    return {
        "totalCpu": total,
        "mesaCpu": mesa,
        "appCpu": round(max(0.0, total - mesa), 3),
        "totalWakeupsPerSec": round(survivor_switches / seconds, 1) if have_switches and seconds > 0 else None,
        "threads": rows,
    }


def summarize_compositor(before, after, names, seconds, clk_tck):
    """{name, cpu, wakeupsPerSec} summed over the compositor processes seen at both ends, or None."""
    pids = [pid for pid in after if pid in before and before[pid] is not None and after[pid] is not None]
    if not pids or seconds <= 0:
        return None
    ticks = sum(after[p]["ticks"] - before[p]["ticks"] for p in pids)
    switches = 0
    for p in pids:
        for tid, a in after[p]["threads"].items():
            b = before[p]["threads"].get(tid)
            if b and a["switches"] is not None and b["switches"] is not None:
                switches += a["switches"] - b["switches"]
    return {
        "name": ",".join(sorted({names[p] for p in pids})),
        "cpu": round(ticks * 100.0 / clk_tck / seconds, 3),
        "wakeupsPerSec": round(switches / seconds, 1),
    }


def summarize_machine_load(before, after, tasksmack_cpu, compositor_cpu, logical_cpus):
    """{busyPctOfAll, restPctOfAll} from two parse_proc_stat_cpu() readings; TaskSmack's and the
    compositor's CPU are % of one CPU, as Measure-MachineLoad takes them."""
    total = after[0] - before[0]
    idle = after[1] - before[1]
    if total <= 0:
        return None
    busy = round((total - idle) * 100.0 / total, 2)
    others = (tasksmack_cpu + (compositor_cpu or 0.0)) / logical_cpus
    return {"busyPctOfAll": busy, "restPctOfAll": round(max(0.0, busy - others), 2)}


def _utc(epoch):
    return datetime.datetime.fromtimestamp(epoch, datetime.UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def sample_phase(args):
    proc = Path(args.proc_root)
    pid = args.pid
    clk_tck = os.sysconf("SC_CLK_TCK")
    logical = os.cpu_count() or 1
    run_start = args.run_start if args.run_start is not None else time.time()
    rows = []

    def memory_row(now, mem):
        rows.append(
            {
                "seconds": round(now - run_start, 1),
                "phase": args.kind,
                **mem,
                "rep": args.rep,
            }
        )

    compositor_names = find_compositors(proc)
    comp_before = {p: snapshot_process(proc, p) for p in compositor_names}
    sys_before = parse_proc_stat_cpu(_read(proc / "stat") or "")
    before = snapshot_process(proc, pid)
    start_mono = time.monotonic()
    start_epoch = time.time()
    if before is None:
        raise ProcessGone(f"process {pid} is not running")
    mem_start = read_memory(proc, pid)
    memory_row(start_epoch, mem_start)
    peak = mem_start["privateWorkingSetMB"]

    deadline = start_mono + args.seconds
    poll = args.memory_poll_seconds
    next_poll = start_mono + poll
    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        time.sleep(min(next_poll, deadline) - now)
        if not (proc / str(pid)).exists():
            raise ProcessGone(f"process {pid} exited during the {args.kind} phase")
        if time.monotonic() >= next_poll and time.monotonic() < deadline:
            mem = read_memory(proc, pid)
            memory_row(time.time(), mem)
            if mem["privateWorkingSetMB"] is not None and (peak is None or mem["privateWorkingSetMB"] > peak):
                peak = mem["privateWorkingSetMB"]
            next_poll += poll

    after = snapshot_process(proc, pid)
    elapsed = time.monotonic() - start_mono
    end_epoch = time.time()
    sys_after = parse_proc_stat_cpu(_read(proc / "stat") or "")
    comp_after = {p: snapshot_process(proc, p) for p in compositor_names}
    if after is None:
        raise ProcessGone(f"process {pid} exited during the {args.kind} phase")
    mem_end = read_memory(proc, pid)
    memory_row(end_epoch, mem_end)
    if mem_end["privateWorkingSetMB"] is not None and (peak is None or mem_end["privateWorkingSetMB"] > peak):
        peak = mem_end["privateWorkingSetMB"]

    measured = summarize_threads(before, after, elapsed, clk_tck, args.mesa_regex)
    ui = next((r for r in measured["threads"] if r["tid"] == pid), None)
    compositor = summarize_compositor(comp_before, comp_after, compositor_names, elapsed, clk_tck)
    phase = {
        "phase": args.kind,
        "durationSeconds": round(elapsed, 1),
        "cpuStartUtc": _utc(start_epoch),
        "cpuEndUtc": _utc(end_epoch),
        "cpuStart": round(start_epoch, 3),
        "cpuEnd": round(end_epoch, 3),
        "minimized": {"true": True, "false": False}.get(args.minimized),
        "totalCpu": measured["totalCpu"],
        "appCpu": measured["appCpu"],
        "mesaCpu": measured["mesaCpu"],
        "uiThreadCpu": ui["cpu"] if ui else None,
        "totalWakeupsPerSec": measured["totalWakeupsPerSec"],
        "uiThreadWakeupsPerSec": ui["wakeupsPerSec"] if ui else None,
        "timerResolutionMs": None,
        "compositor": compositor,
        "dwm": None,
        "machineLoad": summarize_machine_load(
            sys_before, sys_after, measured["totalCpu"], compositor["cpu"] if compositor else None, logical
        ),
        "frames": None,
        "memoryStart": mem_start,
        "memoryEnd": mem_end,
        "peakPrivateWorkingSetMB": peak,
        "threads": measured["threads"],
    }
    if args.memory_rows:
        with open(args.memory_rows, "a", encoding="utf-8", newline="") as f:
            for row in rows:
                f.write(",".join("" if row[c] is None else str(row[c]) for c in MEMORY_CSV_COLUMNS) + "\n")
    return phase


def frames_from_summary(summaries, fps, frame_p95, frame_p99, frame_max, loop_p95, loop_p99):
    """A phase's frames object from resize-perf-summary.awk's printed figures; n/a reads as null."""

    def num(v):
        return None if v in ("", "n/a") else float(v)

    return {
        "resizePerfSummaries": int(summaries),
        "fps": num(fps),
        "frameP95Ms": num(frame_p95),
        "frameP99Ms": num(frame_p99),
        "frameMaxMs": num(frame_max),
        "loopP95Ms": num(loop_p95),
        "loopP99Ms": num(loop_p99),
    }


def skipped_phase(kind, seconds, note):
    """A phase that was not sampled (the window manager would not minimize the window, say)."""
    return {"phase": kind, "requestedSeconds": seconds, "skipped": True, "note": note}


def describe_phase(phase):
    """One line of a phase's figures for measure-idle.sh's table."""
    if phase.get("skipped"):
        return f"  phase {phase['phase']:<9} skipped: {phase['note']}"

    def fmt(v, suffix=""):
        if v is None:
            return "n/a"
        return f"{str(v).lower() if isinstance(v, bool) else v}{suffix}"

    comp = phase.get("compositor")
    load = phase.get("machineLoad")
    mem = phase.get("memoryEnd") or {}
    frames = phase.get("frames") or {}
    return (
        f"  phase {phase['phase']:<9} {phase['durationSeconds']}s minimized={fmt(phase.get('minimized'))} "
        f"appCpu={phase['appCpu']}% totalCpu={phase['totalCpu']}% fps={fmt(frames.get('fps'))} "
        f"wakeups/s={fmt(phase['totalWakeupsPerSec'])} (ui {fmt(phase['uiThreadWakeupsPerSec'])}) "
        f"privateWS={fmt(mem.get('privateWorkingSetMB'), ' MB')} "
        f"compositor={(comp['name'] + ' ' + str(comp['cpu']) + '%/' + str(comp['wakeupsPerSec'])) if comp else 'n/a'} "
        f"rest={fmt(load['restPctOfAll'] if load else None, '% of all CPUs')}"
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_phases = sub.add_parser("phases")
    p_phases.add_argument("spec")
    p_phase = sub.add_parser("phase")
    p_phase.add_argument("--pid", type=int, required=True)
    p_phase.add_argument("--kind", choices=PHASE_KINDS, required=True)
    p_phase.add_argument("--seconds", type=int, required=True)
    p_phase.add_argument("--memory-poll-seconds", type=int, default=5)
    p_phase.add_argument("--memory-rows")
    p_phase.add_argument("--run-start", type=float)
    p_phase.add_argument("--rep", type=int, default=1)
    p_phase.add_argument("--mesa-regex", default="")
    p_phase.add_argument("--minimized", choices=("true", "false", ""), default="")
    p_phase.add_argument("--proc-root", default="/proc")
    p_frames = sub.add_parser("set-frames")
    p_frames.add_argument("path")
    p_frames.add_argument("figures", nargs=7)
    p_skipped = sub.add_parser("skipped")
    p_skipped.add_argument("kind", choices=PHASE_KINDS)
    p_skipped.add_argument("seconds", type=int)
    p_skipped.add_argument("note")
    p_describe = sub.add_parser("describe")
    p_describe.add_argument("path")
    sub.add_parser("csv-header")
    args = parser.parse_args(argv)

    if args.command == "describe":
        print(describe_phase(json.loads(Path(args.path).read_text(encoding="utf-8"))))
        return 0
    if args.command == "csv-header":
        print(",".join(MEMORY_CSV_COLUMNS))
        return 0

    if args.command == "set-frames":
        path = Path(args.path)
        phase = json.loads(path.read_text(encoding="utf-8"))
        phase["frames"] = frames_from_summary(*args.figures)
        path.write_text(json.dumps(phase) + "\n", encoding="utf-8")
        return 0
    if args.command == "skipped":
        print(json.dumps(skipped_phase(args.kind, args.seconds, args.note)))
        return 0

    if args.command == "phases":
        try:
            for kind, seconds in parse_phase_spec(args.spec):
                print(kind, seconds)
        except PhaseSpecError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2
        return 0

    if args.seconds < 1 or args.memory_poll_seconds < 1:
        print("ERROR: --seconds and --memory-poll-seconds must be at least 1", file=sys.stderr)
        return 2
    try:
        phase = sample_phase(args)
    except ProcessGone as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 3
    json.dump(phase, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
