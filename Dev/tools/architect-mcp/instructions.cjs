'use strict';

// What World Architect's MCP server tells an AI assistant when it connects (server.cjs,
// initialize): the basics, then how to make an interior look like one of the game's own, step by
// step, with the tools that do each step.

const INSTRUCTIONS = [
  'World Architect: an in-game world editor for KCD2-Synced (KCDS), run in local mode. Positions',
  'are world metres (x east, y north, z up); turns are degrees. Leave z out to use the ground',
  "height. Call state first, catalogue to find content names, list for the work's ids. Every",
  'change is an undo step; undo takes back a whole tool call.',
  '',
  'The game loads walls, floors and their collision around the camera only: move the camera',
  'near (camera, or enter_interior) before you place, snap or raycast indoors.',
  '',
  'Making an interior that looks like the game:',
  '1. Find it. buildings_near lists the buildings at a spot: interior names the game rooms in',
  '   it, closed: true means none. interiors_near lists the rooms with their boxes and doors.',
  '2. Get in. A building with rooms: enter_interior. A closed one: open_building (its copy',
  '   placed without the boxes that fill it, a floor under each, a room light inside, one undo',
  '   step), then camera inside it.',
  "3. Look before you change anything: review at the room's middle (with z). Read every",
  '   picture as a player standing there would, and the measured notes under it.',
  '4. Surfaces: slots of a wall piece lists what each slot draws; make_texture (stone_blocks,',
  '   plaster, wood_planks, floor_tiles, bricks, cobbles, roof_tiles) and set_slot give the',
  '   walls, floor and ceiling a real material. No large black, grey or stretched faces.',
  '5. Light it as the game does: one room light per room (light preset room: ambient, takes the',
  '   cold sky light out), then a real light at every flame: candles, torches, chandeliers,',
  '   fireplaces (catalogue folder lights; a lamp prop gives no light by itself). Warm colours,',
  '   shadows on the main lights.',
  '6. Furnish it for its use: catalogue room: church, tavern, smithy, bedroom, kitchen,',
  '   storeroom, study or hall lists what belongs (each item has tags). arrange lays out rows',
  '   (pews, benches along a wall), grids with an aisle and rings (chairs around a table,',
  '   candles around an altar); place with snap floor, wall or ceiling rests a single thing.',
  '   Keep doors and paths free (aisles 1.2 m or more), sizes true to a person (door 2 m,',
  '   table 0.75 m, bench 0.45 m), groups not even grids, and enough small things (cups,',
  '   books, cloth, candles) that it reads as lived in.',
  '7. review again from the same spot and compare; fix every warning it lists before you call',
  '   the room done.',
].join('\n');

module.exports = { INSTRUCTIONS };
