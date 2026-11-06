"""KCDS 1,000-player capacity benchmark runner.

Starts a private kcd2s-server (default UDP port 17777, never the live port 7777) with the resource
host and the kcds-capacity workload resource, drives it with several kcd2s-load-probe processes,
and writes report.json + report.md with:

- hardware, OS, exact commit/build and every server/probe setting,
- numeric budgets for every run, declared and written to disk before the first run starts,
- per-phase p50/p95/p99/max tick work, missed deadlines, server CPU, per-core machine CPU,
  RSS and its trend, queue depths, UDP bytes/packets in both directions per peer and aggregate,
  reliable retransmissions, replication convergence, resource event latency, SDK entity frames
  and resource-host bridge (IPC) traffic,
- a verdict per budget (within / over / not measured). A run inside its budgets is still not
  1,000-player readiness: see the limitations section of the report.

Matrices:
  smoke - 100 clients, short runs that prove the runner and every workload path work.
  full  - 100/500/1,000 clients: --duration (default 1,800 s) mixed runs, distributed and dense,
          plus shorter focus runs (idle, movement, SharedState, events), churn/reconnect, emulated
          1%/5% loss and 50/150 ms RTT, and a 1/2/4/8 snapshot worker sweep.

Use --dry-run to print the plan, the expected duration and the budgets without starting anything.
The runner only stops processes it started itself.
"""

import argparse
import ctypes
import datetime
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
LIVE_PORT = 7777
DEFAULT_PORT = 17777
TICK_HZ = 60.0
CLIENTS_PER_GENERATOR = 100
RESOURCE_DIR = ROOT / "Dev" / "testkit" / "capacity" / "resources"
RESOURCE_HOST_ENTRY = ROOT / "Backend" / "scripting" / "runtimes" / "js" / "main.cjs"
MAP = "trosecko"


