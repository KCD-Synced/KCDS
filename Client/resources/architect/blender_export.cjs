'use strict';

// World Architect's Export for Blender: the work
// and the ground around it as files Blender opens, written by the game client into
// Documents\KCDS\World Architect\exports\<name>\ (architect/saves.hpp, localArchitectExport):
//
//   ground.obj      the ground as a mesh of the heights the game has (metres, Z up, X east, Y
//                   north; the area's south-west corner at its origin)
//   scene.json      every thing of the work: kind, the game's model path, its name, where it
//                   stands (from the same corner), its turn as a quaternion, its scale and its box;
//                   the zones as a server's Area.create takes them
//   import_kcds.py  run in Blender (Scripting, Run Script): the ground as a mesh and a box of the
//                   right size and turn for every thing, named after it, its model path kept as
//                   the custom property kcds_model, in a collection "KCDS <name>"
//
// Models made in Blender come back into the game as .cgf files in the assets folder
// (user_assets.hpp): the Architect lists them in Props, Yours.

// The most ground points a side (the ground's mesh), and the area's side, metres.
const GRID_MOST = 97;
const SIDE_LEAST = 32;
// A wide work is exported whole, its ground coarser (at most GRID_MOST points a side).
const SIDE_MOST = 1024;
const AROUND_WORK = 20;
const AROUND_CAMERA = 64;

const IMPORT_SCRIPT = `# KCDS World Architect: builds the export in this folder in Blender.
# Blender: Scripting tab, Open this file, Run Script (Blender 3.6 or newer).
import json
import os

import bpy

FOLDER = os.path.dirname(os.path.abspath(__file__))


def ground(collection):
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(
        filepath=os.path.join(FOLDER, "ground.obj"), forward_axis="Y", up_axis="Z"
    )
    for made in set(bpy.data.objects) - before:
        for owner in list(made.users_collection):
            owner.objects.unlink(made)
        collection.objects.link(made)
        made.name = "ground"


def thing_box(collection, thing):
    low = thing["box"]["min"]
    high = thing["box"]["max"]
    mesh = bpy.data.meshes.new(thing["label"])
    corners = [
        (x, y, z)
        for x in (low[0], high[0])
        for y in (low[1], high[1])
        for z in (low[2], high[2])
    ]
    faces = [
        (0, 1, 3, 2),
        (4, 6, 7, 5),
        (0, 4, 5, 1),
        (2, 3, 7, 6),
        (0, 2, 6, 4),
        (1, 5, 7, 3),
    ]
    mesh.from_pydata(corners, [], faces)
    made = bpy.data.objects.new(thing["label"], mesh)
    made.location = (thing["at"]["x"], thing["at"]["y"], thing["at"]["z"])
    made.rotation_mode = "QUATERNION"
    turn = thing["rotation"]
    made.rotation_quaternion = (turn["w"], turn["x"], turn["y"], turn["z"])
    made["kcds_model"] = thing["model"]
    made["kcds_kind"] = thing["kind"]
    collection.objects.link(made)


def main():
    with open(os.path.join(FOLDER, "scene.json"), encoding="utf-8") as file:
        scene = json.load(file)
    collection = bpy.data.collections.new("KCDS " + scene["name"])
    bpy.context.scene.collection.children.link(collection)
    ground(collection)
    for thing in scene["things"]:
        thing_box(collection, thing)


main()
`;

function round(value) {
  return Math.round(value * 1000) / 1000;
}

// The area to export: around the work, else around the camera; { x, y, side }, its south-west
// corner and its side in metres.
function areaOf(points, camera) {
  if (points.length === 0) {
    const at = camera ?? { x: 0, y: 0 };
    return { x: at.x - AROUND_CAMERA, y: at.y - AROUND_CAMERA, side: AROUND_CAMERA * 2 };
  }
  const xs = points.map((point) => point.x);
  const ys = points.map((point) => point.y);
  const low = { x: Math.min(...xs) - AROUND_WORK, y: Math.min(...ys) - AROUND_WORK };
  const wide = Math.max(
    Math.max(...xs) + AROUND_WORK - low.x,
    Math.max(...ys) + AROUND_WORK - low.y,
  );
  const side = Math.min(SIDE_MOST, Math.max(SIDE_LEAST, wide));
  return { x: low.x, y: low.y, side };
}

// The ground's mesh as OBJ text: a grid of `count` x `count` heights (null: the lowest known).
function groundObj(area, count, heights) {
  const step = area.side / (count - 1);
  const known = heights.filter((height) => height !== null);
  const floor = known.length > 0 ? Math.min(...known) : 0;
  const lines = ['# KCDS World Architect: the ground (metres, Z up)', 'o ground'];
  for (let row = 0; row < count; row += 1) {
    for (let column = 0; column < count; column += 1) {
      const height = heights[row * count + column] ?? floor;
      lines.push(`v ${round(column * step)} ${round(row * step)} ${round(height)}`);
    }
  }
  for (let row = 0; row < count - 1; row += 1) {
    for (let column = 0; column < count - 1; column += 1) {
      const a = row * count + column + 1;
      lines.push(`f ${a} ${a + 1} ${a + 1 + count} ${a + count}`);
    }
  }
  return `${lines.join('\n')}\n`;
}

module.exports = { areaOf, groundObj, round, GRID_MOST, IMPORT_SCRIPT };
