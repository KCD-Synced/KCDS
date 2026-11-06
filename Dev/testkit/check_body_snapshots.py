"""Compare explicit engine-thread body snapshots from three clients on one Windows host."""

import argparse
import json
import math
from pathlib import Path
import re


MARKER = re.compile(r"BODY SNAPSHOT (begin|row|end) (.*)$")


def latest_snapshot(lines):
    snapshot = None
    complete = False
    for line in lines:
        match = MARKER.search(line)
        if not match:
            continue
        kind, text = match.groups()
        fields = dict(part.split("=", 1) for part in text.split())
        sample = int(fields["sample"])
        if kind == "begin":
            snapshot = {key: int(value) for key, value in fields.items()}
            snapshot["bodies"] = {}
            complete = False
            continue
        if snapshot is None or sample != snapshot["sample"] or complete:
            raise ValueError("body row/end without its current begin")
        if kind == "row":
            player = int(fields["player"])
            if player in snapshot["bodies"]:
                raise ValueError("duplicate player in body snapshot")
            snapshot["bodies"][player] = {
                key: float(value) if key == "hp" else int(value) for key, value in fields.items()
            }
        else:
            if int(fields["rows"]) != snapshot["rows"]:
                raise ValueError("body snapshot end count differs from begin")
            if len(snapshot["bodies"]) != snapshot["rows"]:
                raise ValueError("body snapshot is missing rows")
            snapshot["end_tick"] = int(fields["tick"])
            complete = True
    if snapshot is None or not complete:
        raise ValueError("latest body snapshot is missing or incomplete")
    return snapshot


def compare(snapshots, after_tick, max_skew_ms=100, hp_tolerance=0.02):
    if len(snapshots) != 3:
        raise ValueError("exactly three client snapshots are required")
    if after_tick < 0 or max_skew_ms < 0 or not math.isfinite(hp_tolerance) or hp_tolerance < 0:
        raise ValueError("invalid comparison limits")
    pids = {sample["pid"] for sample in snapshots}
    owners = {sample["owner"] for sample in snapshots}
    revisions = {sample["revision"] for sample in snapshots}
    if len(pids) != 3 or len(owners) != 3 or min(pids) <= 0 or min(owners) <= 0:
        raise ValueError("snapshots must identify three distinct processes and owners")
    if len(revisions) != 1 or min(revisions) <= 0:
        raise ValueError("world revisions disagree")
    for sample in snapshots:
        if sample["tick"] <= after_tick or sample["end_tick"] < sample["tick"]:
            raise ValueError("snapshot is stale or has reversed timestamps")
        if sample["truncated"] or sample["rows"] != 3 or set(sample["bodies"]) != owners:
            raise ValueError("snapshot does not cover all three players")
        for player, body in sample["bodies"].items():
            if body["available"] != 1:
                raise ValueError("native body observation is unavailable")
            if body["owner"] != int(player == sample["owner"]):
                raise ValueError("body owner identity disagrees with snapshot")
            if not sample["tick"] <= body["tick"] <= sample["end_tick"]:
                raise ValueError("body observation falls outside its snapshot")
            if not math.isfinite(body["hp"]) or body["hp"] <= 0 or body["native_ko"] not in (0, 1):
                raise ValueError("invalid native body observation")
    skew = max(sample["end_tick"] for sample in snapshots) - min(
        sample["tick"] for sample in snapshots
    )
    if skew > max_skew_ms:
        raise ValueError("snapshots exceed the permitted observation skew")
    players = []
    for player in sorted(owners):
        observations = [sample["bodies"][player] for sample in snapshots]
        health = [body["hp"] for body in observations]
        knockout = [body["native_ko"] for body in observations]
        players.append(
            {
                "player": player,
                "hp": health,
                "native_ko": knockout,
                "profiles": [body["profile"] for body in observations],
                "actions": [body["action"] for body in observations],
                "agrees": max(health) - min(health) <= hp_tolerance and len(set(knockout)) == 1,
            }
        )
    return {
        "hp_and_native_ko_agree": all(player["agrees"] for player in players),
        "visual_acceptance": False,
        "observation_skew_ms": skew,
        "revision": snapshots[0]["revision"],
        "pids_in_observation_order": [sample["pid"] for sample in snapshots],
        "samples": [sample["sample"] for sample in snapshots],
        "players": players,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs=3)
    parser.add_argument("--after-tick", type=int, required=True)
    parser.add_argument("--max-skew-ms", type=int, default=100)
    parser.add_argument("--hp-tolerance", type=float, default=0.02)
    args = parser.parse_args()
    try:
        snapshots = []
        for path in args.logs:
            with path.open(encoding="utf-8", errors="replace") as stream:
                snapshots.append(latest_snapshot(stream))
        result = compare(snapshots, args.after_tick, args.max_skew_ms, args.hp_tolerance)
    except (ValueError, KeyError, OSError) as error:
        print(json.dumps({"valid_observation": False, "error": str(error)}))
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["hp_and_native_ko_agree"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