def load_baseline_tools():
    """Reuse the process, merge and environment helpers of the existing baseline runner."""
    path = ROOT / "Dev" / "tools" / "load" / "run_load_baseline.py"
    spec = importlib.util.spec_from_file_location("run_load_baseline", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASELINE = load_baseline_tools()


# ---------------------------------------------------------------------------------------------
# Matrix


class Scenario:
    """One benchmark run: a client count, a layout and the workloads active during it."""

    def __init__(
        self,
        name,
        group,
        clients,
        layout,
        duration_s,
        movement=True,
        shared_state_hz=0.0,
        events_hz=0.0,
        churn=False,
        loss=0.0,
        rtt_ms=0.0,
        snapshot_workers=None,
    ):
        self.name = name
        self.group = group
        self.clients = clients
        self.layout = layout  # spawn (no movement), distributed or dense
        self.duration_s = duration_s
        self.movement = movement
        self.shared_state_hz = shared_state_hz
        self.events_hz = events_hz
        self.churn = churn
        self.loss = loss
        self.rtt_ms = rtt_ms
        # None: the runner's --snapshot-workers; a number: this run's worker sweep value.
        self.snapshot_workers = snapshot_workers

    def churn_clients(self):
        if not self.churn:
            return 0
        return max(5, int(self.clients * 0.05))

    def max_peers(self):
        return min(1024, self.clients + self.churn_clients() + 8)

    def describe(self):
        return {
            "name": self.name,
            "group": self.group,
            "clients": self.clients,
            "layout": self.layout,
            "duration_s": self.duration_s,
            "movement": self.movement,
            "shared_state_hz": self.shared_state_hz,
            "events_hz": self.events_hz,
            "churn": self.churn,
            "churn_clients": self.churn_clients(),
            "loss_fraction_per_direction": self.loss,
            "added_rtt_ms": self.rtt_ms,
            "snapshot_workers": self.snapshot_workers,
            "max_peers": self.max_peers(),
        }


def smoke_matrix(options):
    duration = options.duration or 60.0
    short = max(20.0, duration / 3.0)
    return [
        Scenario("0100-distributed-mixed", "core", 100, "distributed", duration, True, 2.0, 2.0),
        Scenario("0100-dense-mixed", "core", 100, "dense", short, True, 2.0, 2.0),
        Scenario("0100-churn", "churn", 100, "distributed", short, True, 0.0, 1.0, churn=True),
        Scenario(
            "0100-loss5-rtt150",
            "network",
            100,
            "distributed",
            short,
            True,
            0.0,
            2.0,
            loss=0.05,
            rtt_ms=150.0,
        ),
        Scenario("0100-dense-workers1", "workers", 100, "dense", short, snapshot_workers=1),
        Scenario("0100-dense-workers2", "workers", 100, "dense", short, snapshot_workers=2),
        Scenario("0100-dense-workers4", "workers", 100, "dense", short, snapshot_workers=4),
    ]


def full_matrix(options):
    duration = options.duration or 1800.0
    focus = options.focus_duration
    scenarios = []
    for clients in (100, 500, 1000):
        prefix = f"{clients:04d}"
        for layout in ("distributed", "dense"):
            scenarios.append(
                Scenario(
                    f"{prefix}-{layout}-mixed", "core", clients, layout, duration, True, 2.0, 2.0
                )
            )
        scenarios.append(Scenario(f"{prefix}-idle", "focus", clients, "spawn", focus, False))
        for layout in ("distributed", "dense"):
            scenarios.append(
                Scenario(f"{prefix}-{layout}-movement", "focus", clients, layout, focus)
            )
        scenarios.append(
            Scenario(
                f"{prefix}-distributed-shared_state",
                "focus",
                clients,
                "distributed",
                focus,
                True,
                10.0,
            )
        )
        scenarios.append(
            Scenario(
                f"{prefix}-distributed-events",
                "focus",
                clients,
                "distributed",
                focus,
                True,
                0.0,
                10.0,
            )
        )
        scenarios.append(
            Scenario(
                f"{prefix}-churn", "churn", clients, "distributed", focus, True, 0.0, 1.0, True
            )
        )
    for clients in (100, 1000):
        prefix = f"{clients:04d}"
        for loss, rtt, label in ((0.01, 0.0, "loss1"), (0.05, 0.0, "loss5")):
            scenarios.append(
                Scenario(
                    f"{prefix}-{label}",
                    "network",
                    clients,
                    "distributed",
                    focus,
                    True,
                    0.0,
                    2.0,
                    loss=loss,
                    rtt_ms=rtt,
                )
            )
        for rtt in (50.0, 150.0):
            scenarios.append(
                Scenario(
                    f"{prefix}-rtt{int(rtt)}",
                    "network",
                    clients,
                    "distributed",
                    focus,
                    True,
                    0.0,
                    2.0,
                    rtt_ms=rtt,
                )
            )
    for workers in (1, 2, 4, 8):
        scenarios.append(
            Scenario(
                f"1000-dense-workers{workers}",
                "workers",
                1000,
                "dense",
                focus,
                snapshot_workers=workers,
            )
        )
    return scenarios


def workers_of(scenario, options):
    if scenario.snapshot_workers is None:
        return options.snapshot_workers
    return scenario.snapshot_workers


def join_timeout(scenario):
    return max(20.0, scenario.clients * 0.06)


def expected_seconds(scenario, options):
    startup = 10.0
    stop = 15.0
    return startup + join_timeout(scenario) + options.warmup + scenario.duration_s + stop


# ---------------------------------------------------------------------------------------------
# Budgets: declared from the scenario and the configuration only, before anything is measured.


def declare_budgets(scenario, options):
    tick_ms = 1000.0 / TICK_HZ
    dense = scenario.layout in ("dense", "spawn")
    rtt = scenario.rtt_ms
    loss = scenario.loss
    peer_snapshot = options.peer_snapshot_bytes_per_s
    budgets = {
        "tick_work_p99_ms": {
            "limit": 0.8 * tick_ms,
            "comparison": "max",
            "basis": "p99 server tick work below 80% of the tick interval (spec)",
        },
        "missed_deadline_fraction": {
            "limit": 0.001,
            "comparison": "max",
            "basis": "at most 0.1% of ticks longer than the tick interval",
        },
        "admitted_fraction": {
            "limit": 1.0,
            "comparison": "min",
            "basis": "every requested client admitted (cap set above the client count)",
        },
        "server_upload_per_peer_bytes_per_s": {
            "limit": 1.25 * peer_snapshot + 32 * 1024,
            "comparison": "max",
            "basis": "peer snapshot budget + 25% KSTP/ack overhead + 32 KiB/s SDK lane",
        },
        "client_upload_per_peer_bytes_per_s": {
            "limit": 96 * 1024 if scenario.movement else 8 * 1024,
            "comparison": "max",
            "basis": "full owner input fields at 60 Hz (or acks/keep-alive only when idle)",
        },
        "server_download_delivery_ratio": {
            "limit": (1.0 - loss) - 0.01,
            "comparison": "min",
            "basis": "server UDP in / client UDP out, allowing the emulated loss + 1%",
        },
        "convergence_p95_ms": {
            "limit": (250.0 if dense else 150.0) + rtt,
            "comparison": "max",
            "basis": "owner input to remote snapshot apply; dense allows deferral; + added RTT",
        },
        "convergence_p99_ms": {
            "limit": (500.0 if dense else 250.0) + rtt + (200.0 if loss else 0.0),
            "comparison": "max",
            "basis": "p99 bound, + added RTT and one retransmission timeout under loss",
        },
        "rss_growth_fraction": {
            "limit": 0.10 if scenario.churn else 0.05,
            "comparison": "max",
            "basis": "server working set growth over the measurement window (retention trend)",
        },
        "receive_inbox_overflow_drops": {
            "limit": 0,
            "comparison": "max",
            "basis": "no member inbox overflow",
        },
        "resource_inbox_drops": {
            "limit": 0,
            "comparison": "max",
            "basis": "no resource host message dropped by the native inbox",
        },
        "resource_bridge_write_wait_ms_per_s": {
            "limit": 0.05 * 1000.0,
            "comparison": "max",
            "basis": "simulation thread blocked on the resource pipe < 5% of wall time",
        },
    }
    if scenario.events_hz > 0:
        budgets["event_latency_p95_ms"] = {
            "limit": 250.0 + rtt,
            "comparison": "max",
            "basis": "resource broadcast to probe receipt, + added RTT",
        }
        budgets["event_delivery_fraction"] = {
            "limit": 0.99,
            "comparison": "min",
            "basis": "reliable events received / expected (events_hz x window x clients)",
        }
    return budgets


# ---------------------------------------------------------------------------------------------
# Machine sampling


class ProcessorTimes(ctypes.Structure):
    _fields_ = [
        ("idle", ctypes.c_longlong),
        ("kernel", ctypes.c_longlong),
        ("user", ctypes.c_longlong),
        ("dpc", ctypes.c_longlong),
        ("interrupt", ctypes.c_longlong),
        ("interrupt_count", ctypes.c_ulong),
    ]


def processor_times():
    """Per logical processor (idle, busy) 100 ns ticks, or None when unavailable."""
    if sys.platform != "win32":
        return None
    count = os.cpu_count() or 1
    buffer = (ProcessorTimes * count)()
    returned = ctypes.c_ulong()
    status = ctypes.windll.ntdll.NtQuerySystemInformation(
        8, ctypes.byref(buffer), ctypes.sizeof(buffer), ctypes.byref(returned)
    )
    if status != 0:
        return None
    result = []
    for entry in buffer:
        busy = entry.kernel - entry.idle + entry.user
        result.append((entry.idle, busy))
    return result


class CoreSampler:
    """Samples per-core machine CPU every few seconds (whole machine, all processes)."""

    def __init__(self, period_s=5.0):
        self.period_s = period_s
        self.samples = []  # (unix_ms, [percent per core])
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=10)

    def _run(self):
        previous = processor_times()
        while previous is not None and not self.stop_event.wait(self.period_s):
            current = processor_times()
            if current is None:
                return
            percents = []
            for (idle_a, busy_a), (idle_b, busy_b) in zip(previous, current):
                total = (idle_b - idle_a) + (busy_b - busy_a)
                percents.append(100.0 * (busy_b - busy_a) / total if total > 0 else 0.0)
            self.samples.append((int(time.time() * 1000), percents))
            previous = current

    def summary(self, window_start_ms, window_end_ms):
        selected = [p for t, p in self.samples if window_start_ms <= t <= window_end_ms + 5000]
        if not selected:
            return None
        cores = len(selected[0])
        mean = [statistics.mean(sample[core] for sample in selected) for core in range(cores)]
        peak = [max(sample[core] for sample in selected) for core in range(cores)]
        return {
            "samples": len(selected),
            "period_s": self.period_s,
            "mean_percent_per_core": [round(value, 1) for value in mean],
            "max_percent_per_core": [round(value, 1) for value in peak],
            "machine_mean_percent": round(statistics.mean(mean), 1),
            "busiest_core_mean_percent": round(max(mean), 1),
            "scope": "whole machine: server, generators and every other process",
        }


