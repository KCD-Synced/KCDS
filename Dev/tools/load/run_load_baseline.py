"""Run kcd2s-server with opt-in metrics against kcd2s-load-probe and write a JSON + Markdown report.

measurement tooling. Every run starts a fresh server on a private port, runs the probe
for one (client count, workload) pair, stops the server through its console ("stop"), and keeps
the raw output. The report records the exact commit, build, machine and settings. A short run of
the current implementation is a baseline, not capacity evidence.

With --generator-processes N the clients of a run are split over N probe processes that share one
layout (--index-offset/--layout-count), so a single generator thread does not limit the result.
Their results are merged here: convergence from the probes' lossless histograms, per-client values
from the per-client lines. Cross-process convergence relies on the shared steady clock of one
machine. Server settings can be passed with --server-env KEY=VALUE (for example
KCDS_SERVER_MAX_PEERS=100).
"""

import argparse
import ctypes
import datetime
import json
import math
import os
from pathlib import Path
import platform
import queue
import statistics
import subprocess
import sys
import shutil
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
SERVER_READY_PREFIX = "KCD2-Synced UDP"
# Every server requires a legal game: the server runs its script host with a test
# Master, and every probe client joins with a pass from it (Dev/testkit/licence_fixture.py).
sys.path.insert(0, str(ROOT / "Dev" / "testkit"))
from licence_fixture import LicenceFixture  # noqa: E402


