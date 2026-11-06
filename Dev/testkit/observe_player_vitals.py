"""Observe identified KCDS clients with read-only process handles; never execute game code.

WHGame 1.5.5 layout: Client/runtime/stats/player_stats.hpp (actor+1640 soul, soul+1800 states)
and Client/runtime/knockout/knockout.cpp (soul buff vector+1352/+1360, instance definition+16).

Consciousness is a computed derived stat, not the seventh float after soul+1800. This observer
does not call its native getter. It records KO log transitions and buff presence as separate
evidence, and leaves native consciousness/dead/unconscious unknown. Screenshots/native
observations are still required to prove that a body is alive, unconscious or dead.
"""

import argparse
import csv
import ctypes
import ctypes.wintypes as windows
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid


IDA_BASE = 0x180000000
FRAMEWORK_GLOBAL = 0x185497F70
IMMORTALITY = "730503bf-735a-4f47-baae-c2d84ee77524"
PERMANENT_UNCONSCIOUSNESS = "c75aa0db-65ca-44d7-9001-e4b6d38c6875"
READINESS = re.compile(r"READINESS WH player=\S+ entity=\S+ id=0x([0-9a-f]+)", re.I)
OWNER = re.compile(r"FORCE_(?:XFORM|ROT) applied player=(\d+)")
REMOTE = re.compile(r"REMOTE player=(\d+) engine=0x([0-9a-f]+) class=Player", re.I)
REMOTE_REMOVED = re.compile(r"REMOTE removed player=(\d+)")
REPLICA_DOWN = re.compile(r"KNOCKOUT replica of player=(\d+) (down|up)")
TICK = re.compile(r"^\[(\d+)\]")


class ClientLog:
    def __init__(self, path):
        self.path = Path(path)
        self.offset = 0
        self.local_engine = None
        self.owner = None
        self.remotes = {}
        self.down = {}
        self.down_at = {}

    def accept(self, line):
        tick = int(match[1]) if (match := TICK.search(line)) else None
        if match := READINESS.search(line):
            self.local_engine = int(match[1], 16)
        if match := OWNER.search(line):
            self.owner = int(match[1])
        if match := REMOTE.search(line):
            self.remotes[int(match[1])] = int(match[2], 16)
        if match := REMOTE_REMOVED.search(line):
            self.remotes.pop(int(match[1]), None)
            self.down.pop(int(match[1]), None)
            self.down_at.pop(int(match[1]), None)
        if match := REPLICA_DOWN.search(line):
            self.down[int(match[1])] = match[2] == "down"
            self.down_at[int(match[1])] = tick
        if self.owner is not None and "KNOCKOUT local player knocked out" in line:
            self.down[self.owner] = True
            self.down_at[self.owner] = tick
        if self.owner is not None and "KNOCKOUT revive woke=1" in line:
            self.down[self.owner] = False
            self.down_at[self.owner] = tick

    def update(self):
        with self.path.open("r", encoding="utf-8", errors="replace") as source:
            source.seek(self.offset)
            for line in source:
                self.accept(line)
            self.offset = source.tell()

    def actors(self):
        if self.owner is None or self.local_engine is None:
            raise ValueError(f"{self.path}: missing current READINESS/FORCE identity evidence")
        return {**self.remotes, self.owner: self.local_engine}