# ---------------------------------------------------------------------------------------------
# Running one scenario


def probe_command(options, scenario, clients, offset, layout_count, duration, leave):
    workload = "idle" if scenario.layout == "spawn" else scenario.layout
    command = [
        str(BASELINE.binary(options, "kcd2s-load-probe")),
        "--port",
        str(options.port),
        "--clients",
        str(clients),
        "--index-offset",
        str(offset),
        "--layout-count",
        str(layout_count),
        "--workload",
        workload,
        "--duration",
        str(duration),
        "--join-timeout",
        str(join_timeout(scenario)),
        "--warmup",
        str(options.warmup),
        "--input-fields",
        options.input_fields,
        "--spacing",
        str(options.spacing),
        "--leave",
        leave,
    ]
    if scenario.loss > 0:
        command += ["--loss", str(scenario.loss)]
    if scenario.rtt_ms > 0:
        command += ["--delay-ms", str(scenario.rtt_ms / 2.0)]
    return command


def send_console(server, text):
    try:
        server.stdin.write(text + "\n")
        server.stdin.flush()
    except OSError:
        pass


class ChurnDriver:
    """Starts short-lived probe groups during the run: joins, leaves and reconnects."""

    def __init__(self, options, scenario, raw_dir):
        self.options = options
        self.scenario = scenario
        self.raw_dir = raw_dir
        self.period_s = 10.0
        self.life_s = 8.0
        self.stop_event = threading.Event()
        self.groups = []  # (process, reader, leave)
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        index = 0
        while not self.stop_event.wait(self.period_s):
            leave = "graceful" if index % 2 == 0 else "timeout"
            clients = self.scenario.churn_clients()
            command = probe_command(
                self.options,
                self.scenario,
                clients,
                0,
                clients,
                self.life_s,
                leave,
            )
            command[command.index("--warmup") + 1] = "0"
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                cwd=self.raw_dir,
            )
            self.groups.append((process, BASELINE.OutputReader(process.stdout), leave))
            index += 1

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=30)
        for process, reader, _ in self.groups:
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            reader.thread.join(timeout=5)

    def summary(self):
        groups = []
        for index, (process, reader, leave) in enumerate(self.groups):
            (self.raw_dir / f"churn-{index}.log").write_text(
                "\n".join(reader.lines) + "\n", encoding="utf8"
            )
            found = BASELINE.tagged_json(reader.lines, "KCDS_LOAD_SUMMARY")
            joins = found[-1]["joins"] if found else None
            groups.append(
                {
                    "leave": leave,
                    "exit_code": process.returncode,
                    "requested": joins["requested"] if joins else None,
                    "admitted": joins["admitted"] if joins else None,
                    "join_p50_ms": joins["time_to_join"]["p50_ms"] if joins else None,
                    "join_max_ms": joins["time_to_join"]["max_ms"] if joins else None,
                }
            )
        admitted = sum(group["admitted"] or 0 for group in groups)
        requested = sum(group["requested"] or 0 for group in groups)
        return {
            "groups": len(groups),
            "clients_per_group": self.scenario.churn_clients(),
            "period_s": self.period_s,
            "life_s": self.life_s,
            "requested": requested,
            "admitted": admitted,
            "group_details": groups,
        }