class OutputReader:
    """Collect a child's stdout lines on a thread so pipes never fill up."""

    def __init__(self, stream):
        self.lines = []
        self.events = queue.Queue()
        self.thread = threading.Thread(target=self._read, args=(stream,), daemon=True)
        self.thread.start()

    def _read(self, stream):
        for raw in stream:
            line = raw.rstrip("\r\n")
            self.lines.append(line)
            self.events.put(line)

    def wait_for_prefix(self, prefix, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if line.startswith(prefix):
                return True
        return False


# DurationHistogram bucket layout (Backend/integration/local/metrics/duration_histogram.hpp).
FINE_WIDTH_US, FINE_LIMIT_US = 10, 1000
MEDIUM_WIDTH_US, MEDIUM_LIMIT_US = 100, 100000
COARSE_WIDTH_US, COARSE_LIMIT_US = 10000, 10000000
FINE_BUCKETS = FINE_LIMIT_US // FINE_WIDTH_US
MEDIUM_BUCKETS = (MEDIUM_LIMIT_US - FINE_LIMIT_US) // MEDIUM_WIDTH_US
COARSE_BUCKETS = (COARSE_LIMIT_US - MEDIUM_LIMIT_US) // COARSE_WIDTH_US


def bucket_upper_ms(index):
    if index < FINE_BUCKETS:
        return (index + 1) * FINE_WIDTH_US / 1000.0
    if index < FINE_BUCKETS + MEDIUM_BUCKETS:
        offset = index - FINE_BUCKETS
        return (FINE_LIMIT_US + (offset + 1) * MEDIUM_WIDTH_US) / 1000.0
    if index < FINE_BUCKETS + MEDIUM_BUCKETS + COARSE_BUCKETS:
        offset = index - FINE_BUCKETS - MEDIUM_BUCKETS
        return (MEDIUM_LIMIT_US + (offset + 1) * COARSE_WIDTH_US) / 1000.0
    return float("inf")


def merge_histograms(histograms):
    """Merge lossless probe histograms and describe them like the probe does."""
    buckets = {}
    count = 0
    total = 0.0
    maximum = 0.0
    for histogram in histograms:
        count += histogram["count"]
        total += histogram["sum_ms"] or 0.0
        maximum = max(maximum, histogram["max_ms"] or 0.0)
        for index, samples in histogram["buckets"].items():
            buckets[int(index)] = buckets.get(int(index), 0) + samples

    def percentile(p):
        if count == 0:
            return 0.0
        rank = max(1, math.ceil(p / 100.0 * count))  # nearest rank, as in the probe
        seen = 0
        for index in sorted(buckets):
            seen += buckets[index]
            if seen >= rank:
                return min(bucket_upper_ms(index), maximum)
        return maximum

    return {
        "count": count,
        "mean_ms": total / count if count else 0.0,
        "p50_ms": percentile(50),
        "p95_ms": percentile(95),
        "p99_ms": percentile(99),
        "max_ms": maximum,
    }


def describe_values(values):
    if not values:
        return {"count": 0, "min": 0.0, "mean": 0.0, "max": 0.0, "p50": 0.0}
    return {
        "count": len(values),
        "min": min(values),
        "mean": statistics.mean(values),
        "max": max(values),
        "p50": statistics.median(values),
    }


def merge_probe_results(summaries, client_lines):
    """Combine the summaries and per-client lines of one or more probe processes."""
    window_start = max(summary["window_start_unix_ms"] for summary in summaries)
    window_end = min(summary["window_end_unix_ms"] for summary in summaries)
    starts = [summary["window_start_unix_ms"] for summary in summaries]
    window_s = statistics.mean(summary["window_s"] for summary in summaries)
    admitted = [line for line in client_lines if line["admitted"]]
    ready = [line for line in client_lines if line["avatar_ms"] >= 0]
    joins = {
        "requested": sum(summary["joins"]["requested"] for summary in summaries),
        "admitted": sum(summary["joins"]["admitted"] for summary in summaries),
        "not_admitted": sum(summary["joins"]["not_admitted"] for summary in summaries),
        "avatars_ready": sum(summary["joins"]["avatars_ready"] for summary in summaries),
        "clients_refused_server_full": sum(
            summary["joins"]["clients_refused_server_full"] for summary in summaries
        ),
        "refusals_received_total": sum(
            summary["joins"]["refusals_received_total"] for summary in summaries
        ),
    }
    join_times = [line["join_ms"] for line in admitted]
    joins["time_to_join"] = {
        "p50_ms": statistics.median(join_times) if join_times else 0.0,
        "max_ms": max(join_times) if join_times else 0.0,
    }

    def traffic_sum(key):
        result = {}
        for name in (
            "packets_sent",
            "bytes_sent",
            "packets_received",
            "bytes_received",
        ):
            result[name] = sum(summary["traffic"][key][name] for summary in summaries)
        return result

    admitted_traffic = traffic_sum("admitted_clients")
    traffic = {
        "admitted_clients": admitted_traffic,
        "not_admitted_clients": traffic_sum("not_admitted_clients"),
    }
    if joins["admitted"] and window_s > 0:
        traffic["bytes_sent_per_admitted_client_per_s"] = (
            admitted_traffic["bytes_sent"] / joins["admitted"] / window_s
        )
        traffic["bytes_received_per_admitted_client_per_s"] = (
            admitted_traffic["bytes_received"] / joins["admitted"] / window_s
        )

    snapshot_rates = [line["applied_snapshots_per_s"] for line in ready]
    visible = [line["visible_players"] for line in ready]
    update_rates = [line.get("updates_per_s_per_visible_player", 0.0) for line in ready]
    rates = describe_values(snapshot_rates)
    replication = {
        "applied_snapshots_per_client": {
            "clients": rates["count"],
            "min_hz": rates["min"],
            "mean_hz": rates["mean"],
            "max_hz": rates["max"],
        },
        "visible_players": describe_values(visible),
        "updates_per_s_per_visible_player": describe_values(update_rates),
        "convergence": merge_histograms(
            [summary["replication"]["convergence_histogram"] for summary in summaries]
        ),
    }
    generators = [summary["generator"] for summary in summaries]
    generator = {
        "processes": len(generators),
        "cpu_percent_of_one_core": sum(g["cpu_percent_of_one_core"] for g in generators),
        "cpu_percent_of_one_core_max_process": max(
            g["cpu_percent_of_one_core"] for g in generators
        ),
        "loop_busy_fraction": max(g["loop_busy_fraction"] for g in generators),
        "achieved_tick_hz_min": min(g["achieved_tick_hz"] for g in generators),
        "skipped_input_slots": sum(g["skipped_input_slots"] for g in generators),
        "late_ticks": sum(g["late_ticks"] for g in generators),
        "peak_working_set_bytes_max": max(g["peak_working_set_bytes"] for g in generators),
    }
    return {
        "window_start_unix_ms": window_start,
        "window_end_unix_ms": window_end,
        "window_start_skew_ms": max(starts) - min(starts),
        "window_s": window_s,
        "joins": joins,
        "traffic": traffic,
        "replication": replication,
        "generator": generator,
    }


def tagged_json(lines, tag):
    """Return the JSON objects of all lines that start with `tag `."""
    prefix = tag + " "
    result = []
    for line in lines:
        if line.startswith(prefix):
            result.append(json.loads(line[len(prefix) :]))
    return result


def git_output(*arguments):
    try:
        return subprocess.check_output(["git", *arguments], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def windows_cpu_model():
    try:
        import winreg

        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
        )
        value, _ = winreg.QueryValueEx(key, "ProcessorNameString")
        return value.strip()
    except OSError:
        return None


def windows_physical_cores():
    command = [
        "powershell",
        "-NoProfile",
        "-Command",
        "(Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfCores -Sum).Sum",
    ]
    try:
        return int(subprocess.check_output(command, text=True, timeout=30).strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def windows_total_memory():
    class MemoryStatus(ctypes.Structure):
        _fields_ = [
            ("length", ctypes.c_ulong),
            ("memory_load", ctypes.c_ulong),
            ("total_physical", ctypes.c_ulonglong),
            ("available_physical", ctypes.c_ulonglong),
            ("total_page_file", ctypes.c_ulonglong),
            ("available_page_file", ctypes.c_ulonglong),
            ("total_virtual", ctypes.c_ulonglong),
            ("available_virtual", ctypes.c_ulonglong),
            ("available_extended_virtual", ctypes.c_ulonglong),
        ]

    status = MemoryStatus()
    status.length = ctypes.sizeof(MemoryStatus)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return status.total_physical
    return None


def linux_cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def linux_total_memory():
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        pass
    return None


def cmake_cache_value(build_dir, name):
    cache = Path(build_dir) / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf8", errors="replace").splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1]
    return None


def cmake_compiler(build_dir):
    """Compiler id and version recorded by CMake (multi-config generators do not cache a path)."""
    for record in sorted((Path(build_dir) / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        values = {}
        for line in record.read_text(encoding="utf8", errors="replace").splitlines():
            for name in ("CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
                if line.startswith(f"set({name} "):
                    values[name] = line.split(" ", 1)[1].rstrip(")").strip('"')
        if values:
            return " ".join(values.get(name, "?") for name in sorted(values))
    return None


def environment(options):
    is_windows = sys.platform == "win32"
    dirty = git_output("status", "--porcelain", "--untracked-files=no")
    return {
        "commit": git_output("rev-parse", "HEAD"),
        "branch": git_output("rev-parse", "--abbrev-ref", "HEAD"),
        "tracked_changes": bool(dirty),
        "build_dir": str(Path(options.build_dir).resolve()),
        "build_config": options.config,
        "cmake_generator": cmake_cache_value(options.build_dir, "CMAKE_GENERATOR"),
        "cxx_compiler": cmake_compiler(options.build_dir),
        "os": platform.platform(),
        "cpu_model": windows_cpu_model() if is_windows else linux_cpu_model(),
        "physical_cores": windows_physical_cores() if is_windows else None,
        "logical_cores": os.cpu_count(),
        "total_memory_bytes": (windows_total_memory() if is_windows else linux_total_memory()),
        "python": platform.python_version(),
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }


def binary(options, name):
    suffix = ".exe" if sys.platform == "win32" else ""
    candidates = [
        Path(options.build_dir) / "Backend" / "integration" / options.config / (name + suffix),
        Path(options.build_dir) / "Backend" / "integration" / (name + suffix),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"{name} not found under {options.build_dir}")


def window_lines(metric_lines, window_start_ms, window_end_ms):
    """Server interval lines whose covered interval lies inside the probe measurement window."""
    selected = []
    for line in metric_lines:
        end = line["unix_ms"]
        begin = end - line["interval_s"] * 1000.0
        if begin >= window_start_ms - 50 and end <= window_end_ms + 50:
            selected.append(line)
    return selected


def summarize_server(metric_lines, window_start_ms, window_end_ms):
    lines = window_lines(metric_lines, window_start_ms, window_end_ms)
    if not lines:
        return {"intervals": 0}
    seconds = sum(line["interval_s"] for line in lines)
    ticks = sum(line["tick"]["ticks"] for line in lines)
    work = [line["tick"]["work"] for line in lines]
    period = [line["tick"]["period"] for line in lines]
    udp = [line["udp"]["interval"] for line in lines]
    per_peer = [line["per_peer_interval"] for line in lines]
    phases = {}
    names = (
        "ingress",
        "timeouts",
        "server",
        "server_receive",
        "snapshot_build",
        "snapshot_send",
        "resources",
    )
    for name in names:
        if not all(name in line["tick"]["phases"] for line in lines):
            continue
        samples = [line["tick"]["phases"][name] for line in lines]
        phases[name] = {
            "p50_ms_median": statistics.median(s["p50_ms"] for s in samples),
            "p99_ms_worst": max(s["p99_ms"] for s in samples),
            "max_ms": max(s["max_ms"] for s in samples),
        }
    connected = [line["peers"]["connected"] for line in lines]
    bytes_in = sum(u["bytes_in"] for u in udp)
    bytes_out = sum(u["bytes_out"] for u in udp)
    peers_mean = statistics.mean(connected) if connected else 0
    last = lines[-1]
    return {
        "intervals": len(lines),
        "seconds": seconds,
        "ticks": ticks,
        "tick_rate_hz": ticks / seconds if seconds else 0,
        "work_ms": {
            "p50_median_of_intervals": statistics.median(w["p50_ms"] for w in work),
            "p95_worst_interval": max(w["p95_ms"] for w in work),
            "p99_worst_interval": max(w["p99_ms"] for w in work),
            "max": max(w["max_ms"] for w in work),
            "mean": (sum(w["mean_ms"] * w["count"] for w in work) / ticks if ticks else 0),
        },
        "period_ms": {
            "p50_median_of_intervals": statistics.median(p["p50_ms"] for p in period),
            "p99_worst_interval": max(p["p99_ms"] for p in period),
            "max": max(p["max_ms"] for p in period),
        },
        "phases": phases,
        "missed_deadlines": sum(line["tick"]["missed_deadlines"] for line in lines),
        # Fixed-rate scheduler fields (absent in metrics from the sleep-based loop).
        "late_finishes": sum(line["tick"].get("late_finishes", 0) for line in lines),
        "skipped_slots": sum(line["tick"].get("skipped_slots", 0) for line in lines),
        "start_lateness_ms": (
            {
                "p50_ms_median": statistics.median(
                    line["tick"]["start_lateness"]["p50_ms"] for line in lines
                ),
                "p99_ms_worst": max(line["tick"]["start_lateness"]["p99_ms"] for line in lines),
            }
            if all("start_lateness" in line["tick"] for line in lines)
            else None
        ),
        "connected_min": min(connected),
        "connected_max": max(connected),
        "udp": {
            "bytes_in_per_s": bytes_in / seconds,
            "bytes_out_per_s": bytes_out / seconds,
            "packets_in_per_s": sum(u["packets_in"] for u in udp) / seconds,
            "packets_out_per_s": sum(u["packets_out"] for u in udp) / seconds,
            "send_would_block": sum(u["send_would_block"] for u in udp),
        },
        "per_peer": {
            "bytes_out_per_s_mean": sum(p["bytes_out_mean"] for p in per_peer) / seconds,
            "bytes_out_per_s_max_interval": max(
                p["bytes_out_max"] / line["interval_s"] for p, line in zip(per_peer, lines)
            ),
            "bytes_in_per_s_mean": sum(p["bytes_in_mean"] for p in per_peer) / seconds,
            "connected_mean": peers_mean,
        },
        "queues": {
            "inbox_max": max(line["queues"]["inbox_max"] for line in lines),
            "outstanding_reliable_max": max(
                line["queues"]["outstanding_reliable_total"] for line in lines
            ),
            "inbox_overflow_drops_total": last["queues"]["inbox_overflow_drops_total"],
            "receive_budget_exhausted_ticks_total": last["queues"][
                "receive_budget_exhausted_ticks_total"
            ],
        },
        "refused_handshake_datagrams_total": last["peers"]["refused_handshake_datagrams_total"],
        "refusals_sent_total": last["peers"].get("refusals_sent_total"),
        "refusals_rate_limited_total": last["peers"].get("refusals_rate_limited_total"),
        "joins_total": last["peers"]["joins_total"],
        "process": {
            "cpu_percent_of_one_core_mean": statistics.mean(
                line["process"]["cpu_percent_of_one_core"] for line in lines
            ),
            "cpu_percent_of_one_core_max": max(
                line["process"]["cpu_percent_of_one_core"] for line in lines
            ),
            "working_set_bytes_max": max(line["process"]["working_set_bytes"] for line in lines),
        },
        "since_start_work_ms": last["since_start"]["work"],
        "snapshots": summarize_snapshots(lines),
    }


def summarize_snapshots(lines):
    """Snapshot budget and fragmentation counters (absent in metrics before phase 2)."""
    if not all("snapshots" in line for line in lines):
        return None
    totals = {}
    for name in (
        "built",
        "payload_bytes",
        "budget_limited",
        "deferred_entries",
        "deferred_constructs",
        "overdraw",
        "aggregate_limited_ticks",
        "fragmented_datagrams",
        "fragments",
    ):
        totals[name] = sum(line["snapshots"][name] for line in lines)
    last = lines[-1]["snapshots"]
    totals["largest_payload_bytes_since_start"] = last["largest_payload_bytes_since_start"]
    totals["largest_udp_payload_since_start"] = last["largest_udp_payload_since_start"]
    totals["budget_limited_fraction"] = (
        totals["budget_limited"] / totals["built"] if totals["built"] else 0.0
    )
    return totals


def stop_server(server):
    try:
        server.stdin.write("stop\n")
        server.stdin.flush()
    except OSError:
        pass
    try:
        server.wait(timeout=10)
        return "stopped"
    except subprocess.TimeoutExpired:
        server.terminate()
        server.wait(timeout=10)
        return "terminated"


def run_one(options, clients, workload, out_dir):
    name = f"{clients:04d}-{workload}"
    raw_dir = out_dir / "raw" / name
    raw_dir.mkdir(parents=True, exist_ok=True)
    resources = raw_dir / "resources"
    resources.mkdir(exist_ok=True)
    with LicenceFixture(raw_dir, resources) as licence:
        return run_with_licence(options, clients, workload, name, raw_dir, resources, licence)


def run_with_licence(options, clients, workload, name, raw_dir, resources, licence):
    env = licence.server_env()
    env["KCDS_SERVER_METRICS"] = str(options.metrics_interval)
    for assignment in options.server_env:
        key, value = assignment.split("=", 1)
        env[key] = value
    probe_env = licence.probe_env()
    server_command = [
        str(binary(options, "kcd2s-server")),
        str(options.port),
        "127.0.0.1",
        "trosecko",
        shutil.which("node") or "node",
        str(ROOT / "Backend" / "scripting" / "runtimes" / "js" / "main.cjs"),
        str(resources),
    ]
    processes = max(1, min(options.generator_processes, clients))
    probe_commands = []
    offset = 0
    for index in range(processes):
        share = clients // processes + (1 if index < clients % processes else 0)
        probe_commands.append(
            [
                str(binary(options, "kcd2s-load-probe")),
                "--port",
                str(options.port),
                "--clients",
                str(share),
                "--index-offset",
                str(offset),
                "--layout-count",
                str(clients),
                "--workload",
                workload,
                "--duration",
                str(options.duration),
                "--join-timeout",
                str(options.join_timeout),
                "--warmup",
                str(options.warmup),
                "--tick-hz",
                str(options.tick_hz),
                "--input-hz",
                str(options.input_hz),
                "--input-fields",
                options.input_fields,
                "--spacing",
                str(options.spacing),
            ]
        )
        offset += share
    print(f"[{name}] starting server on port {options.port}", flush=True)
    server = subprocess.Popen(
        server_command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        cwd=raw_dir,
    )
    server_reader = OutputReader(server.stdout)
    pids = {"server": server.pid, "probes": []}
    probes = []
    probe_exits = []
    try:
        if not server_reader.wait_for_prefix(SERVER_READY_PREFIX, 15):
            raise RuntimeError("Server did not report readiness")
        print(
            f"[{name}] server pid {server.pid}; starting {processes} probe(s)",
            flush=True,
        )
        budget = options.duration + options.join_timeout + options.warmup + 120
        for command in probe_commands:
            probe = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                cwd=raw_dir,
                env=probe_env,
            )
            pids["probes"].append(probe.pid)
            probes.append((probe, OutputReader(probe.stdout)))
        deadline = time.monotonic() + budget
        for probe, reader in probes:
            try:
                probe_exits.append(probe.wait(timeout=max(1.0, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                probe.kill()
                probe_exits.append(probe.wait())
            reader.thread.join(timeout=5)
        # Let the server finish the interval that contains the end of the window.
        time.sleep(options.metrics_interval * 2 + 0.5)
    finally:
        for probe, _ in probes:
            if probe.poll() is None:
                probe.kill()
                probe.wait()
        server_stop = stop_server(server)
        server_reader.thread.join(timeout=5)
    (raw_dir / "server.log").write_text("\n".join(server_reader.lines) + "\n", encoding="utf8")
    summaries = []
    client_lines = []
    for index, (_, reader) in enumerate(probes):
        (raw_dir / f"probe-{index}.log").write_text("\n".join(reader.lines) + "\n", encoding="utf8")
        found = tagged_json(reader.lines, "KCDS_LOAD_SUMMARY")
        if found:
            summaries.append(found[-1])
        client_lines.extend(tagged_json(reader.lines, "KCDS_LOAD_CLIENT"))
    if any(code != 0 for code in probe_exits) or len(summaries) != len(probes):
        raise RuntimeError(f"[{name}] probe failed with exit codes {probe_exits}")
    summary = merge_probe_results(summaries, client_lines)
    # UDP datagrams lost between the generator and the server (loopback: OS buffer drops).
    window_s = summary["window_s"]
    sent = summary["traffic"]["admitted_clients"]["bytes_sent"] / window_s if window_s else 0
    received = (
        summary["traffic"]["admitted_clients"]["bytes_received"] / window_s if window_s else 0
    )
    configuration = tagged_json(server_reader.lines, "KCDS_METRICS_CONFIG")
    metric_lines = tagged_json(server_reader.lines, "KCDS_METRICS")
    server_summary = summarize_server(
        metric_lines, summary["window_start_unix_ms"], summary["window_end_unix_ms"]
    )
    if server_summary.get("intervals", 0) and sent and server_summary["udp"]["bytes_out_per_s"]:
        server_summary["delivery"] = {
            "server_in_over_clients_out": server_summary["udp"]["bytes_in_per_s"] / sent,
            "clients_in_over_server_out": received / server_summary["udp"]["bytes_out_per_s"],
        }
    server_errors = [
        line for line in server_reader.lines if "exception" in line.lower() or "failed" in line
    ]
    print(
        f"[{name}] admitted {summary['joins']['admitted']}/{clients}, server stop: {server_stop}",
        flush=True,
    )
    return {
        "name": name,
        "clients_requested": clients,
        "workload": workload,
        "pids": pids,
        "probe_exit_codes": probe_exits,
        "generator_processes": processes,
        "server_env": options.server_env,
        "server_stop": server_stop,
        "server_exit_code": server.returncode,
        "server_configuration": configuration[0] if configuration else None,
        "server": server_summary,
        "server_error_lines": server_errors[:20],
        "probe": summary,
        "probe_processes": summaries,
        "probe_clients": client_lines,
        "raw_logs": str(raw_dir),
    }


def kib(value):
    return f"{value / 1024:.1f}"


def snapshot_cell(server, key, pattern):
    snapshots = server.get("snapshots") if server.get("intervals", 0) else None
    if not snapshots:
        return "n/a"
    return pattern.format(snapshots[key])


def markdown(report):
    env = report["environment"]
    settings = report["settings"]
    lines = [
        "# KCDS load baseline run",
        "",
        "Short baseline of the current implementation. Not capacity evidence.",
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
            else ""
        ),
        f"- Started (UTC): {env['started_utc']}",
        f"- Settings: `{json.dumps(settings, sort_keys=True)}`",
        "",
        "| Clients | Workload | Admitted | Not admitted | Join p50/max ms | Server tick work "
        "p50/p95/p99/max ms | Loop period p50 ms | Missed | Server out KiB/s (per peer) | "
        "Server in KiB/s (per peer) | Snapshots/s per client | Convergence p50/p95/p99 ms | "
        "Server CPU % core | Generator CPU % core / busy | Visible mean | Updates/s per visible | "
        "Budget-limited snapshots | Largest UDP payload | Server in / clients out |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for run in report["runs"]:
        probe = run["probe"]
        server = run["server"]
        joins = probe["joins"]
        join_time = joins["time_to_join"]
        convergence = probe["replication"]["convergence"]
        generator = probe["generator"]
        rates = probe["replication"]["applied_snapshots_per_client"]
        if server.get("intervals", 0) == 0:
            server_cells = ["n/a"] * 6
        else:
            work = server["work_ms"]
            server_cells = [
                f"{work['p50_median_of_intervals']:.3f}/{work['p95_worst_interval']:.3f}/"
                f"{work['p99_worst_interval']:.3f}/{work['max']:.3f}",
                f"{server['period_ms']['p50_median_of_intervals']:.1f}",
                str(server["missed_deadlines"]),
                f"{kib(server['udp']['bytes_out_per_s'])} "
                f"({kib(server['per_peer']['bytes_out_per_s_mean'])})",
                f"{kib(server['udp']['bytes_in_per_s'])} "
                f"({kib(server['per_peer']['bytes_in_per_s_mean'])})",
                f"{server['process']['cpu_percent_of_one_core_mean']:.1f}",
            ]
        lines.append(
            "| "
            + " | ".join(
                [
                    str(run["clients_requested"]),
                    run["workload"],
                    str(joins["admitted"]),
                    str(joins["not_admitted"]),
                    f"{join_time['p50_ms']:.0f}/{join_time['max_ms']:.0f}",
                    server_cells[0],
                    server_cells[1],
                    server_cells[2],
                    server_cells[3],
                    server_cells[4],
                    f"{rates['mean_hz']:.1f} (min {rates['min_hz']:.1f})",
                    f"{convergence['p50_ms']:.1f}/{convergence['p95_ms']:.1f}/"
                    f"{convergence['p99_ms']:.1f}",
                    server_cells[5],
                    f"{generator['cpu_percent_of_one_core']:.1f} / "
                    f"{generator['loop_busy_fraction']:.2f}",
                    f"{probe['replication']['visible_players']['mean']:.1f}",
                    f"{probe['replication']['updates_per_s_per_visible_player']['mean']:.1f}",
                    snapshot_cell(server, "budget_limited_fraction", "{:.1%}"),
                    snapshot_cell(server, "largest_udp_payload_since_start", "{}"),
                    (
                        "{:.3f}".format(server["delivery"]["server_in_over_clients_out"])
                        if server.get("delivery")
                        else "n/a"
                    ),
                ]
            )
            + " |"
        )
    lines.append("")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--clients", default="8,32,100", help="comma separated client counts")
    parser.add_argument("--workloads", default="distributed,dense", help="comma separated")
    parser.add_argument("--duration", type=float, default=60.0, help="measurement seconds")
    parser.add_argument("--join-timeout", type=float, default=15.0)
    parser.add_argument("--warmup", type=float, default=2.0)
    parser.add_argument("--tick-hz", type=float, default=60.0)
    parser.add_argument("--input-hz", type=float, default=60.0)
    parser.add_argument("--input-fields", default="full", choices=["full", "compact"])
    parser.add_argument("--metrics-interval", type=float, default=1.0)
    parser.add_argument("--port", type=int, default=7841, help="private port; never 7777")
    parser.add_argument("--spacing", type=float, default=200.0, help="distributed grid spacing (m)")
    parser.add_argument(
        "--generator-processes",
        type=int,
        default=1,
        help="split the clients of every run over this many probe processes",
    )
    parser.add_argument(
        "--server-env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="extra environment for kcd2s-server, e.g. KCDS_SERVER_MAX_PEERS=100",
    )
    parser.add_argument("--out-dir", required=True)
    options = parser.parse_args()
    if options.port == 7777:
        parser.error("use a private port; 7777 is the default live server port")
    for assignment in options.server_env:
        if "=" not in assignment or not assignment.startswith("KCDS_SERVER_"):
            parser.error("--server-env expects KCDS_SERVER_<NAME>=<value>")

    out_dir = Path(options.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "schema": "kcds.load.report.v1",
        "label": "short baseline of the current implementation; not capacity evidence",
        "environment": environment(options),
        "settings": {
            "clients": options.clients,
            "workloads": options.workloads,
            "duration_s": options.duration,
            "join_timeout_s": options.join_timeout,
            "warmup_s": options.warmup,
            "tick_hz": options.tick_hz,
            "input_hz": options.input_hz,
            "input_fields": options.input_fields,
            "metrics_interval_s": options.metrics_interval,
            "port": options.port,
            "spacing_m": options.spacing,
            "generator_processes": options.generator_processes,
            "server_env": options.server_env,
            "server_address": "127.0.0.1 (loopback, server and generator on one machine)",
        },
        "runs": [],
    }
    for clients in [int(value) for value in options.clients.split(",")]:
        for workload in options.workloads.split(","):
            report["runs"].append(run_one(options, clients, workload, out_dir))
            (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    (out_dir / "report.md").write_text(markdown(report), encoding="utf8")
    print(f"Report: {out_dir / 'report.json'} and {out_dir / 'report.md'}")


if __name__ == "__main__":
    main()
