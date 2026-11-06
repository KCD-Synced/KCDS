"""A glTF 2.0 animation onto the game's character: its joints' turns as the studio's
animation keys (studio_set_animation), retargeted by joint name.

    python kcds_gltf_anim.py names <file.gltf|.glb> <request.json>
    python kcds_gltf_anim.py convert <file.gltf|.glb> <request.json>

request.json: { joints: [{ name, parent }] (the game's skeleton, parent a name or ''),
map: { <game joint>: <glTF joint> } (names the matching misses), animation (its index or name, 0
at first), rate (keys a second, 10 at first), info: { <game joint>: studio_joint_info's answer for
it } (convert only: its parent's axes, its bone and child in the body's frame [right, forward, up]) }.

`names` answers which game joints the file's joints match ({ mapped: { game: glTF }, unmappedGltf,
unmappedGame, animations: [{ index, name, length }] }); `convert` answers { animation: { keys,
blend, loop }, mapped, unmappedGltf, unmappedGame, left out } for studio_set_animation, or
{ error }.

The joints match by name, any case, without a rig's prefix (mixamorig:, Bip01, DEF-, ...); `map`
adds or changes pairs. How it retargets: each game bone is laid along the file's bone at rest (the
shortest turn from the game's standing bone to the file's rest bone, so a file resting in a T-pose
gives the game T-pose arms where it does), then turned as the file's joint turns away from its
rest (the nodes as they stand without the animation), in the world; relative to the joint the
game's parent matches, that is put about the game parent's axes (as the studio turns a joint, on
top of the body standing still), as degrees about x, then y, then z. glTF's world is the body's
frame as glTF characters face: right -x, forward +z, up +y. The hips' rise (metres) is each key's
lift. A player's animation takes at most 32 keys and 32 joints: the keys are spread over the
animation, and the joints kept are the body's before the fingers and toes, the most moving first;
joints that do not move are left out.
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_gltf  # noqa: E402

MOST_KEYS = 32
MOST_JOINTS = 32
STILL_DEGREES = 0.5
PREFIXES = (
    "mixamorig:",
    "mixamorig_",
    "mixamorig",
    "bip01_",
    "bip01 ",
    "bip01",
    "def-",
    "def_",
    "orig-",
    "armature_",
)
SMALL_PARTS = ("finger", "thumb", "index", "middle", "ring", "pinky", "toe")


class AnimationError(ValueError):
    """What the file or the request has that the importer cannot take."""


# ---- Quaternions (x, y, z, w) ----


def q_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def q_conjugate(q):
    return (-q[0], -q[1], -q[2], q[3])


def q_normalize(q):
    length = math.sqrt(sum(part * part for part in q))
    return tuple(part / length for part in q) if length > 1e-12 else (0.0, 0.0, 0.0, 1.0)


def q_slerp(a, b, share):
    dot = sum(a[k] * b[k] for k in range(4))
    if dot < 0:
        b = tuple(-part for part in b)
        dot = -dot
    if dot > 0.9995:
        return q_normalize(tuple(a[k] + (b[k] - a[k]) * share for k in range(4)))
    angle = math.acos(dot)
    first = math.sin((1 - share) * angle) / math.sin(angle)
    second = math.sin(share * angle) / math.sin(angle)
    return tuple(a[k] * first + b[k] * second for k in range(4))


def q_matrix(q):
    """Rows of the rotation matrix."""
    x, y, z, w = q
    return (
        (1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
        (2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
        (2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)),
    )


def q_rotate(q, v):
    m = q_matrix(q)
    return tuple(sum(m[r][c] * v[c] for c in range(3)) for r in range(3))


def body(v):
    """glTF's world to the body's frame [right, forward, up] (glTF characters face +z)."""
    return (-v[0], v[2], v[1])


def degrees_xyz(rows):
    """[x, y, z] degrees of a rotation R = Rz(z) Ry(y) Rx(x) (x first, about fixed axes), as the
    studio's animation turns a joint (animation.cjs quaternion)."""
    sine = max(-1.0, min(1.0, -rows[2][0]))
    y = math.asin(sine)
    if abs(sine) < 0.99999:
        x = math.atan2(rows[2][1], rows[2][2])
        z = math.atan2(rows[1][0], rows[0][0])
    else:
        # Gimbal lock: all of the turn about z.
        x = 0.0
        z = math.atan2(-rows[0][1], rows[1][1])
    return [math.degrees(x), math.degrees(y), math.degrees(z)]


# ---- Names ----