def merge_sdk(summaries):
    histograms = []
    frames = 0
    events = 0
    refused = 0
    impairment = {"client_upload": {}, "client_download": {}}
    for summary in summaries:
        sdk = summary.get("sdk")
        if not sdk:
            continue
        frames += sdk["entity_frames"]
        events += sdk["events"]
        refused += sdk["entity_acks_refused"]
        histograms.append(sdk["event_latency_histogram"])
        for direction in ("client_upload", "client_download"):
            counters = summary["impairment"][direction]
            for key, value in counters.items():
                impairment[direction][key] = impairment[direction].get(key, 0) + value
    return {
        "entity_frames": frames,
        "entity_acks_refused": refused,
        "events": events,
        "event_latency": BASELINE.merge_histograms(histograms) if histograms else None,
        "impairment": impairment,
    }


def window_metric_lines(metric_lines, start_ms, end_ms):
    return BASELINE.window_lines(metric_lines, start_ms, end_ms)


def summarize_extra(metric_lines, start_ms, end_ms):
    """Server fields the baseline summary does not read: bridge, reliability and RSS trend."""
    lines = window_metric_lines(metric_lines, start_ms, end_ms)
    if not lines:
        return {}
    seconds = sum(line["interval_s"] for line in lines)
    resources = [line.get("resources") for line in lines if line.get("resources")]
    extra = {}
    if resources:
        extra["resource_bridge"] = {
            "messages_to_host_per_s": sum(r["messages_to_host"] for r in resources) / seconds,
            "bytes_to_host_per_s": sum(r["bytes_to_host"] for r in resources) / seconds,
            "write_wait_ms_per_s": sum(r["write_wait_ms"] for r in resources) / seconds,
            "messages_from_host_per_s": sum(r["messages_from_host"] for r in resources) / seconds,
            "inbox_drops_total": resources[-1]["inbox_drops_total"],
            "tracked_entities_max": max(r["tracked_entities"] for r in resources),
            "tracked_views_max": max(r["tracked_views"] for r in resources),
        }
    first = lines[0]["queues"]
    last = lines[-1]["queues"]
    if "reliable_sent_total" in last:
        sent = last["reliable_sent_total"] - first["reliable_sent_total"]
        retransmits = last["reliable_retransmits_total"] - first["reliable_retransmits_total"]
        extra["reliable"] = {
            "sent_per_s": sent / seconds,
            "retransmits_per_s": retransmits / seconds,
            "retransmit_fraction": retransmits / sent if sent else 0.0,
        }
    sets = [line["process"]["working_set_bytes"] for line in lines]
    third = max(1, len(sets) // 3)
    early = statistics.mean(sets[:third])
    late = statistics.mean(sets[-third:])
    extra["rss"] = {
        "first_bytes": sets[0],
        "last_bytes": sets[-1],
        "max_bytes": max(sets),
        "early_third_mean_bytes": early,
        "late_third_mean_bytes": late,
        "growth_fraction": (late - early) / early if early else 0.0,
    }
    ticks = sum(line["tick"]["ticks"] for line in lines)
    missed = sum(line["tick"]["missed_deadlines"] for line in lines)
    extra["missed_deadline_fraction"] = missed / ticks if ticks else 0.0
    extra["packets_per_peer"] = {
        "out_per_s": (
            statistics.mean(
                line["udp"]["interval"]["packets_out"]
                / line["interval_s"]
                / line["peers"]["connected"]
                for line in lines
                if line["peers"]["connected"]
            )
            if any(line["peers"]["connected"] for line in lines)
            else 0.0
        ),
        "in_per_s": (
            statistics.mean(
                line["udp"]["interval"]["packets_in"]
                / line["interval_s"]
                / line["peers"]["connected"]
                for line in lines
                if line["peers"]["connected"]
            )
            if any(line["peers"]["connected"] for line in lines)
            else 0.0
        ),
    }
    return extra


def measured_values(scenario, run):
    """Flat values compared against the declared budgets (None when not measured)."""
    server = run.get("server") or {}
    extra = run.get("server_extra") or {}
    probe = run.get("probe") or {}
    sdk = run.get("sdk") or {}
    values = {}
    work = server.get("work_ms")
    values["tick_work_p99_ms"] = work["p99_worst_interval"] if work else None
    values["missed_deadline_fraction"] = extra.get("missed_deadline_fraction")
    joins = probe.get("joins")
    values["admitted_fraction"] = joins["admitted"] / joins["requested"] if joins else None
    per_peer = server.get("per_peer")
    values["server_upload_per_peer_bytes_per_s"] = (
        per_peer["bytes_out_per_s_mean"] if per_peer else None
    )
    traffic = probe.get("traffic", {})
    values["client_upload_per_peer_bytes_per_s"] = traffic.get(
        "bytes_sent_per_admitted_client_per_s"
    )
    delivery = server.get("delivery")
    values["server_download_delivery_ratio"] = (
        delivery["server_in_over_clients_out"] if delivery else None
    )
    convergence = probe.get("replication", {}).get("convergence")
    has_samples = convergence and convergence["count"] > 0
    values["convergence_p95_ms"] = convergence["p95_ms"] if has_samples else None
    values["convergence_p99_ms"] = convergence["p99_ms"] if has_samples else None
    rss = extra.get("rss")
    values["rss_growth_fraction"] = rss["growth_fraction"] if rss else None
    queues = server.get("queues")
    values["receive_inbox_overflow_drops"] = (
        queues["inbox_overflow_drops_total"] if queues else None
    )
    bridge = extra.get("resource_bridge")
    values["resource_inbox_drops"] = bridge["inbox_drops_total"] if bridge else None
    values["resource_bridge_write_wait_ms_per_s"] = (
        bridge["write_wait_ms_per_s"] if bridge else None
    )
    latency = sdk.get("event_latency")
    values["event_latency_p95_ms"] = latency["p95_ms"] if latency and latency["count"] else None
    window = probe.get("window_s")
    ready = joins["avatars_ready"] if joins else 0
    if scenario.events_hz > 0 and window and ready:
        # One event per client may straddle each end of the window.
        expected = max(1.0, scenario.events_hz * window - 1.0) * ready
        values["event_delivery_fraction"] = sdk.get("events", 0) / expected
    else:
        values["event_delivery_fraction"] = None
    return values


def evaluate(budgets, values):
    verdicts = {}
    for name, budget in budgets.items():
        value = values.get(name)
        if value is None:
            verdicts[name] = {"value": None, "limit": budget["limit"], "verdict": "not measured"}
            continue
        if budget["comparison"] == "max":
            inside = value <= budget["limit"]
        else:
            inside = value >= budget["limit"]
        verdicts[name] = {
            "value": value,
            "limit": budget["limit"],
            "verdict": "within" if inside else "over",
        }
    return verdicts


def run_scenario(options, scenario, out_dir, budgets):
    raw_dir = out_dir / "raw" / scenario.name
    raw_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["KCDS_SERVER_METRICS"] = str(options.metrics_interval)
    env["KCDS_SERVER_MAX_PEERS"] = str(scenario.max_peers())
    env["KCDS_SERVER_SNAPSHOT_WORKERS"] = str(workers_of(scenario, options))
    env["KCDS_SERVER_PEER_SNAPSHOT_BYTES_PER_S"] = str(options.peer_snapshot_bytes_per_s)
    for assignment in options.server_env:
        key, value = assignment.split("=", 1)
        env[key] = value
    server_command = [
        str(BASELINE.binary(options, "kcd2s-server")),
        str(options.port),
        "127.0.0.1",
        MAP,
        options.node,
        str(RESOURCE_HOST_ENTRY),
        str(RESOURCE_DIR),
    ]
    processes = max(1, math.ceil(scenario.clients / CLIENTS_PER_GENERATOR))
    print(f"[{scenario.name}] server on port {options.port}, {processes} generator(s)", flush=True)
    server = subprocess.Popen(
        server_command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        cwd=raw_dir,
    )
    server_reader = BASELINE.OutputReader(server.stdout)
    sampler = CoreSampler()
    churn = ChurnDriver(options, scenario, raw_dir) if scenario.churn else None
    probes = []
    probe_exits = []
    started = time.monotonic()
    try:
        if not server_reader.wait_for_prefix("RESOURCE HOST ready", 30):
            raise RuntimeError("Server or resource host did not report readiness")
        if scenario.shared_state_hz > 0:
            send_console(server, f"capacity shared_state {scenario.shared_state_hz:g}")
        if scenario.events_hz > 0:
            send_console(server, f"capacity events {scenario.events_hz:g}")
        sampler.start()
        offset = 0
        for index in range(processes):
            share = scenario.clients // processes + (
                1 if index < scenario.clients % processes else 0
            )
            command = probe_command(
                options,
                scenario,
                share,
                offset,
                scenario.clients,
                scenario.duration_s,
                "graceful",
            )
            offset += share
            probe = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                cwd=raw_dir,
            )
            probes.append((probe, BASELINE.OutputReader(probe.stdout)))
        if churn:
            churn.start()
        budget_s = scenario.duration_s + join_timeout(scenario) + options.warmup + 120
        deadline = time.monotonic() + budget_s
        for probe, reader in probes:
            try:
                probe_exits.append(probe.wait(timeout=max(1.0, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                probe.kill()
                probe_exits.append(probe.wait())
            reader.thread.join(timeout=5)
        if churn:
            churn.stop()
        time.sleep(options.metrics_interval * 2 + 0.5)
    finally:
        sampler.stop()
        for probe, _ in probes:
            if probe.poll() is None:
                probe.kill()
                probe.wait()
        if churn:
            churn.stop_event.set()
        send_console(server, "capacity idle")
        server_stop = BASELINE.stop_server(server)
        server_reader.thread.join(timeout=5)
    (raw_dir / "server.log").write_text("\n".join(server_reader.lines) + "\n", encoding="utf8")
    summaries = []
    client_lines = []
    for index, (_, reader) in enumerate(probes):
        (raw_dir / f"probe-{index}.log").write_text("\n".join(reader.lines) + "\n", encoding="utf8")
        found = BASELINE.tagged_json(reader.lines, "KCDS_LOAD_SUMMARY")
        if found:
            summaries.append(found[-1])
        client_lines.extend(BASELINE.tagged_json(reader.lines, "KCDS_LOAD_CLIENT"))
    if any(code != 0 for code in probe_exits) or len(summaries) != len(probes):
        raise RuntimeError(f"[{scenario.name}] probe failed with exit codes {probe_exits}")
    probe_summary = BASELINE.merge_probe_results(summaries, client_lines)
    start_ms = probe_summary["window_start_unix_ms"]
    end_ms = probe_summary["window_end_unix_ms"]
    metric_lines = BASELINE.tagged_json(server_reader.lines, "KCDS_METRICS")
    configuration = BASELINE.tagged_json(server_reader.lines, "KCDS_METRICS_CONFIG")
    server_summary = BASELINE.summarize_server(metric_lines, start_ms, end_ms)
    window_s = probe_summary["window_s"]
    clients_out = probe_summary["traffic"]["admitted_clients"]["bytes_sent"] / window_s
    clients_in = probe_summary["traffic"]["admitted_clients"]["bytes_received"] / window_s
    if (
        server_summary.get("intervals", 0)
        and clients_out
        and server_summary["udp"]["bytes_out_per_s"]
    ):
        server_summary["delivery"] = {
            "server_in_over_clients_out": server_summary["udp"]["bytes_in_per_s"] / clients_out,
            "clients_in_over_server_out": clients_in / server_summary["udp"]["bytes_out_per_s"],
        }
    resource_lines = [line for line in server_reader.lines if line.startswith("RESOURCE")]
    run = {
        "scenario": scenario.describe(),
        "snapshot_workers": workers_of(scenario, options),
        "generator_processes": processes,
        "probe_exit_codes": probe_exits,
        "server_stop": server_stop,
        "server_exit_code": server.returncode,
        "server_configuration": configuration[0] if configuration else None,
        "server": server_summary,
        "server_extra": summarize_extra(metric_lines, start_ms, end_ms),
        "probe": probe_summary,
        "sdk": merge_sdk(summaries),
        "cpu_per_core": sampler.summary(start_ms, end_ms),
        "churn": churn.summary() if churn else None,
        "resource_log_sample": resource_lines[:20],
        "wall_seconds": time.monotonic() - started,
        "raw_logs": str(raw_dir),
    }
    run["verdicts"] = evaluate(budgets, measured_values(scenario, run))
    admitted = probe_summary["joins"]["admitted"]
    print(
        f"[{scenario.name}] admitted {admitted}/{scenario.clients}, "
        f"server stop: {server_stop}, wall {run['wall_seconds']:.0f} s",
        flush=True,
    )
    return run


# ---------------------------------------------------------------------------------------------
# Report


LIMITATIONS = [
    "Loss and RTT are emulated inside the probe (netsim): fixed one-way delay without jitter, "
    "applied when a client polls its socket (rounded up to its 60 Hz tick), loss independent per "
    "direction. No OS-level shaper is installed on this machine.",
    "Server, generators and the user's other processes share one machine and the loopback "
    "interface; loopback has no real NIC, MTU or kernel queue limits. Per-core CPU is machine-wide.",
    "Probes are headless KSTP clients: no game engine, no client resource runtime (WebView). "
    "They acknowledge SDK entity frames and count them, but do not decode directories/SharedStates.",
    "Targeted resource events are not measured: the server SDK has no per-client emit yet "
    "(kcds.core.Events.broadcast only).",
    "Resource download/hot reload, stalled readers and scope changes while data is in flight are "
    "not part of this runner; they remain separate acceptance items.",
    "Oldest-delivery age of reliable queues is not instrumented (only depths and retransmissions).",
    "Script event-loop lag inside the resource host is not instrumented; bridge write wait and "
    "inbox drops are the visible symptoms.",
    "Allocations are not counted; RSS (working set) and its trend are.",
]


def fmt(value, pattern="{:.1f}"):
    if value is None:
        return "n/a"
    return pattern.format(value)


def kib(value):
    return fmt(value / 1024.0 if value is not None else None)


def markdown(report):
    env = report["environment"]
    lines = [
        f"# KCDS capacity benchmark ({report['matrix']} matrix)",
        "",
        f"**{report['label']}**",
        "",
        f"- Commit: `{env['commit']}` (branch `{env['branch']}`, "
        f"tracked changes: {env['tracked_changes']})",
        f"- Build: {env['build_config']} ({env['cmake_generator']}), compiler `{env['cxx_compiler']}`",
        f"- OS: {env['os']}",
        f"- CPU: {env['cpu_model']} ({env['physical_cores']} physical / "
        f"{env['logical_cores']} logical cores)",
        (
            f"- RAM: {env['total_memory_bytes'] / 2**30:.1f} GiB"
            if env["total_memory_bytes"]
            else "- RAM: unknown"
        ),
        f"- Started (UTC): {env['started_utc']}; budgets declared (UTC): "
        f"{report['budgets_declared_utc']}",
        f"- Settings: `{json.dumps(report['settings'], sort_keys=True)}`",
        "",
        "## Runs",
        "",
        "| Run | Clients | Admitted | Tick work p50/p95/p99/max ms | Phases p99 ms "
        "(ingress/server: receive, build, send/resources) | Missed | Server CPU % core | Machine CPU mean / "
        "busiest core % | RSS max MiB (growth) | Server out KiB/s total (per peer) | Server in "
        "KiB/s total (per peer) | Client up/down KiB/s per peer | Packets/s per peer out/in | "
        "Retransmit % | Convergence p50/p95/p99 ms | Event p95 ms | SDK frames/s per client | "
        "Bridge KiB/s to host (wait ms/s) |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for run in report["runs"]:
        if "error" in run:
            lines.append(f"| {run['scenario']['name']} | error: {run['error']} |" + " |" * 16)
            continue
        scenario = run["scenario"]
        server = run["server"]
        extra = run["server_extra"]
        probe = run["probe"]
        joins = probe["joins"]
        traffic = probe["traffic"]
        convergence = probe["replication"]["convergence"]
        cores = run["cpu_per_core"]
        bridge = extra.get("resource_bridge")
        reliable = extra.get("reliable")
        latency = run["sdk"]["event_latency"]
        window = probe["window_s"]
        if server.get("intervals", 0):
            work = server["work_ms"]
            phases = server["phases"]

            def phase(name):
                return fmt(phases.get(name, {}).get("p99_ms_worst"), "{:.2f}")

            tick_cell = (
                f"{work['p50_median_of_intervals']:.2f}/{work['p95_worst_interval']:.2f}/"
                f"{work['p99_worst_interval']:.2f}/{work['max']:.2f}"
            )
            server_parts = ", ".join(
                [phase("server_receive"), phase("snapshot_build"), phase("snapshot_send")]
            )
            phase_cell = "/".join(
                [phase("ingress"), f"{phase('server')}: {server_parts}", phase("resources")]
            )
            missed = str(server["missed_deadlines"])
            server_cpu = fmt(server["process"]["cpu_percent_of_one_core_mean"])
            out_cell = (
                f"{kib(server['udp']['bytes_out_per_s'])} "
                f"({kib(server['per_peer']['bytes_out_per_s_mean'])})"
            )
            in_cell = (
                f"{kib(server['udp']['bytes_in_per_s'])} "
                f"({kib(server['per_peer']['bytes_in_per_s_mean'])})"
            )
        else:
            tick_cell = phase_cell = missed = server_cpu = out_cell = in_cell = "n/a"
        rss = extra.get("rss")
        rss_cell = (
            f"{rss['max_bytes'] / 2**20:.0f} ({rss['growth_fraction'] * 100:+.1f}%)"
            if rss
            else "n/a"
        )
        packets = extra.get("packets_per_peer")
        packets_cell = f"{packets['out_per_s']:.0f}/{packets['in_per_s']:.0f}" if packets else "n/a"
        sdk_rate = (
            run["sdk"]["entity_frames"] / joins["avatars_ready"] / window
            if joins["avatars_ready"] and window
            else None
        )
        lines.append(
            "| "
            + " | ".join(
                [
                    f"{scenario['name']} (workers {run.get('snapshot_workers', '?')})",
                    str(scenario["clients"]),
                    str(joins["admitted"]),
                    tick_cell,
                    phase_cell,
                    missed,
                    server_cpu,
                    (
                        f"{cores['machine_mean_percent']:.0f} / {cores['busiest_core_mean_percent']:.0f}"
                        if cores
                        else "n/a"
                    ),
                    rss_cell,
                    out_cell,
                    in_cell,
                    f"{kib(traffic.get('bytes_sent_per_admitted_client_per_s'))}/"
                    f"{kib(traffic.get('bytes_received_per_admitted_client_per_s'))}",
                    packets_cell,
                    fmt(reliable["retransmit_fraction"] * 100 if reliable else None, "{:.2f}"),
                    f"{convergence['p50_ms']:.1f}/{convergence['p95_ms']:.1f}/"
                    f"{convergence['p99_ms']:.1f}",
                    fmt(latency["p95_ms"] if latency and latency["count"] else None),
                    fmt(sdk_rate, "{:.2f}"),
                    (
                        f"{bridge['bytes_to_host_per_s'] / 1024:.1f} "
                        f"({bridge['write_wait_ms_per_s']:.2f})"
                        if bridge
                        else "n/a"
                    ),
                ]
            )
            + " |"
        )
    lines += ["", "## Budgets (declared before measuring) and verdicts", ""]
    for run in report["runs"]:
        name = run["scenario"]["name"]
        lines.append(f"### {name}")
        lines.append("")
        lines.append("| Budget | Limit | Measured | Verdict | Basis |")
        lines.append("|---|---|---|---|---|")
        budgets = report["budgets"][name]
        verdicts = run.get("verdicts", {})
        for key, budget in budgets.items():
            verdict = verdicts.get(key, {"value": None, "verdict": "not run"})
            comparison = "<=" if budget["comparison"] == "max" else ">="
            lines.append(
                f"| {key} | {comparison} {budget['limit']:.4g} | "
                f"{fmt(verdict['value'], '{:.4g}')} | {verdict['verdict']} | {budget['basis']} |"
            )
        if run.get("churn"):
            churn = run["churn"]
            lines.append("")
            lines.append(
                f"Churn: {churn['groups']} groups of {churn['clients_per_group']} clients every "
                f"{churn['period_s']:.0f} s (life {churn['life_s']:.0f} s, alternating graceful "
                f"and timeout leave): {churn['admitted']}/{churn['requested']} admitted."
            )
        lines.append("")
    lines += ["## What this runner cannot measure on this machine", ""]
    lines += [f"- {item}" for item in LIMITATIONS]
    lines.append("")
    return "\n".join(lines) + "\n"


def node_executable(options):
    if options.node:
        return options.node
    cached = BASELINE.cmake_cache_value(options.build_dir, "KCDS_NODE_EXECUTABLE")
    if cached:
        return cached
    return "node"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--matrix", choices=["smoke", "full"], default="smoke")
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="measurement seconds of the core runs (smoke 60, full 1800)",
    )
    parser.add_argument(
        "--focus-duration",
        type=float,
        default=300.0,
        help="measurement seconds of the full matrix focus/churn/network/worker runs",
    )
    parser.add_argument("--warmup", type=float, default=None, help="seconds (smoke 5, full 60)")
    parser.add_argument("--input-fields", default="full", choices=["full", "compact"])
    parser.add_argument(
        "--spacing",
        type=float,
        default=50.0,
        help="distributed grid spacing in metres; 50 m with the 150 m interest radius gives each "
        "player a bounded neighbourhood (about 28 others)",
    )
    parser.add_argument("--metrics-interval", type=float, default=1.0)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="private port; never 7777")
    parser.add_argument("--node", default=None, help="node executable for the resource host")
    parser.add_argument(
        "--peer-snapshot-bytes-per-s",
        type=int,
        default=256 * 1024,
        help="KCDS_SERVER_PEER_SNAPSHOT_BYTES_PER_S for every run (budgets derive from it)",
    )
    parser.add_argument(
        "--server-env",
        action="append",
        default=[],
        metavar="KCDS_SERVER_NAME=VALUE",
        help="extra server environment for every run",
    )
    parser.add_argument(
        "--snapshot-workers",
        type=int,
        default=4,
        help="KCDS_SERVER_SNAPSHOT_WORKERS of every run outside the worker sweep (0/1 = serial)",
    )
    parser.add_argument("--only", default=None, help="run only scenarios whose name contains this")
    parser.add_argument("--dry-run", action="store_true", help="print the plan and budgets only")
    parser.add_argument("--out-dir", required=False)
    options = parser.parse_args()
    if options.port == LIVE_PORT:
        parser.error("use a private port; 7777 is the live server port")
    for assignment in options.server_env:
        if "=" not in assignment or not assignment.startswith("KCDS_SERVER_"):
            parser.error("--server-env expects KCDS_SERVER_<NAME>=<value>")
    if options.warmup is None:
        options.warmup = 5.0 if options.matrix == "smoke" else 60.0
    options.node = node_executable(options)

    if options.matrix == "smoke":
        scenarios = smoke_matrix(options)
    else:
        scenarios = full_matrix(options)
    if options.only:
        scenarios = [scenario for scenario in scenarios if options.only in scenario.name]
    budgets = {scenario.name: declare_budgets(scenario, options) for scenario in scenarios}
    total = sum(expected_seconds(scenario, options) for scenario in scenarios)
    print(
        f"{len(scenarios)} run(s), expected wall time about {total / 3600:.1f} h "
        f"({total / 60:.0f} min)",
        flush=True,
    )
    for scenario in scenarios:
        print(
            f"  {scenario.name}: {scenario.clients} clients, {scenario.layout}, "
            f"{scenario.duration_s:.0f} s, about {expected_seconds(scenario, options) / 60:.1f} min"
        )
    if options.dry_run:
        print(json.dumps(budgets, indent=2))
        return
    if not options.out_dir:
        parser.error("--out-dir is required unless --dry-run")

    out_dir = Path(options.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "schema": "kcds.capacity.report.v1",
        "matrix": options.matrix,
        "label": (
            "smoke run: proves the runner and workloads work; not capacity evidence"
            if options.matrix == "smoke"
            else "capacity benchmark; budgets are gates, not a readiness claim"
        ),
        "environment": BASELINE.environment(options),
        "settings": {
            "tick_hz": TICK_HZ,
            "warmup_s": options.warmup,
            "input_fields": options.input_fields,
            "distributed_spacing_m": options.spacing,
            "metrics_interval_s": options.metrics_interval,
            "port": options.port,
            "clients_per_generator_process": CLIENTS_PER_GENERATOR,
            "peer_snapshot_bytes_per_s": options.peer_snapshot_bytes_per_s,
            "snapshot_workers_outside_sweep": options.snapshot_workers,
            "server_env": options.server_env,
            "resource_host": str(RESOURCE_HOST_ENTRY.relative_to(ROOT)),
            "workload_resource": str(RESOURCE_DIR.relative_to(ROOT)),
            "node": options.node,
            "server_address": "127.0.0.1 (loopback, server and generators on one machine)",
        },
        "plan": [scenario.describe() for scenario in scenarios],
        "expected_wall_seconds": total,
        "budgets_declared_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "budgets": budgets,
        "limitations": LIMITATIONS,
        "runs": [],
    }
    report_path = out_dir / "report.json"
    # Budgets reach the disk before the first measurement starts.
    report_path.write_text(json.dumps(report, indent=2), encoding="utf8")
    for scenario in scenarios:
        try:
            run = run_scenario(options, scenario, out_dir, budgets[scenario.name])
        except Exception as error:  # keep the remaining runs of an overnight matrix
            print(f"[{scenario.name}] failed: {error}", flush=True)
            run = {"scenario": scenario.describe(), "error": str(error)}
        report["runs"].append(run)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf8")
        (out_dir / "report.md").write_text(markdown(report), encoding="utf8")
    print(f"Report: {report_path} and {out_dir / 'report.md'}")


if __name__ == "__main__":
    main()