class ProcessMemory:
    def __init__(self, pid, expected_runtime):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [windows.DWORD, windows.BOOL, windows.DWORD]
        self.kernel.OpenProcess.restype = windows.HANDLE
        self.kernel.CloseHandle.argtypes = [windows.HANDLE]
        self.kernel.ReadProcessMemory.argtypes = [
            windows.HANDLE,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self.psapi.EnumProcessModulesEx.argtypes = [
            windows.HANDLE,
            ctypes.POINTER(ctypes.c_void_p),
            windows.DWORD,
            ctypes.POINTER(windows.DWORD),
            windows.DWORD,
        ]
        self.psapi.GetModuleFileNameExW.argtypes = [
            windows.HANDLE,
            ctypes.c_void_p,
            windows.LPWSTR,
            windows.DWORD,
        ]
        # PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, deliberately no write/create-thread rights.
        self.handle = self.kernel.OpenProcess(0x10 | 0x400, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), f"OpenProcess({pid})")
        modules = (ctypes.c_void_p * 2048)()
        needed = windows.DWORD()
        if not self.psapi.EnumProcessModulesEx(
            self.handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3
        ):
            raise OSError(ctypes.get_last_error(), "EnumProcessModulesEx")
        paths = {}
        for address in modules[: min(needed.value // ctypes.sizeof(ctypes.c_void_p), len(modules))]:
            path = ctypes.create_unicode_buffer(32768)
            if self.psapi.GetModuleFileNameExW(self.handle, address, path, len(path)):
                paths[Path(path.value).name.lower()] = (address, Path(path.value))
        self.base, self.game_module = paths["whgame.dll"]
        _, runtime = paths["kcd2s-game-client-v4.dll"]
        if runtime.resolve() != expected_runtime.resolve():
            raise ValueError(f"PID {pid} loaded a different runtime: {runtime}")

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        output = ctypes.create_string_buffer(size)
        count = ctypes.c_size_t()
        if (
            not address
            or not self.kernel.ReadProcessMemory(
                self.handle, address, output, size, ctypes.byref(count)
            )
            or count.value != size
        ):
            raise OSError(ctypes.get_last_error(), f"ReadProcessMemory({address:#x}, {size})")
        return output.raw

    def pointer(self, address):
        return struct.unpack("<Q", self.read(address, 8))[0]

    def integer(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def actor_registry(self):
        framework = self.pointer(self.base + FRAMEWORK_GLOBAL - IDA_BASE)
        actor_system = self.pointer(framework + 1304)
        entity_system = self.pointer(actor_system + 48)
        head = self.pointer(actor_system + 64)
        node = self.pointer(head)
        actors = {}
        visited = set()
        while node and node != head:
            if node in visited or len(visited) >= 4000:
                raise ValueError("Actor registry is cyclic or exceeds the verified reader bound")
            visited.add(node)
            actors[self.integer(node + 16)] = self.pointer(node + 24)
            node = self.pointer(node)
        return actors, entity_system

    def buffs(self, soul):
        begin = self.pointer(soul + 1352)
        end = self.pointer(soul + 1360)
        if not begin and not end:
            return set()
        if not begin or end < begin or (end - begin) % 8 or end - begin > 4096:
            raise ValueError("Soul buff vector is outside the verified layout bounds")
        identifiers = set()
        for address in range(begin, end, 8):
            instance = self.pointer(address)
            definition = self.pointer(instance + 16) if instance else 0
            if definition:
                identifiers.add(str(uuid.UUID(bytes_le=self.read(definition, 16))))
        return identifiers

    def sample(self, engine, actors, entities):
        actor = actors[engine]
        entity = self.pointer(entities + 24 * (engine & 0x3FFFF) + 528)
        if (
            not actor
            or not entity
            or self.integer(actor + 48) != engine
            or self.integer(entity + 12) != engine
            or self.pointer(actor + 56) != entity
        ):
            raise ValueError(f"Engine entity {engine:#x} no longer has its logged actor identity")
        soul = self.pointer(actor + 1640)
        states = struct.unpack("<4f", self.read(soul + 1800, 16))
        if not all(math.isfinite(value) for value in states):
            raise ValueError("Soul contains non-finite states")
        row = dict(zip(("health", "stamina", "exhaust", "hunger"), states))
        movement = self.pointer(actor + 632)
        core = self.pointer(movement + 528) if movement else 0
        row["combat_state"] = self.integer(core + 56) if core else None
        try:
            buffs = self.buffs(soul)
            row["immortality_buff"] = IMMORTALITY in buffs
            row["permanent_unconsciousness_buff"] = PERMANENT_UNCONSCIOUSNESS in buffs
        except (OSError, ValueError) as error:
            row["buff_error"] = str(error)
        return row


class Agreement:
    def __init__(self, settle_ms, tolerance=0.01001):
        self.settle_ms = settle_ms
        self.tolerance = tolerance
        self.owners = {}
        self.owner_comparisons = {}
        self.last_observed_at = {}
        self.last_view_agreement = {}
        self.comparisons = 0
        self.settled_mismatches = 0
        self.transient_mismatches = 0
        self.zero_replica_alive_owner = 0
        self.missing_views = 0
        self.maximum_difference = 0.0

    def observe(self, now, network, owner, replicas):
        previous = self.owners.get(network)
        # Ignore float noise smaller than the precision published by the native client.
        value = round(owner, 2)
        if previous is None or previous[0] != value:
            self.owners[network] = (value, now)
        self.last_observed_at[network] = now
        self.last_view_agreement[network] = len(replicas) == 2 and all(
            abs(replica - owner) <= self.tolerance for replica in replicas
        )
        stable = now - self.owners[network][1] >= self.settle_ms
        if len(replicas) != 2:
            self.missing_views += 1
        timeline = []
        for replica in replicas:
            difference = abs(replica - owner)
            self.maximum_difference = max(self.maximum_difference, difference)
            if stable:
                self.comparisons += 1
                self.owner_comparisons[network] = self.owner_comparisons.get(network, 0) + 1
            status = "agreement"
            if replica <= 0 < owner:
                self.zero_replica_alive_owner += 1
                status = "zero_replica_alive_owner"
            if difference > self.tolerance:
                if stable:
                    self.settled_mismatches += 1
                    status = "settled_mismatch"
                else:
                    self.transient_mismatches += 1
                    status = "transient_lag"
            timeline.append(
                {
                    "difference": difference,
                    "owner_stable_ms": now - self.owners[network][1],
                    "settled": stable,
                    "status": status,
                }
            )
        return timeline

    def result(self):
        result = vars(self).copy()
        result.pop("owners")
        result.pop("last_observed_at")
        result.pop("last_view_agreement")
        result["unsettled_final_owners"] = [
            network
            for network, (_, changed) in self.owners.items()
            if self.last_observed_at[network] - changed < self.settle_ms
        ]
        result["final_mismatch_owners"] = [
            network for network, agrees in self.last_view_agreement.items() if not agrees
        ]
        result["hp_agreement_passed"] = (
            bool(self.owners)
            and all(self.owner_comparisons.get(network, 0) > 0 for network in self.owners)
            and self.settled_mismatches == 0
            and self.zero_replica_alive_owner == 0
            and self.missing_views == 0
            and not result["unsettled_final_owners"]
            and not result["final_mismatch_owners"]
        )
        result["native_lifecycle_proven"] = False
        return result


class KnockoutEvidence:
    """Compare observed owner KO intent with actual replica permanent-buff presence.

    This is deliberately separate from native unconscious/dead predicates, which remain unknown.
    Positive HP alone never supplies a KO state: an immortal knocked-out owner still has HP.
    """

    def __init__(self, settle_ms):
        self.settle_ms = settle_ms
        self.comparisons = 0
        self.mismatches = 0
        self.unknown_owner_samples = 0
        self.unknown_replica_samples = 0
        self.missing_views = 0
        self.states = {}

    def observe(self, now, network, down, changed_at, replica_buffs):
        if down is None or changed_at is None:
            self.unknown_owner_samples += 1
            return
        self.states[network] = down
        if len(replica_buffs) != 2:
            self.missing_views += 1
        if now - changed_at < self.settle_ms:
            return
        for present in replica_buffs:
            if present is None:
                self.unknown_replica_samples += 1
            else:
                self.comparisons += 1
                if present != down:
                    self.mismatches += 1

    def result(self):
        return {
            "settled_comparisons": self.comparisons,
            "settled_intent_buff_mismatches": self.mismatches,
            "unknown_owner_samples": self.unknown_owner_samples,
            "unknown_replica_samples": self.unknown_replica_samples,
            "missing_views": self.missing_views,
            "last_known_owner_intents": self.states,
            "native_lifecycle_proven": False,
        }


def identities(clients):
    requested = " OR ".join(f"ProcessId = {int(client[1])}" for client in clients)
    command = (
        f"Get-CimInstance Win32_Process -Filter '{requested}' | "
        "Select-Object ProcessId,Name,ExecutablePath,CommandLine | ConvertTo-Json -Compress"
    )
    output = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    records = json.loads(output)
    if isinstance(records, dict):
        records = [records]
    by_pid = {record["ProcessId"]: record for record in records}
    for label, pid, port, _ in clients:
        record = by_pid[int(pid)]
        if record["Name"].lower() != "kingdomcome.exe" or not re.search(
            rf"\+sv_port\s+{int(port)}\b", record["CommandLine"] or ""
        ):
            raise ValueError(f"Client {label} PID {pid} does not match the requested game/port")
    return by_pid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--client", nargs=4, action="append", metavar=("LABEL", "PID", "PORT", "LOG")
    )
    parser.add_argument("--runtime-dll", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--interval-ms", type=int, default=20)
    parser.add_argument("--settle-ms", type=int, default=300)
    options = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Live observation requires Windows")
    if (
        len(options.client or []) != 3
        or len({client[0] for client in options.client}) != 3
        or len({int(client[1]) for client in options.client}) != 3
        or options.seconds <= 0
        or options.interval_ms < 1
        or options.settle_ms < 0
    ):
        parser.error("Provide three distinct client labels/PIDs and positive sampling settings")
    identity = identities(options.client)
    readers = {}
    logs = {}
    try:
        for label, pid, _, path in options.client:
            readers[label] = ProcessMemory(int(pid), options.runtime_dll)
            logs[label] = ClientLog(path)
            logs[label].update()
            logs[label].actors()
        if len({log.owner for log in logs.values()}) != 3:
            raise ValueError("Three distinct network owners were not proven by the current logs")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetTickCount64.restype = ctypes.c_uint64
        agreement = Agreement(options.settle_ms)
        knockout = KnockoutEvidence(options.settle_ms)
        fields = [
            "tick_ms",
            "read_completed_ms",
            "label",
            "pid",
            "network",
            "engine",
            "owner",
            "health",
            "stamina",
            "exhaust",
            "hunger",
            "combat_state",
            "immortality_buff",
            "permanent_unconsciousness_buff",
            "ko_log_state",
            "ko_log_tick_ms",
            "consciousness",
            "native_dead",
            "native_unconscious",
            "buff_error",
            "error",
        ]
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.with_suffix(".identity.json").write_text(
            json.dumps(identity, indent=2), encoding="utf-8"
        )
        started = time.monotonic()
        next_sample = started
        errors = 0
        frames = 0
        timeline_path = options.output.with_suffix(".agreement.csv")
        with (
            options.output.open("w", newline="", encoding="utf-8") as destination,
            timeline_path.open("w", newline="", encoding="utf-8") as timeline_destination,
        ):
            writer = csv.DictWriter(destination, fieldnames=fields)
            writer.writeheader()
            timeline_writer = csv.DictWriter(
                timeline_destination,
                fieldnames=[
                    "tick_ms",
                    "view_tick_ms",
                    "network",
                    "owner_label",
                    "view_label",
                    "owner_health",
                    "replica_health",
                    "difference",
                    "owner_stable_ms",
                    "settled",
                    "status",
                ],
            )
            timeline_writer.writeheader()
            while time.monotonic() - started < options.seconds:
                frame = []
                for label, pid, _, _ in options.client:
                    log = logs[label]
                    log.update()
                    reader = readers[label]
                    actors, entities = reader.actor_registry()
                    for network, engine in log.actors().items():
                        row = {
                            "tick_ms": kernel.GetTickCount64(),
                            "label": label,
                            "pid": int(pid),
                            "network": network,
                            "engine": engine,
                            "owner": network == log.owner,
                            "ko_log_state": log.down.get(network),
                            "ko_log_tick_ms": log.down_at.get(network),
                        }
                        try:
                            row.update(reader.sample(engine, actors, entities))
                        except (OSError, ValueError, KeyError) as error:
                            row["error"] = str(error)
                            errors += 1
                        row["read_completed_ms"] = kernel.GetTickCount64()
                        writer.writerow(row)
                        frame.append(row)
                for network in {log.owner for log in logs.values()}:
                    views = [row for row in frame if row["network"] == network and "health" in row]
                    owner = next((row for row in views if row["owner"]), None)
                    if owner is None:
                        agreement.missing_views += 1
                        continue
                    replicas = [row for row in views if not row["owner"]]
                    comparisons = agreement.observe(
                        owner["tick_ms"],
                        network,
                        owner["health"],
                        [row["health"] for row in replicas],
                    )
                    knockout.observe(
                        owner["tick_ms"],
                        network,
                        owner["ko_log_state"],
                        owner["ko_log_tick_ms"],
                        [row.get("permanent_unconsciousness_buff") for row in replicas],
                    )
                    for replica, comparison in zip(replicas, comparisons):
                        timeline_writer.writerow(
                            {
                                "tick_ms": owner["tick_ms"],
                                "view_tick_ms": replica["tick_ms"],
                                "network": network,
                                "owner_label": owner["label"],
                                "view_label": replica["label"],
                                "owner_health": owner["health"],
                                "replica_health": replica["health"],
                                **comparison,
                            }
                        )
                frames += 1
                destination.flush()
                timeline_destination.flush()
                next_sample += options.interval_ms / 1000
                delay = next_sample - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_sample = time.monotonic()
        result = agreement.result()
        result["frames"] = frames
        result["elapsed_seconds"] = time.monotonic() - started
        result["read_errors"] = errors
        result["knockout_evidence"] = knockout.result()
        result["hp_agreement_passed"] = result["hp_agreement_passed"] and errors == 0
        options.output.with_suffix(".summary.json").write_text(
            json.dumps(result, indent=2), encoding="utf-8"
        )
        print(json.dumps(result, indent=2))
    finally:
        for reader in readers.values():
            reader.close()


if __name__ == "__main__":
    main()