def plain(name):
    """A joint's name without a rig's prefix, case or punctuation."""
    text = name.lower()
    for prefix in PREFIXES:
        if text.startswith(prefix):
            text = text[len(prefix) :]
            break
    return "".join(c for c in text if c.isalnum())


def match(game_joints, gltf_names, given):
    """{ game joint: glTF joint } by plain name, `given` pairs first."""
    by_plain = {}
    for name in gltf_names:
        by_plain.setdefault(plain(name), name)
    mapped = {}
    for joint in game_joints:
        name = joint["name"]
        if name in given:
            if given[name] not in gltf_names:
                raise AnimationError(f"map: {given[name]} is not a joint of the file")
            mapped[name] = given[name]
        elif plain(name) in by_plain:
            mapped[name] = by_plain[plain(name)]
    return mapped


# ---- The file's skeleton and animation ----


def skeleton(document):
    """The file's joints (the first skin's, else every node), their parents and rest poses."""
    nodes = document.get("nodes", [])
    skins = document.get("skins", [])
    joints = skins[0]["joints"] if skins else list(range(len(nodes)))
    parents = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index
    names = {}
    for index in joints:
        name = nodes[index].get("name") or f"joint_{index}"
        names[index] = name
    return names, parents


def rest_trs(node):
    if "matrix" in node:
        raise AnimationError(
            "a joint given as a matrix is not read (export translation, rotation, scale)"
        )
    return (
        tuple(node.get("translation", (0.0, 0.0, 0.0))),
        q_normalize(tuple(node.get("rotation", (0.0, 0.0, 0.0, 1.0)))),
        tuple(node.get("scale", (1.0, 1.0, 1.0))),
    )


def pick_animation(document, which):
    animations = document.get("animations", [])
    if not animations:
        raise AnimationError("the file has no animation")
    if isinstance(which, str):
        for index, animation in enumerate(animations):
            if animation.get("name") == which:
                return index, animation
        raise AnimationError(f"no animation named {which}")
    if not isinstance(which, int) or not 0 <= which < len(animations):
        raise AnimationError(f"animation is 0 to {len(animations) - 1} or a name")
    return which, animations[which]


def tracks(document, buffers, animation):
    """{ node: { path: (times, values, interpolation) } }."""
    out = {}
    for channel in animation.get("channels", []):
        target = channel.get("target", {})
        if "node" not in target or target.get("path") not in ("translation", "rotation", "scale"):
            continue
        sampler = animation["samplers"][channel["sampler"]]
        times = [value[0] for value in kcds_gltf.accessor(document, buffers, sampler["input"])]
        values = kcds_gltf.accessor(document, buffers, sampler["output"])
        interpolation = sampler.get("interpolation", "LINEAR")
        if interpolation == "CUBICSPLINE":
            # In-tangent, value, out-tangent per key: the values only (a linear blend between).
            values = values[1::3]
        out.setdefault(target["node"], {})[target["path"]] = (times, values, interpolation)
    return out


def sample(track, time, rotation):
    times, values, interpolation = track
    if time <= times[0]:
        return values[0]
    if time >= times[-1]:
        return values[-1]
    after = next(index for index, at in enumerate(times) if at > time)
    before = after - 1
    if interpolation == "STEP":
        return values[before]
    share = (time - times[before]) / (times[after] - times[before])
    if rotation:
        return q_slerp(q_normalize(values[before]), q_normalize(values[after]), share)
    return tuple(
        values[before][k] + (values[after][k] - values[before][k]) * share
        for k in range(len(values[before]))
    )


def world_poses(document, node_tracks, time):
    """Per node its world turn and place at `time` (rest where it has no track; None: rest)."""
    nodes = document.get("nodes", [])
    parents = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index
    turns = {}
    places = {}

    def pose(index):
        if index in turns:
            return turns[index], places[index]
        translation, rotation, scale = rest_trs(nodes[index])
        own = node_tracks.get(index, {}) if node_tracks is not None else {}
        if "translation" in own:
            translation = sample(own["translation"], time, False)
        if "rotation" in own:
            rotation = q_normalize(sample(own["rotation"], time, True))
        if "scale" in own:
            scale = sample(own["scale"], time, False)
        if index in parents:
            parent_turn, parent_place = pose(parents[index])
            parent_scale = scales[parents[index]]
            scaled = tuple(translation[k] * parent_scale[k] for k in range(3))
            place = tuple(parent_place[k] + q_rotate(parent_turn, scaled)[k] for k in range(3))
            turn = q_normalize(q_multiply(parent_turn, rotation))
            scales[index] = tuple(parent_scale[k] * scale[k] for k in range(3))
        else:
            place = translation
            turn = rotation
            scales[index] = scale
        turns[index] = turn
        places[index] = place
        return turn, place

    scales = {}
    for index in range(len(nodes)):
        pose(index)
    return turns, places


