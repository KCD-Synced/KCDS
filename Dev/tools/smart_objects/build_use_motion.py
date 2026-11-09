"""Builds the root motion of the game's sit-down, lie-down and stand-up fragments for Npc.useObject.

The game's own fragments move the body while they play: a sit-down from behind a bench steps over
it, a lie-down walks onto the bed and turns. The game starts each one where it ends on the seat,
so Npc.useObject needs how far each one moves. They are measured in the game with the
measure_motion resource next to this script (see its server.js), whose server log lines this
reads:

    python Dev/tools/smart_objects/build_use_motion.py <server log>

writes Backend/scripting/runtimes/js/smart_objects/use_motion.json: "<fragment>|<tags>" ->
[right, forward, turn], the metres the fragment moves the body to its right and forward and the
degrees it turns it (counter-clockwise), in the frame the body had when the fragment started.
Height is left out: the body stays on the ground.
"""

import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "Backend/scripting/runtimes/js/smart_objects/use_motion.json"


def motion(before, after):
    """[right, forward, turn] from pose `before` to pose `after` ([x, y, z, yaw]) in before's frame."""
    dx = after[0] - before[0]
    dy = after[1] - before[1]
    yaw = before[3]
    # The body faces +y at yaw 0, so its right is +x.
    right = dx * math.cos(yaw) + dy * math.sin(yaw)
    forward = -dx * math.sin(yaw) + dy * math.cos(yaw)
    turn = math.degrees(after[3] - yaw)
    turn = (turn + 180.0) % 360.0 - 180.0
    # + 0.0 writes -0.0 as 0.0.
    return [round(right, 2) + 0.0, round(forward, 2) + 0.0, round(turn, 1) + 0.0]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    table = {}
    for line in Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines():
        marker = line.find("MEASURE {")
        if marker < 0:
            continue
        row = json.loads(line[marker + len("MEASURE ") :])
        if row["downReason"] != "finished" or row["upReason"] != "finished":
            continue
        table["|".join(row["down"])] = motion(row["start"], row["seated"])
        table["|".join(row["up"])] = motion(row["seated"], row["stood"])
    lines = [f"  {json.dumps(key)}: {json.dumps(value)}" for key, value in sorted(table.items())]
    OUT.write_text("{\n" + ",\n".join(lines) + "\n}\n", encoding="utf-8", newline="\n")
    print(f"{len(table)} fragments -> {OUT}")


if __name__ == "__main__":
    main()
