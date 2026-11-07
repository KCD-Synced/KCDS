'use strict';

// The MCP tools of World Architect's server (server.cjs), one for each editor command
// (Client/resources/architect/ai_commands.cjs).

const point = (what) => ({
  type: 'object',
  description: `${what}: world metres; leave z out to use the ground height there`,
  properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } },
  required: ['x', 'y'],
});
const points = (what) => ({ type: 'array', items: point(what), minItems: 1 });
const object = (properties, required = []) => ({ type: 'object', properties, required });
const id = { type: 'integer', description: "a thing's id (from list, place or duplicate)" };
// The catalogue's tags and the kinds of room they furnish, as the editor reads them from model
// paths (Client/resources/architect/tags.cjs; a test keeps the two the same).
const TAG_NAMES = [
  'seating',
  'table',
  'bed',
  'storage',
  'lighting',
  'textile',
  'decoration',
  'religious',
  'kitchen',
  'food',
  'tools',
  'smithy',
  'fixture',
  'books',
  'weapons',
];
const ROOM_KINDS = [
  'church',
  'tavern',
  'smithy',
  'bedroom',
  'kitchen',
  'storeroom',
  'study',
  'hall',
];

const TOOLS = [
  {
    name: 'state',
    description:
      'The editor now: open or not, map, mode, selection, how many things and undo steps, the look ' +
      '(time, weather, wind), the camera and the terrain brush.',
    inputSchema: object({}),
  },
  {
    name: 'open',
    description:
      'Opens the editor, or comes back to it from playing as the player (local mode only).',
    inputSchema: object({}),
  },
  {
    name: 'close',
    description:
      'Play as player: closes the editor to walk the world as the player (open comes back).',
    inputSchema: object({}),
  },
  {
    name: 'switch_map',
    description:
      'Loads another map: trosecko, kutnohorsko, klaster, or a custom map on this PC ' +
      '(kcds_map_<name>; state lists them all). The work here is saved first; the editor opens ' +
      'again there in about 15 s.',
    inputSchema: object({ map: { type: 'string', description: 'a name from state.maps' } }, [
      'map',
    ]),
  },
  {
    name: 'camera',
    description: 'Moves the editor camera and/or points it at a spot; answers its position.',
    inputSchema: object({
      position: point('where the camera goes'),
      lookAt: point('what it looks at'),
    }),
  },
  {
    name: 'scale',
    description:
      'Grows (steps > 0) or shrinks (steps < 0) a thing by an id, ×1.1 a step: a plant, prop, ' +
      "collision shape, zone, or the level's own object where it stands. One undo step.",
    inputSchema: object(
      { id: { type: 'integer' }, steps: { type: 'integer', minimum: -30, maximum: 30 } },
      ['id', 'steps'],
    ),
  },
  {
    name: 'export_server',
    description:
      'Exports the work as a KCDS resource folder (architect_<map>: package.json, server.js, ' +
      "client.js, work.json) into the editor's exports folder, ready for a server's resources; " +
      'answers the folder and what is in it.',
    inputSchema: object({}),
  },
  {
    name: 'export_blender',
    description:
      'Exports the work and the ground around it for Blender (ground.obj, scene.json, ' +
      "import_kcds.py) into the editor's exports folder; answers the folder.",
    inputSchema: object({}),
  },
  {
    name: 'fly_speed',
    description: 'How fast the editor camera flies, 1 to 200 metres a second.',
    inputSchema: object({ speed: { type: 'number', minimum: 1, maximum: 200 } }, ['speed']),
  },
  {
    name: 'door',
    description:
      'Opens or closes a level door (an id from pick) in this game, as the game swings it; ' +
      'not saved.',
    inputSchema: object({ id: { type: 'integer' }, open: { type: 'boolean' } }, ['id', 'open']),
  },
  {
    name: 'properties',
    description:
      "A level entity's own properties (an id from pick, or the selection without one: a door, " +
      'a light, a spawn ...), as the level set them: [{ key, type, value }].',
    inputSchema: object({ id: { type: 'integer' } }, []),
  },
  {
    name: 'set_property',
    description:
      "Sets one of a level entity's properties in this game (a bool or a number), as Details " +
      'does; not saved in the project.',
    inputSchema: object(
      {
        id: { type: 'integer' },
        key: { type: 'string' },
        value: { type: ['boolean', 'number'] },
      },
      ['key', 'value'],
    ),
  },
  {
    name: 'bake_navmesh',
    description:
      "On a custom map: makes its navmesh again from the ground as it is now, without the author's " +
      'buildings, props and collision shapes; used from the next start.',
    inputSchema: object({}, []),
  },
  {
    name: 'walk',
    description:
      'Sends the selected preview NPC (People: place npc_man or npc_woman) walking to a point by ' +
      "the game's own walk on the navmesh; npc_state then tells walking, reached or no path.",
    inputSchema: object({ to: point('where to') }, ['to']),
  },
  {
    name: 'npc_state',
    description: "The selected preview NPC's walk: walking, reached, no path, stopped or standing.",
    inputSchema: object({}, []),
  },
  {
    name: 'gates_near',
    description:
      "The level's gates (drawbridge, portcullis, castle and town gates) within radius metres " +
      "(at most 60) of a point: their number, key, state (-1 the level's own) and state names.",
    inputSchema: object({ at: point('where'), radius: { type: 'number' } }, ['at']),
  },
  {
    name: 'gate',
    description: 'Moves a gate (its number from gates_near) into a state (its name or index).',
    inputSchema: object(
      { gate: { type: 'integer' }, state: { type: ['string', 'integer'] }, at: point('near') },
      ['gate', 'state', 'at'],
    ),
  },
  {
    name: 'look',
    description:
      'Time of day (hour 0 to 23.5 in half hours), clock (Held, ×15, ×60, ×600), weather and ' +
      'wind by their labels, and whether time and weather are frozen (unfrozen weather is the ' +
      "game's own).",
    inputSchema: object({
      hour: { type: 'number', minimum: 0, maximum: 23.5 },
      freeze_time: { type: 'boolean' },
      freeze_weather: { type: 'boolean' },
      clock: { type: 'string', enum: ['Held', '×15', '×60', '×600'] },
      weather: {
        type: 'string',
        enum: [
          'Clear',
          'Few clouds',
          'Overcast',
          'Cloudy',
          'Showers',
          'Fog and drizzle',
          'Fog',
          'Storm',
        ],
      },
      wind: { type: 'string', enum: ['Calm', 'Breeze', 'Windy', 'Gale'] },
    }),
  },
  {
    name: 'catalogue',
    description:
      'Searches the content: buildings and structures (prefab), trees and bushes (vegetation), ' +
      'props: furniture, decorations, fixtures, lamps and candle holders, tools, food, carts, ' +
      'rocks, building parts (prop), collision shapes (shape), and the Effects mode: real light ' +
      "sources (folder lights: candle, torch, fireplace, lantern, lamp ...; tune them with 'light'), " +
      'world sounds (folder sounds) and particle effects (fire, smoke, weather, effects_work, ' +
      "effects_other; tune them with 'effect'). A prop lamp or chandelier gives no light by " +
      'itself: put a light where its flames are. Answers names to place; buildings with their box and ' +
      'size (width along x, depth along y, height, metres, before any turn; a town house of ' +
      "Kuttenberg's streets has its front, gable and arcades, on its -y side), props too with " +
      'sizes: true. minWidth ... maxHeight keep only what fits.',
    inputSchema: object({
      query: { type: 'string', description: 'part of a name, e.g. "house", "oak", "box"' },
      tag: {
        type: 'string',
        enum: TAG_NAMES,
        description: 'what it is for (each item answers its tags)',
      },
      tags: { type: 'array', items: { type: 'string', enum: TAG_NAMES }, description: 'all of' },
      room: {
        type: 'string',
        enum: ROOM_KINDS,
        description: 'what furnishes that kind of room (one of its tags)',
      },
      folder: { type: 'string', description: 'one folder only, e.g. "houses", "structures"' },
      limit: { type: 'integer', minimum: 1, maximum: 500 },
      sizes: { type: 'boolean', description: "props' sizes too (asks the game, slower)" },
      minWidth: { type: 'number' },
      maxWidth: { type: 'number' },
      minDepth: { type: 'number' },
      maxDepth: { type: 'number' },
      minHeight: { type: 'number' },
      maxHeight: { type: 'number' },
    }),
  },
  {
    name: 'list',
    description:
      "The work's things with ids: prefab, vegetation, shape, level (hidden level objects). " +
      'Each says its box in its own frame (min and max, metres, before its turn): to line ' +
      'house fronts up side by side and keep things from overlapping.',
    inputSchema: object({
      kind: { type: 'string', enum: ['prefab', 'vegetation', 'prop', 'shape', 'level'] },
    }),
  },
  {
    name: 'place',
    description:
      'Puts a catalogue item at a point, turned (degrees). Answers the new thing with its id. ' +
      'snap rests a prop where it belongs instead of at the exact point: floor (its foot on the ' +
      'first floor under at), ceiling (hung by its top from the first ceiling over at: ' +
      'chandeliers, hanging lamps), wall (its back against the first wall from at toward ' +
      '`toward`, turned to face into the room: shelves, torches, pictures). Use snap indoors: a ' +
      'guessed z floats or sinks.',
    inputSchema: object(
      {
        name: { type: 'string', description: 'its full name from catalogue' },
        at: point('where (inside the room, with z, for snap ceiling or wall)'),
        turn: { type: 'number' },
        snap: { type: 'string', enum: ['floor', 'ceiling', 'wall'] },
        toward: point('snap wall: a point past the wall, the way to look for it'),
        fitGround: {
          type: 'boolean',
          description: 'level the ground under a building (default on)',
        },
      },
      ['name', 'at'],
    ),
  },
  {
    name: 'arrange',
    description:
      'Furniture laid out in one step (one undo step): name placed at each spot of a layout, or ' +
      'the things ids moved to them. row: from `from` to `to` (count evenly, both ends) or from ' +
      '`from` along direction (degrees from +x) spacing metres apart: pews, torches along a ' +
      'wall, a shelf of jars. grid: rows x columns around at, spacing ([across, along] or one), ' +
      'turned turn, an aisle of metres down its middle: pews either side of a nave, beds in a ' +
      'dormitory. around: count on a ring of radius around at, facing in (chairs at a table, ' +
      'candles around an altar) or out. turn turns all (a prop faces its -y side the way turn ' +
      'points it; a few models run along their y, the answer then says to add 90), toward faces ' +
      'each at a point (the altar, the fireplace). snap floor rests ' +
      'each on the floor indoors. Leave room to walk: 0.6 m between a table and a wall, an ' +
      'aisle of 1.2 m or more.',
    inputSchema: object(
      {
        layout: { type: 'string', enum: ['row', 'grid', 'around'] },
        name: { type: 'string', description: 'a catalogue item to place at each spot' },
        ids: { type: 'array', items: { type: 'integer' }, description: 'things to move instead' },
        count: { type: 'integer', minimum: 1, maximum: 64 },
        from: point('row: its first spot'),
        to: point('row: its last spot'),
        direction: { type: 'number', description: 'row without to: degrees from +x' },
        spacing: {
          description: 'metres between spots (grid: one number or [across, along])',
          oneOf: [{ type: 'number' }, { type: 'array', items: { type: 'number' } }],
        },
        at: point('grid, around: the middle'),
        rows: { type: 'integer', minimum: 1 },
        columns: { type: 'integer', minimum: 1 },
        aisle: { type: 'number', description: 'grid: metres free down its middle' },
        radius: { type: 'number', description: 'around: metres from at' },
        start: { type: 'number', description: 'around: degrees from +x of the first' },
        face: { type: 'string', enum: ['in', 'out'] },
        turn: { type: 'number' },
        toward: point('each faces this point'),
        snap: { type: 'string', enum: ['floor'] },
      },
      ['layout'],
    ),
  },
  {
    name: 'place_many',
    description:
      'Places up to 300 catalogue items in one call: [{ name, at, turn }]. With avoidOverlap, ' +
      'a building or prop that would stand more than a quarter in another is left out. Answers ' +
      'placed [{ id, name }] and skipped [{ name, at, reason }].',
    inputSchema: object(
      {
        items: {
          type: 'array',
          items: object(
            { name: { type: 'string' }, at: point('where'), turn: { type: 'number' } },
            ['name', 'at'],
          ),
        },
        avoidOverlap: { type: 'boolean' },
        inset: {
          type: 'number',
          description: 'metres off each side of a box in the overlap check (0; fill_row uses 2.5)',
        },
        fitGround: { type: 'boolean' },
      },
      ['items'],
    ),
  },
  {
    name: 'fill_row',
    description:
      'Lines a street front with buildings side by side, without gaps or overlaps: from `from` ' +
      'to `to` along the street line, their fronts (gable, arcades) toward `facing`, left or ' +
      "right of the walk from `from` to `to`. Kuttenberg's town houses by default, or `names`; " +
      'none deeper than `depth` metres (40). Answers as place_many.',
    inputSchema: object(
      {
        from: point('where the row starts'),
        to: point('where it ends'),
        facing: { type: 'string', enum: ['left', 'right'] },
        depth: { type: 'number' },
        names: { type: 'array', items: { type: 'string' } },
        avoidOverlap: { type: 'boolean', description: 'true by default' },
      },
      ['from', 'to', 'facing'],
    ),
  },
  {
    name: 'light',
    description:
      'A real light of yours (a point light, as Effects > Lights): a new one from a preset at a ' +
      'point (give z: a light at the ground lights the floor only), or one placed before by id. ' +
      'Sets its color ([r, g, b] 0 to 1, or Warm, Fire, Candle, White, Moon, Red, Green, Blue), ' +
      'intensity (0 to 100), radius (metres it reaches, 0.5 to 100) and shadows; one undo step. ' +
      'Candle 1.5 / 3 m, torch 4 / 7 m, fireplace 6 / 9 m, lamp 6 / 12 m to start from. ' +
      'ambient: a room light, as the game lights its own rooms: it sets the ambient light within ' +
      'its radius (no shine, no shadows). One in the middle of an opened building, radius to its ' +
      'walls, warm and dim, takes the cold sky light out; the lamps and candles then light it. ' +
      'Answers the light with its settings.',
    inputSchema: object({
      id,
      preset: {
        type: 'string',
        enum: [
          'candle',
          'torch',
          'fireplace',
          'lantern',
          'lamp',
          'moonlight',
          'red',
          'green',
          'blue',
          'room',
        ],
      },
      at: point('where a new light stands (its height too)'),
      color: { type: ['array', 'string'], items: { type: 'number' } },
      intensity: { type: 'number' },
      radius: { type: 'number' },
      shadows: { type: 'boolean' },
      ambient: { type: 'boolean' },
    }),
  },
  {
    name: 'effect',
    description:
      'A particle effect of yours (fire, smoke, dust, placed from catalogue): its size (times ' +
      'its own, 0.25 to 32) and repeat (plays again every 1, 2, 5 or 10 s; 0 runs on).',
    inputSchema: object(
      { id, size: { type: 'number' }, repeat: { type: 'integer', enum: [0, 1, 2, 5, 10] } },
      ['id'],
    ),
  },
  {
    name: 'select',
    description: 'Selects a thing (the editor shows it).',
    inputSchema: object({ id }, ['id']),
  },
  {
    name: 'move',
    description:
      'Moves and/or turns a thing of the work. A level object (from hide_level_at) is hidden and ' +
      'your copy (a prop or plant) stands at the new place; the answer is the copy.',
    inputSchema: object({ id, at: point('where to'), turn: { type: 'number' } }, ['id']),
  },
  {
    name: 'delete',
    description: 'Deletes a thing of the work.',
    inputSchema: object({ id }, ['id']),
  },
  {
    name: 'duplicate',
    description: 'Copies a thing beside itself.',
    inputSchema: object({ id }, ['id']),
  },
  {
    name: 'shape_size',
    description: "Sets a collision shape's size in metres.",
    inputSchema: object(
      { id, size: object({ x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } }) },
      ['id', 'size'],
    ),
  },
  {
    name: 'texture',
    description:
      'Makes a material of your own from a picture on this PC (a photo of a wall, a roof, a ' +
      "cobbled street): the picture as the game's texture with its mip levels and a material " +
      'that draws it, in your assets folder (objects/kcds/<folder>/<name>). PNG always; JPG ' +
      'and other kinds when ffmpeg is installed. The game loads it at once; set_material puts ' +
      'it on a prop. Answers its material name.',
    inputSchema: object(
      {
        image: { type: 'string', description: "the picture's full path on this PC" },
        name: { type: 'string', description: '1 to 32 lower-case letters, digits or _' },
        folder: {
          type: 'string',
          description: 'your folder under objects/kcds (textures by default)',
        },
        surface: {
          type: 'string',
          enum: ['stone', 'wood', 'plaster', 'metal', 'cloth', 'ground'],
          description: 'what it sounds and breaks as (stone by default)',
        },
        tile: { type: 'number', description: 'times the picture repeats on a face (1 by default)' },
        size: {
          type: 'integer',
          enum: [256, 512, 1024, 2048],
          description: 'the largest side kept, pixels (2048 by default)',
        },
      },
      ['image', 'name'],
    ),
  },
  {
    name: 'interiors_near',
    description:
      "The game's own interiors around a point: each building with rooms inside, as the level " +
      'marks them (a room has an environment probe, its box the room, and a room shape), with ' +
      'its rooms (name, middle, box in metres) and doors, nearest first. A building of the level ' +
      'with none (a church, a tower) is closed inside: open_building opens it. Details > Interior ' +
      'of a picked level building shows the same.',
    inputSchema: object(
      { at: point('the middle'), radius: { type: 'number', description: 'metres (40)' } },
      ['at'],
    ),
  },
  {
    name: 'buildings_near',
    description:
      "The level's buildings around a point (houses, churches, towers, walls: their model, " +
      "place and turn), nearest first, each with interior: the name of the game's own rooms in " +
      'it (enter_interior goes in), or closed: true when the game has none (open_building opens ' +
      'it). hidden: also the buildings the level keeps hidden in this game (its other story ' +
      "states: St James's church finished where the game shows its scaffolding), each with " +
      'hidden: true; open_building with its mesh opens one where it stands. The camera must be ' +
      'near: the game loads the level around it.',
    inputSchema: object(
      {
        at: point('the middle'),
        radius: { type: 'number', description: 'metres (30; at most 40)' },
        hidden: { type: 'boolean', description: 'also the hidden ones (false)' },
      },
      ['at'],
    ),
  },
  {
    name: 'open_building',
    description:
      'Opens a closed building of the level where it stands, in one step: the nearest building ' +
      'to at (or mesh from buildings_near) is copied without the collision boxes that fill its ' +
      "inside, with a floor put under each, the copy placed at its spot and turn, the level's " +
      'own hidden, and a room light in its largest room, as Details > Interior > Open it. One ' +
      'undo step. Answers the removed boxes, the placed pieces and the light; then ' +
      'review inside, put walls (walls in the model frame) where a doorway should stay closed, ' +
      'light and furnish it. list_only: only list its boxes.',
    inputSchema: object(
      {
        at: point('at or near the building'),
        mesh: { type: 'string', description: "the building's model (buildings_near)" },
        radius: { type: 'number' },
        thinnest: { type: 'number', description: 'boxes no thinner than this go (2.5 m)' },
        remove: { type: 'array', items: { type: 'string' }, description: 'only these boxes go' },
        floor: { type: 'boolean', description: 'a floor under each removed box (true)' },
        walls: {
          type: 'array',
          items: { type: 'object' },
          description:
            "walls in the model's frame: [{ from: [x, y], to: [x, y], base, height, thickness }]",
        },
        floors: {
          type: 'array',
          items: { type: 'object' },
          description: "more floors in the model's frame: [{ min: [x, y], max: [x, y], top }]",
        },
        materials: {
          type: 'object',
          description: 'sub-materials drawn as others, e.g. { "void": "plaster_b" }',
        },
        name: { type: 'string', description: "the copy's name (default: the model's)" },
        list_only: { type: 'boolean' },
      },
      ['at'],
    ),
  },
  {
    name: 'enter_interior',
    description:
      "Puts the editor camera inside a room of the game's own interior near a point (the nearest, " +
      'or building by its name from interiors_near; room by its index), as Details > Interior > ' +
      'Go inside. Then review or screenshot it, furnish it, light it.',
    inputSchema: object(
      {
        at: point('near the building'),
        building: { type: 'string' },
        room: { type: 'integer' },
        radius: { type: 'number' },
      },
      ['at'],
    ),
  },
  {
    name: 'entities_near',
    description:
      "The level's entities around a point, the nearest first: doors (AnimDoor), lights, " +
      'environment probes (EnvironmentLight: the ambient light of a room), area and trigger ' +
      'shapes ... class keeps one class only; properties: true reads their properties too (the ' +
      'first 16). The camera must be near: the game loads the level around it.',
    inputSchema: object(
      {
        at: point('the middle'),
        radius: { type: 'number', description: 'metres (30; at most 256)' },
        class: { type: 'string' },
        properties: { type: 'boolean' },
      },
      ['at'],
    ),
  },
  {
    name: 'make_texture',
    description:
      'Makes a material of your own from a texture drawn by code (no picture needed), as Props > ' +
      "Yours' New ... tiles do: stone_blocks, bricks, plaster, wood_planks, floor_tiles, cobbles " +
      'or roof_tiles, seamless, with a normal map from its relief so light rakes across it. ' +
      'Answers its material name for set_material or set_slot.',
    inputSchema: object(
      {
        kind: {
          type: 'string',
          enum: [
            'stone_blocks',
            'bricks',
            'plaster',
            'wood_planks',
            'floor_tiles',
            'cobbles',
            'roof_tiles',
          ],
        },
        name: { type: 'string', description: '1 to 32 lower-case letters, digits or _' },
        color: {
          type: 'array',
          items: { type: 'number' },
          description: 'the base colour [r, g, b], 0 to 1 (each kind has its own)',
        },
        size: { type: 'integer', enum: [256, 512, 1024], description: 'pixels a side (512)' },
        tile: { type: 'number', description: 'times it repeats on a face (each kind its own)' },
        count: {
          type: 'integer',
          description: 'blocks, planks, tiles or stones across the texture',
        },
        variation: { type: 'number', description: '0 to 1: how much blocks and patches differ' },
        wear: { type: 'number', description: '0 to 1: dirt in the joints and low places' },
        joints: { type: 'number', description: "a joint's width, 0 to 0.2 of a block" },
        relief: { type: 'number', description: 'how strongly the relief shows in the light (4)' },
        seed: { type: 'integer', description: 'another seed draws another texture of the kind' },
        surface: { type: 'string', enum: ['stone', 'wood', 'plaster', 'metal', 'cloth', 'ground'] },
        folder: { type: 'string', description: 'your folder under objects/kcds (textures)' },
      },
      ['kind', 'name'],
    ),
  },
  {
    name: 'slots',
    description:
      "A prop's material and its slots (sub-materials: plaster, planks, glass ...), by index and " +
      'name, what you drew each as, and which of your props share the material (an opened ' +
      "building's pieces). For your own models (an opened building) each slot says how much of " +
      'the model it draws (share of its faces). raycast names the slot a drawn face has.',
    inputSchema: object({ id }, ['id']),
  },
  {
    name: 'set_slot',
    description:
      "Draws one slot of a prop's material as another material, on every prop drawn with the " +
      'same material (all: false for this prop only), as Details > Slots does: material (yours ' +
      'from make_texture or texture, or a game material path from slots; lookSlot names its slot ' +
      'when it has slots), like (another slot of the same material), or reset (the slot as the ' +
      'model has it); tile repeats its pictures on a face. One undo step. Answers the new material.',
    inputSchema: object(
      {
        id,
        slot: { type: ['string', 'integer'], description: 'the slot: its name or index' },
        material: { type: 'string' },
        lookSlot: { type: ['string', 'integer'] },
        like: { type: ['string', 'integer'] },
        reset: { type: 'boolean' },
        tile: { type: 'number' },
        all: { type: 'boolean' },
      },
      ['id', 'slot'],
    ),
  },
  {
    name: 'open_model',
    description:
      'open_building does all of this where the building stands, in one step; open_model is the ' +
      'part by part way. Opens a closed building of the game: copies its model (mesh, a .cgf path from pick) into ' +
      'your assets as objects/kcds/opened/<name>.cgf with its LODs, without the collision boxes ' +
      'that fill its inside (the game closes a building with them), so it can be walked into. ' +
      'Without remove, every box no thinner than thinnest metres (2.5) goes; name boxes in ' +
      'remove to take only those (a dry run first: list_only). Answers the removed boxes (centre ' +
      "and size in the model's frame) and how many stay: put walls and a floor back there.",
    inputSchema: object(
      {
        mesh: { type: 'string', description: "the game model's path, e.g. from pick" },
        remove: { type: 'array', items: { type: 'string' }, description: 'box names to remove' },
        thinnest: { type: 'number' },
        name: { type: 'string', description: "the copy's name (default: the model's)" },
        list_only: { type: 'boolean', description: 'only list the boxes, write nothing' },
        materials: {
          type: 'object',
          description:
            'sub-materials drawn as others of the same material, e.g. { "void": "plaster_b" }: ' +
            'the inside the game never shows is drawn with "void" (black)',
        },
      },
      ['mesh'],
    ),
  },
  {
    name: 'import_model',
    description:
      'Makes a model of your own from a 3D file on this PC (Blender: File > Export > Wavefront ' +
      "OBJ, with its .mtl and pictures beside it): the game's model, its materials and textures " +
      'in your assets folder (objects/kcds/<folder>/<name>.cgf), loaded at once; place it by ' +
      'that name. Objects named collision... are not drawn but collide as boxes; objects named ' +
      'door... are not drawn: their places come back as doors, for real game doors. Without ' +
      "collision objects each drawn object's box collides (collision: 'objects').",
    inputSchema: object(
      {
        file: { type: 'string', description: "the .obj file's full path on this PC" },
        name: { type: 'string', description: '1 to 32 lower-case letters, digits or _' },
        folder: {
          type: 'string',
          description: 'your folder under objects/kcds (models by default)',
        },
        up: {
          type: 'string',
          enum: ['y', 'z'],
          description: "the file's up axis (Blender's OBJ: y)",
        },
        scale: { type: 'number', description: 'times the size (1 by default: the file in metres)' },
        two_sided: {
          type: 'boolean',
          description:
            'draw both sides of every face (for a model whose faces point either way, such as ' +
            "a city's survey data; false by default)",
        },
        collision: {
          type: 'string',
          enum: ['marked', 'objects', 'bounds', 'none'],
          description: 'what collides: the collision objects, each object, one box, or nothing',
        },
        size: {
          type: 'integer',
          enum: [256, 512, 1024, 2048],
          description: "the largest side kept of the model's pictures, pixels",
        },
      },
      ['file', 'name'],
    ),
  },
  {
    name: 'assets',
    description:
      'Your own models (meshes, placed by their path as props) and materials, the assets ' +
      'folder looked at again and loaded; refused names files the game cannot take.',
    inputSchema: object({}),
  },
  {
    name: 'set_material',
    description:
      'Draws a placed prop with one of your materials (assets lists them; null: its own), one ' +
      'undo step.',
    inputSchema: object(
      { id, material: { type: ['string', 'null'], description: 'a material of yours, or null' } },
      ['id'],
    ),
  },
  {
    name: 'pick',
    description:
      "Selects what stands at a point (straight down): your nearest thing, else the level's own " +
      'object there (a house, tree, fence), with an id to move, turn or hide it. With from and ' +
      'to, what that line meets first (a door in a wall, for door and properties).',
    inputSchema: object({
      at: point('where (straight down onto it)'),
      from: point('or a line from here'),
      to: point('to here: what it meets first (a door in a wall)'),
    }),
  },
  {
    name: 'hide_level_named',
    description:
      "Hides the level's own object of a model (its .cgf path) nearest a point, within radius " +
      'metres (30), in this game: also under a building of yours, where hide_level_at meets yours.',
    inputSchema: object(
      { mesh: { type: 'string' }, at: point('near'), radius: { type: 'number' } },
      ['mesh', 'at'],
    ),
  },
  {
    name: 'show_level',
    description: 'Shows a level object hidden in this game again (its id from list, kind level).',
    inputSchema: object({ id }, ['id']),
  },
  {
    name: 'hide_level_at',
    description:
      "Hides the level's own object (a house, a tree, a rock) found straight down at a point, in this game.",
    inputSchema: object({ at: point('where') }, ['at']),
  },
  {
    name: 'terrain',
    description:
      'Shapes the ground along points as one stroke: raise, lower or flatten (to the first point height).',
    inputSchema: object(
      {
        mode: { type: 'string', enum: ['raise', 'lower', 'flatten'] },
        points: points('the stroke'),
        size: { type: 'number', description: 'brush radius, metres' },
        strength: { type: 'number', description: 'metres a dab (raise and lower)' },
      },
      ['mode', 'points'],
    ),
  },
  {
    name: 'new_map',
    description:
      'Makes a new custom map from nothing on this PC: a flat square of ground (size metres a ' +
      'side, at height metres), covered with one ground material, with a navmesh so people walk ' +
      'on it, from your own game files. The game lists it at once; switch_map opens it ' +
      '(kcds_map_<name>). A name already used on this PC is refused.',
    inputSchema: object(
      {
        name: { type: 'string', description: '1 to 32 lower-case letters, digits or _' },
        size: { type: 'integer', enum: [128, 256, 512, 1024, 2048] },
        height: { type: 'number', description: 'the ground height, metres (32 by default)' },
        ground: {
          type: 'string',
          enum: [
            'grass',
            'meadow',
            'pasture',
            'trampled',
            'soil',
            'mud',
            'stony_mud',
            'sand',
            'gravel',
            'forest',
            'forest_leaves',
            'cobblestones',
            'paving',
            'cobblestones_grass',
            'river_shore',
          ],
        },
      },
      ['name'],
    ),
  },
  {
    name: 'paint',
    description:
      "Paints the ground's look along points as one stroke: the material covers the inner half of " +
      'the brush and blends into the ground around over the outer half (footsteps and dust follow ' +
      "it). A custom map takes every material; a game level's ground only the ones it has there.",
    inputSchema: object(
      {
        material: {
          type: 'string',
          enum: [
            'grass',
            'meadow',
            'pasture',
            'trampled',
            'soil',
            'mud',
            'stony_mud',
            'sand',
            'gravel',
            'forest',
            'forest_leaves',
            'cobblestones',
            'paving',
            'cobblestones_grass',
            'river_shore',
          ],
        },
        points: points('the stroke'),
        size: { type: 'number', description: 'brush radius, metres' },
      },
      ['material', 'points'],
    ),
  },
  {
    name: 'materials',
    description: 'The ground materials paint puts on.',
    inputSchema: object({}),
  },
  {
    name: 'clear_nature',
    description:
      "Clears the level's trees, bushes, plants and grass along points (brush radius `size`).",
    inputSchema: object({ points: points('the stroke'), size: { type: 'number' } }, ['points']),
  },
  {
    name: 'add_nature',
    description: 'Plants the grass the ground there grows along points (brush radius `size`).',
    inputSchema: object({ points: points('the stroke'), size: { type: 'number' } }, ['points']),
  },
  {
    name: 'undo',
    description: 'Undoes steps.',
    inputSchema: object({ count: { type: 'integer', minimum: 1, maximum: 100 } }),
  },
  {
    name: 'redo',
    description: 'Redoes steps.',
    inputSchema: object({ count: { type: 'integer', minimum: 1, maximum: 100 } }),
  },
  {
    name: 'history',
    description: 'The undo steps done and undone, by label.',
    inputSchema: object({}),
  },
  {
    name: 'ground_height',
    description: 'The terrain height under points.',
    inputSchema: object({ points: points('where') }, ['points']),
  },
  {
    name: 'raycast',
    description: 'The first thing a line meets: its position and the level model it is, if any.',
    inputSchema: object(
      {
        from: point('start'),
        to: point('end'),
        mode: { type: 'string', enum: ['anything', 'ground', 'cover'] },
      },
      ['from', 'to'],
    ),
  },
  {
    name: 'screenshot',
    description:
      'A picture of the world as the editor camera sees it (without the panels), or from a position ' +
      'looking at a point. Use it to look at what you built.',
    inputSchema: object({
      position: point('where the picture is taken from'),
      lookAt: point('what it looks at'),
      width: { type: 'integer', minimum: 320, maximum: 1600 },
    }),
  },
  {
    name: 'review',
    description:
      'Look at an area as a player would, to judge your work before you call it done: up to 5 ' +
      'pictures from inside it (from near each corner toward the middle at eye height, kept ' +
      'short of the walls, and one from above or under the ceiling), each measured (brightness, ' +
      'contrast, colour cast, flat black and textureless areas) with what that means; and what ' +
      "the editor's Validation finds there: props that float, hang from nothing or sink into " +
      'the floor, lamps that give no light, overlaps (each with the id to fix). Answers whether ' +
      'it is indoors and the ceiling height. Use it after each round of changes.',
    inputSchema: object(
      {
        at: point('the middle of the room or place'),
        radius: { type: 'number', description: 'metres around at (10; 2 to 60)' },
        views: { type: 'integer', minimum: 0, maximum: 5, description: 'pictures (5)' },
        width: { type: 'integer', minimum: 320, maximum: 1600 },
      },
      ['at'],
    ),
  },
  {
    name: 'saves',
    description: 'The saves on this PC (Documents\\KCDS\\World Architect).',
    inputSchema: object({}),
  },
  {
    name: 'save',
    description: 'Saves the work under a name (letters, digits, spaces, - and _).',
    inputSchema: object({ name: { type: 'string' } }, ['name']),
  },
  {
    name: 'load',
    description: 'Replaces the work with a save (for this map).',
    inputSchema: object({ name: { type: 'string' } }, ['name']),
  },
  {
    name: 'merge',
    description: 'Adds a save to the work (for this map).',
    inputSchema: object({ name: { type: 'string' } }, ['name']),
  },
  {
    name: 'reset_map',
    description:
      "Reset map (the Saves tab's button): everything built on this map goes (buildings, plants, " +
      'props, lights, sounds, effects, shapes, zones, terrain and paint, hidden and moved level ' +
      "objects) and the game's own map is back, the time and weather as the editor starts. The " +
      'work is saved first as "<map> before reset <date> <time>" (answered as keptAs); saves and ' +
      'your own assets stay. Needs confirm: true.',
    inputSchema: object({ confirm: { type: 'boolean' } }, ['confirm']),
  },
  {
    name: 'delete_save',
    description: 'Deletes a save.',
    inputSchema: object({ name: { type: 'string' } }, ['name']),
  },
];

module.exports = { TOOLS, TAG_NAMES, ROOM_KINDS };