# ---- The conversion ----


def load_request(path):
    try:
        with open(path, encoding="utf-8") as file:
            request = json.load(file)
    except (OSError, json.JSONDecodeError) as error:
        raise AnimationError(f"the request file: {error}") from error
    joints = request.get("joints")
    if not isinstance(joints, list) or not joints:
        raise AnimationError("the request has no game joints")
    return request


def names(path, request):
    document, _, _ = kcds_gltf.load(path)
    joint_names, _ = skeleton(document)
    gltf_names = list(joint_names.values())
    mapped = match(request["joints"], gltf_names, request.get("map", {}))
    animations = []
    _, buffers, _ = kcds_gltf.load(path)
    for index, animation in enumerate(document.get("animations", [])):
        length = 0.0
        for times, _, _ in [
            track
            for node in tracks(document, buffers, animation).values()
            for track in node.values()
        ]:
            length = max(length, times[-1])
        animations.append(
            {"index": index, "name": animation.get("name", ""), "length": round(length, 3)}
        )
    used = set(mapped.values())
    return {
        "mapped": mapped,
        "unmappedGltf": [name for name in gltf_names if name not in used],
        "unmappedGame": [
            joint["name"] for joint in request["joints"] if joint["name"] not in mapped
        ],
        "animations": animations,
    }


def _mapped_ancestor(game_joint, parents_by_name, mapped):
    """The nearest game ancestor that a glTF joint matches, or None (the body)."""
    at = parents_by_name.get(game_joint, "")
    while at:
        if at in mapped:
            return at
        at = parents_by_name.get(at, "")
    return None


def _axes_rows(axes):
    """Columns x, y, z of the parent's axes (body frame) as a matrix's rows."""
    x, y, z = axes["x"], axes["y"], axes["z"]
    return ((x[0], y[0], z[0]), (x[1], y[1], z[1]), (x[2], y[2], z[2]))


def _multiply3(a, b):
    return tuple(
        tuple(sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)) for r in range(3)
    )


def _transpose(a):
    return tuple(tuple(a[c][r] for c in range(3)) for r in range(3))


CHANGE = ((-1, 0, 0), (0, 0, 1), (0, 1, 0))


def _in_body(q):
    """A glTF world turn as a matrix in the body's frame: C R C^T."""
    return _multiply3(_multiply3(CHANGE, q_matrix(q)), _transpose(CHANGE))


def _arc(start, end):
    """The shortest turn (matrix) taking direction `start` onto `end`; none for a missing one."""
    u = kcds_gltf._unit(start)
    v = kcds_gltf._unit(end)
    dot = sum(u[k] * v[k] for k in range(3))
    if dot > 0.999999:
        return ((1, 0, 0), (0, 1, 0), (0, 0, 1))
    if dot < -0.999999:
        side = kcds_gltf._unit(
            kcds_gltf._cross(u, (1.0, 0.0, 0.0) if abs(u[0]) < 0.9 else (0.0, 1.0, 0.0))
        )
        return q_matrix((*side, 0.0))
    axis = kcds_gltf._cross(u, v)
    return q_matrix(q_normalize((*axis, 1.0 + dot)))


def _unwrap(degrees, before):
    """Each angle the turn equal to it nearest the key before's (no turn the long way round)."""
    if before is None:
        return degrees
    out = []
    for value, previous in zip(degrees, before):
        while value - previous > 180:
            value -= 360
        while value - previous < -180:
            value += 360
        out.append(value)
    return out


def convert(path, request):
    document, buffers, _ = kcds_gltf.load(path)
    joint_names, _ = skeleton(document)
    index_of = {name: index for index, name in joint_names.items()}
    mapped = match(request["joints"], list(joint_names.values()), request.get("map", {}))
    if not mapped:
        raise AnimationError("no joint of the file matches the game's (give `map`)")
    info = request.get("info", {})
    missing = [name for name in mapped if "axes" not in info.get(name, {})]
    if missing:
        raise AnimationError(f"no joint info (parent axes) for {', '.join(missing[:5])}")
    _, animation = pick_animation(document, request.get("animation", 0))
    node_tracks = tracks(document, buffers, animation)
    length = max(
        (track[0][-1] for node in node_tracks.values() for track in node.values()), default=0.0
    )
    if length <= 0:
        raise AnimationError("the animation has no length")
    length = min(length, 60.0)
    rate = float(request.get("rate", 10))
    count = max(2, min(MOST_KEYS, int(round(length * rate)) + 1))
    times = [round(length * at / (count - 1), 3) for at in range(count)]
    rest_turns, rest_places = world_poses(document, None, 0.0)
    parents_by_name = {joint["name"]: joint.get("parent") or "" for joint in request["joints"]}
    hips = next((name for name in mapped if plain(name) == "hips"), None)
    # Each game bone first laid along the file's bone at rest (the file's T-pose arms, say, from
    # the game's arms down): the shortest turn from the game's rest bone to the file's.
    aligned = {}
    for game, gltf in mapped.items():
        joint_info = info[game]
        child = joint_info.get("child")
        node = index_of[gltf]
        if joint_info.get("bone") and child in mapped:
            ends = rest_places[index_of[mapped[child]]]
            file_bone = body(tuple(ends[k] - rest_places[node][k] for k in range(3)))
            aligned[game] = _arc(joint_info["bone"], file_bone)
        else:
            aligned[game] = ((1, 0, 0), (0, 1, 0), (0, 0, 1))
    turns_by_joint = {name: [] for name in mapped}
    lifts = []
    for time in times:
        turns, places = world_poses(document, node_tracks, time)
        # Each game joint's turn from its rest in the body's frame: the file's (its turn from its
        # rest) after the alignment.
        deltas = {}
        for game, gltf in mapped.items():
            node = index_of[gltf]
            change = _in_body(q_multiply(turns[node], q_conjugate(rest_turns[node])))
            deltas[game] = _multiply3(change, aligned[game])
        for game in mapped:
            ancestor = _mapped_ancestor(game, parents_by_name, mapped)
            relative = (
                deltas[game]
                if ancestor is None
                else _multiply3(_transpose(deltas[ancestor]), deltas[game])
            )
            # About the game parent's axes: A^T R A.
            parent_axes = _axes_rows(info[game]["axes"])
            local = _multiply3(_multiply3(_transpose(parent_axes), relative), parent_axes)
            turns_by_joint[game].append(degrees_xyz(local))
        if hips:
            node = index_of[mapped[hips]]
            lifts.append(round(body(places[node])[2] - body(rest_places[node])[2], 3))
    # Each joint's turns made continuous; those that hardly move left out; the body before the
    # fingers and toes, the most moving first, at most 32.
    moving = []
    for game, series in turns_by_joint.items():
        before = None
        for at, degrees in enumerate(series):
            series[at] = _unwrap(degrees, before)
            before = series[at]
        reach = max(abs(part) for degrees in series for part in degrees)
        if reach >= STILL_DEGREES:
            small = any(part in game.lower() for part in SMALL_PARTS)
            moving.append((small, -reach, game))
    moving.sort()
    kept = [game for _, _, game in moving[:MOST_JOINTS]]
    left_out = [game for _, _, game in moving[MOST_JOINTS:]]
    keys = []
    for at, time in enumerate(times):
        pose = {game: [round(part, 1) for part in turns_by_joint[game][at]] for game in kept}
        key = {"time": time, "pose": pose}
        if hips and abs(lifts[at]) >= 0.005:
            key["lift"] = max(-1.0, min(0.5, lifts[at]))
        keys.append(key)
    keys[0]["time"] = 0
    used = set(mapped.values())
    return {
        "animation": {"keys": keys, "blend": "linear", "loop": True},
        "mapped": mapped,
        "unmappedGltf": [name for name in joint_names.values() if name not in used],
        "unmappedGame": [
            joint["name"] for joint in request["joints"] if joint["name"] not in mapped
        ],
        "leftOut": left_out,
    }


def main(argv):
    if len(argv) != 3 or argv[0] not in ("names", "convert"):
        print("usage: python kcds_gltf_anim.py names|convert <file.gltf|.glb> <request.json>")
        return 2
    try:
        request = load_request(argv[2])
        answer = names(argv[1], request) if argv[0] == "names" else convert(argv[1], request)
    except (AnimationError, kcds_gltf.GltfError, OSError, KeyError, IndexError, TypeError) as error:
        print(json.dumps({"error": str(error) or type(error).__name__}))
        return 1
    print(json.dumps(answer))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
