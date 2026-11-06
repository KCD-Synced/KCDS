'use strict';

// The editors' script-step tools of the one MCP server for both editors (server.cjs): the
// Asset/Animation Studio's tools and the editor_*
// tools of both editors. server.cjs lists them beside World Architect's own commands
// (tools.cjs) and hands a call of one of them to handle() here. They reach the open editor of a
// KCDS game in its local mode through the game client's named pipe on this machine,
// \\.\pipe\kcds-editor-<process id> (Client/runtime/architect/editor_pipe.hpp): no network, no
// server. Each tool is one step of the open editor (the editors' script() steps); the editor's
// answer is the tool's result.
//
// Several KCDS editors open: KCDS_EDITOR_PID names the game's process; otherwise the first pipe.

const { execFile } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

const PROTOCOL_VERSION = '2024-11-05';
const SERVER = Object.freeze({ name: 'kcds-architect', version: '0.1.0' });
const STEP_TIMEOUT_MS = 10000;
const LONG_STEP_TIMEOUT_MS = 40000;
// The longest step line sent whole (the bridge's own limit is 4096 bytes; a character of a
// step's JSON may take more than one).
const STEP_PART_CHARS = 1800;

// The game's surface types a made mesh may have (Dev/tools/assets/kcds_studio/studio_shapes.py
// SURFACES, of the game's Libs/MaterialEffects/SurfaceTypes.xml).
const SURFACES = Object.freeze([
  'mat_wood',
  'mat_wood_soft',
  'mat_metal',
  'mat_rock',
  'mat_plaster',
  'mat_ceramics',
  'mat_glass',
  'mat_fabric',
  'mat_leather',
  'mat_sack',
  'mat_thatch',
  'mat_wicker',
  'mat_soil',
  'mat_default',
]);
const SHAPES_SCRIPT = path.join(__dirname, '..', 'assets', 'kcds_studio', 'studio_shapes.py');
const GLTF_SCRIPT = path.join(__dirname, '..', 'assets', 'gltf', 'kcds_gltf.py');
const GLTF_SKIN_SCRIPT = path.join(__dirname, '..', 'assets', 'gltf', 'kcds_gltf_skin.py');
const FBX_SCRIPT = path.join(__dirname, '..', 'assets', 'fbx', 'kcds_fbx.py');
const GLTF_ANIMATION_SCRIPT = path.join(__dirname, '..', 'assets', 'gltf', 'kcds_gltf_anim.py');

// The tools: their MCP description and the editor step each makes.
const TOOLS = Object.freeze([
  {
    name: 'studio_state',
    description:
      'The Asset/Animation Studio as it is now: whether it is open, its mode, the dock’s tab and ' +
      'folder, what is selected, the prop (a rope, chain or whip: segments, length, swing test, ' +
      'where it is held, a whip’s grip, its physics with collide and the posed shape), the mesh ' +
      'and where it stands, the chosen joint, the animation (keys, length, playhead time, ' +
      'playing, loop, how many prop keys), the game clip playing (path, from, to, loop, in place ' +
      'and now: its time 0 to 1 and length in seconds) and the status message.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'state',
  },
  {
    name: 'studio_make_prop',
    description:
      'Makes a prop in the character’s right hand: "rope" (the game’s rope physics) or "whip" ' +
      '(the game’s whip handle with a physics lash). It takes the place of the prop there was.',
    inputSchema: {
      type: 'object',
      properties: { kind: { type: 'string', enum: ['rope', 'whip'] } },
      required: ['kind'],
      additionalProperties: false,
    },
    step: (args) => `make ${args.kind}`,
  },
  {
    name: 'studio_change_prop',
    description:
      'Changes the prop by one step: "segments" by 2 (2 to 64) or "length" by 0.1 m ' +
      '(0.2 to 20 m), up (+1) or down (-1).',
    inputSchema: {
      type: 'object',
      properties: {
        field: { type: 'string', enum: ['segments', 'length'] },
        direction: { type: 'integer', enum: [1, -1] },
      },
      required: ['field', 'direction'],
      additionalProperties: false,
    },
    step: (args) => `set ${args.field} ${args.direction}`,
  },
  {
    name: 'studio_swing_test',
    description:
      'Turns the swing test on or off: the prop’s start goes round a circle fast, like a whip.',
    inputSchema: {
      type: 'object',
      properties: { on: { type: 'boolean' } },
      required: ['on'],
      additionalProperties: false,
    },
    step: (args) => `sweep ${args.on ? 'on' : 'off'}`,
  },
  {
    name: 'studio_record_keys',
    description:
      'Records the prop’s simulation into keys: its points 30 times a second for 1 to 10 seconds ' +
      '(move it meanwhile, for example with studio_swing_test). The keys stay with the prop; ' +
      'studio_state shows them once the recording is over.',
    inputSchema: {
      type: 'object',
      properties: { seconds: { type: 'integer', minimum: 1, maximum: 10 } },
      required: ['seconds'],
      additionalProperties: false,
    },
    step: (args) => `record ${args.seconds}`,
  },
  {
    name: 'studio_get_keys',
    description:
      'The recorded keys (studio_record_keys) a part at a time, to measure the motion: from key ' +
      'first, at most 30 (count). Answers { rate (keys a second), total, first, frames }, each ' +
      'frame the prop’s points in world metres, its start (the hand or the handle’s tip) first, ' +
      '[x, y, z, x, y, z, ...]; null without keys.',
    inputSchema: {
      type: 'object',
      properties: {
        first: { type: 'integer', minimum: 0 },
        count: { type: 'integer', minimum: 1, maximum: 30 },
      },
      additionalProperties: false,
    },
    step: (args) => `keyframes ${args.first ?? 0} ${args.count ?? 30}`,
  },
  {
    name: 'studio_play_keys',
    description:
      'Plays the prop’s recorded keys in a loop instead of its simulation (on), or lets it ' +
      'simulate again (off).',
    inputSchema: {
      type: 'object',
      properties: { on: { type: 'boolean' } },
      required: ['on'],
      additionalProperties: false,
    },
    step: (args) => `playkeys ${args.on ? 'on' : 'off'}`,
  },
  {
    name: 'studio_remove_prop',
    description: 'Takes the prop away.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'remove',
  },
  {
    name: 'studio_export',
    description:
      'Exports the studio’s prop as a server resource (kcds_studio_<kind>) that gives every ' +
      'player who joins that prop: written to %LOCALAPPDATA%\\KCDS\\studio\\exports\\<name>.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'export',
  },
  {
    name: 'studio_bones',
    description:
      'Shows or hides the character’s skeleton (Bones mode): every joint drawn over the body. ' +
      'Returns the mode and the number of joints.',
    inputSchema: {
      type: 'object',
      properties: { on: { type: 'boolean' } },
      required: ['on'],
      additionalProperties: false,
    },
    step: (args) => `bones ${args.on ? 'on' : 'off'}`,
  },
  {
    name: 'studio_joints',
    description:
      'The character’s joints in the skeleton’s order: { id, name, parent } (parent -1 = root). ' +
      'Bones mode must have been shown once.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'joints',
  },
  {
    name: 'studio_choose_joint',
    description: 'Chooses a joint by id (Bones mode): it is drawn highlighted with its name.',
    inputSchema: {
      type: 'object',
      properties: { id: { type: 'integer', minimum: 0 } },
      required: ['id'],
      additionalProperties: false,
    },
    step: (args) => `joint ${args.id}`,
  },
  {
    name: 'studio_sky',
    description:
      'The studio’s sky: hour (0 to 23, the clock stands still there; 12 at first), preset ' +
      '(the game’s weather: "cloudless_sunny", "overcast_rain", "storm") and rain (0 to 1). ' +
      'Without arguments it answers the sky now.',
    inputSchema: {
      type: 'object',
      properties: {
        hour: { type: 'integer', minimum: 0, maximum: 23 },
        preset: { type: 'string', enum: ['cloudless_sunny', 'overcast_rain', 'storm'] },
        rain: { type: 'number', minimum: 0, maximum: 1 },
      },
      additionalProperties: false,
    },
    step: (args) => (Object.keys(args).length > 0 ? `sky ${JSON.stringify(args)}` : 'sky'),
  },
  {
    name: 'studio_place',
    description:
      'Where the studio’s things stand (each change one undo step). prop: where the rope or ' +
      'whip is held: joint "RightHand" (at first), "LeftHand", or "" for free in the scene at ' +
      '`at` [x, y, z] metres (the character stands at 32, 32 on the floor at z 32) turned ' +
      '`turn` [x, y, z] degrees about the world’s x, then y, then z. mesh: where your mesh ' +
      'stands: at, turn and scale (0.05 to 20), or held: joint "RightHand" or "LeftHand" (in ' +
      'the palm) or any joint’s name of the character ("Head", "Spine2"; studio_find_joints), ' +
      'then at and turn are its place and turn in the joint (at first the joint’s own) and it ' +
      'goes with the joint as the character moves; joint "" sets it free beside the character. ' +
      'end: a rope’s end pinned to a world point ' +
      '({ pinned: true, at }) or free. Without arguments it answers where they stand.',
    inputSchema: {
      type: 'object',
      properties: {
        prop: {
          type: 'object',
          properties: {
            joint: { type: 'string', enum: ['RightHand', 'LeftHand', ''] },
            at: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
            turn: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
          },
          additionalProperties: false,
        },
        mesh: {
          type: 'object',
          properties: {
            at: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
            turn: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
            scale: { type: 'number', minimum: 0.05, maximum: 20 },
            joint: { type: 'string', pattern: '^[A-Za-z0-9_]{0,64}$' },
          },
          additionalProperties: false,
        },
        end: {
          type: 'object',
          properties: {
            pinned: { type: 'boolean' },
            at: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
          },
          additionalProperties: false,
        },
      },
      additionalProperties: false,
    },
    step: (args) => (Object.keys(args).length > 0 ? `place ${JSON.stringify(args)}` : 'place'),
  },
  {
    name: 'studio_texture',
    description:
      'Your textures on your mesh (one undo step). Pictures (.png, .jpg, or a .dds) in ' +
      '%LOCALAPPDATA%\\KCDS\\studio\\textures are made the game’s textures by the game client ' +
      '(at most 2048 pixels a side). name: the file’s name without its extension ("" gives the ' +
      'mesh its own material back); tiling: how many times it repeats (0.125 to 64, 1 at first). ' +
      'A mesh must be shown (Static props). Without arguments it asks for the list of textures ' +
      'and answers the ones known so far.',
    inputSchema: {
      type: 'object',
      properties: {
        name: { type: 'string', pattern: '^[A-Za-z0-9_-]{0,48}$' },
        tiling: { type: 'number', minimum: 0.125, maximum: 64 },
      },
      additionalProperties: false,
    },
    step: (args) =>
      args.name === undefined
        ? 'textures'
        : `texture ${args.name}${args.tiling ? ` ${args.tiling}` : ''}`.trim(),
  },
  {
    name: 'studio_make_mesh',
    description:
      'Makes a simple prop as one of your meshes (the game’s own model files <name>.cgf and ' +
      '.mtl in %LOCALAPPDATA%\\KCDS\\studio\\meshes, no Blender needed) and shows it beside the ' +
      'character; made again under the same name, the one shown loads anew where it stands. ' +
      'shape: "box" (size [width x, depth y, height z] metres, 0.5 each at first), "cylinder" ' +
      '(radius 0.25, height 0.5, sides 24), "sphere" (radius 0.25, rings 12, sides 24) or ' +
      '"plane" (size [width, depth], 1 each, facing up). Each stands on the ground about its ' +
      'middle. colour [r, g, b] 0 to 1 (0.8 grey at first); surface: how it sounds and what it ' +
      'does when hit, one of the game’s surface types (mat_wood at first). Put a texture on it ' +
      'with studio_texture, move and size it with studio_place mesh. Answers { name, shape, ' +
      'vertices, triangles, files } and the editor’s answer.',
    inputSchema: {
      type: 'object',
      properties: {
        name: { type: 'string', pattern: '^[A-Za-z0-9_-]{1,48}$' },
        shape: { type: 'string', enum: ['box', 'cylinder', 'sphere', 'plane'] },
        size: {
          type: 'array',
          items: { type: 'number', minimum: 0.01, maximum: 50 },
          minItems: 2,
          maxItems: 3,
        },
        radius: { type: 'number', minimum: 0.01, maximum: 50 },
        height: { type: 'number', minimum: 0.01, maximum: 50 },
        sides: { type: 'integer', minimum: 3, maximum: 128 },
        rings: { type: 'integer', minimum: 2, maximum: 64 },
        colour: {
          type: 'array',
          items: { type: 'number', minimum: 0, maximum: 1 },
          minItems: 3,
          maxItems: 3,
        },
        surface: { type: 'string', enum: SURFACES },
      },
      required: ['name', 'shape'],
      additionalProperties: false,
    },
    run: (args, link) => makeMesh(args, link),
  },
  {
    name: 'studio_game_files',
    description:
      'Searches the game’s own files: kind "objects" (every object model, Objects/....cgf, about ' +
      '11,000) or "clips" (every animation clip, Animations/....caf, about 3,000). query: words ' +
      'that must all be in the path, any case ("whip", "horse saddle", "sword long"; "" for all). ' +
      'Answers { page, pages, total, paths } with 50 paths a page (page from 0). The first search ' +
      'reads the game’s packs (a few seconds). Show an object with studio_game_prop, play a clip ' +
      'with studio_game_clip.',
    inputSchema: {
      type: 'object',
      properties: {
        kind: { type: 'string', enum: ['objects', 'clips'] },
        query: { type: 'string', maxLength: 40 },
        page: { type: 'integer', minimum: 0 },
      },
      required: ['kind'],
      additionalProperties: false,
    },
    step: (args) => `gamefiles ${args.kind} ${args.page ?? 0} ${args.query ?? ''}`.trim(),
  },
  {
    name: 'studio_game_prop',
    description:
      'Shows one of the game’s own objects beside the character in place of the mesh (one undo ' +
      'step): path as studio_game_files answers it (Objects/....cgf); "" takes it away. It is ' +
      'placed, turned and sized as a mesh is (studio_place mesh), and an export gives every ' +
      'player it beside them by its game path.',
    inputSchema: {
      type: 'object',
      properties: {
        path: { type: 'string', pattern: '^$|^[Oo]bjects/[^:]+\\.[Cc][Gg][Ff]$', maxLength: 259 },
      },
      required: ['path'],
      additionalProperties: false,
    },
    step: (args) => `mesh ${args.path}`.trim(),
  },
  {
    name: 'studio_game_clip',
    description:
      'Plays one of the game’s own animation clips on the studio’s character (its real motion ' +
      'capture, e.g. a whipping or a sword attack): path as studio_game_files answers it ' +
      '(Animations/....caf); "" stops it. loop: again and again (true at first) or once. from, ' +
      'to: only that part of the clip plays (shares of its length, 0 to 1; a cutscene’s strikes ' +
      'without its walking in). in_place (true at first): the character stays where it stood, ' +
      'the clip’s walking and turning do not move it. The same path again changes only the ' +
      'part, loop and place while it plays on. top_body: only the upper body plays it, the legs ' +
      'keep the body’s own stance or walk. path "state" answers where it is: its time (0 to 1) ' +
      'and its whole length in seconds. path "bake" makes the part playing the studio’s own ' +
      'animation (Bake to keys: the whole body, legs included, its best loop, in place and ' +
      'facing forward; state.lastBake says what it made). A prop in the hand follows the hand, ' +
      'so a whip’s lash swings with the clip.',
    inputSchema: {
      type: 'object',
      properties: {
        path: {
          type: 'string',
          pattern: '^$|^state$|^bake$|^[Aa]nimations/[^:]+\\.[Cc][Aa][Ff]$',
          maxLength: 259,
        },
        loop: { type: 'boolean' },
        from: { type: 'number', minimum: 0, maximum: 1 },
        to: { type: 'number', minimum: 0, maximum: 1 },
        in_place: { type: 'boolean' },
        top_body: { type: 'boolean' },
      },
      required: ['path'],
      additionalProperties: false,
    },
    step: (args) => {
      if (args.path === 'state' || args.path === '' || args.path === 'bake') {
        return `gameclip ${args.path}`.trim();
      }
      const options = {
        loop: args.loop !== false,
        ...(args.from !== undefined ? { from: args.from } : {}),
        ...(args.to !== undefined ? { to: args.to } : {}),
        inPlace: args.in_place !== false,
        ...(args.top_body !== undefined ? { topBody: args.top_body } : {}),
      };
      return `gameclip ${args.path} ${JSON.stringify(options)}`;
    },
  },
  {
    name: 'studio_lash_keys',
    description:
      'Lash to keys: the whip’s lash recorded along the animation (it plays from its start while ' +
      'the physics moves the lash, one round, up to 10 s), then each key of the animation keeps ' +
      'the lash’s shape at its time (a key’s lash: 12 points from the handle’s tip in its frame, ' +
      '[along, right, up] metres): played here and on players the lash takes exactly those ' +
      'shapes, key to key, no physics. studio_get_animation shows each key’s lash; ' +
      'studio_set_animation takes it, so the shapes can be changed by hand too.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'lashkeys',
  },
  {
    name: 'studio_lash_motion',
    description:
      'Lash from motion (one undo step): the animation plays one round while the game records ' +
      'where the whip’s handle is, then the lash is made on that path and kept as the ' +
      'animation’s lash track (40 frames a second, no physics on players). mode "designed" (the ' +
      'default): the throw is found in the hand’s path (its fastest moment, start, stop, way) ' +
      'and a turn runs along the lash from the hand to the tip, each piece swinging over to the ' +
      'throw in turn (only the pieces in the turn move against each other, so it rolls and ' +
      'never snaps), straight along the throw at the crack; otherwise the lash trails the hand ' +
      'like a drawn rope and droops, so it cannot whirl round the hand or ball up. design sets ' +
      'its numbers: turnSeconds (each piece’s swing, the loop’s size, 0.1), rollAfter (how long ' +
      'after the hand stops the turn reaches the tip, 0.1), rollPower (1 even, more: faster ' +
      'toward the tip, 1.6), fallSpeed (the tip’s sinking, m/s, 4), rootFallShare (the handle ' +
      'end’s share of it, 0.15), rootKeep (the first piece keeping the handle’s line, 0.15), ' +
      'throwShare (the share of the hand’s top speed that starts and ends the throw, 0.35), ' +
      'stopSeconds (how soon a throw stops, 0.2); lastLash.design says those used. ' +
      'lastLash.throw says the throw found (the fastest move ' +
      'that stops hard; throw: { at } sets its fastest moment instead). The hand’s path is ' +
      'recorded for two rounds of a loop and fitted as one smooth loop, so the game’s uneven ' +
      'frames do not make the hand seem to jump. mode "simulated": ' +
      'the lash is simulated on that path away from the game ' +
      '(24 pieces of exactly its length, stiff at the handle and loose at the tip, lighter toward ' +
      'the tip so the wave from the hand speeds up into a crack, gravity, the air, the floor; a ' +
      'loop goes round 4 times first and its end is blended into its start) and kept as the ' +
      'animation’s lash track: the shape at its own times, 40 frames a second, played the same ' +
      'on every player with no physics (it replaces the keys’ lash shapes). A crack is chaotic: ' +
      'without stiffRoot, stiffTip, air or inner given it tries twelve (inner 0.015 and 0.02, ' +
      'stiffTip 0.05, 0.08 and 0.12, air 0.03 and 0.06) and keeps the one that measures best ' +
      '(fewest problems, then least coiled, straightest reach, least fold). Options change how ' +
      'it moves: gravity (m/s², 9.81), damping (0.3), air (drag of fast pieces, 0.03), inner ' +
      '(the leather’s own friction: each step’s share of a point’s speed pulled toward its ' +
      'neighbours’, so a wave does not ring up and down the lash, 0.02), stiffRoot and ' +
      'stiffTip (the pull toward straight each step, 0 to 1: 0.9 and 0.08), taper (how much ' +
      'lighter the tip is, 0 to 0.95: 0.8), friction (on the floor, 4), rate (frames a second, ' +
      '40). Then studio_wait for "lash": lastLash has the track, the tip’s top speed (m/s, when), ' +
      'the settings chosen and each tried, the milliseconds it took and how the lash plays (as ' +
      'studio_lash_check).',
    inputSchema: {
      type: 'object',
      properties: {
        gravity: { type: 'number', minimum: 0, maximum: 30 },
        damping: { type: 'number', minimum: 0, maximum: 5 },
        air: { type: 'number', minimum: 0, maximum: 1 },
        stiffRoot: { type: 'number', minimum: 0, maximum: 1 },
        stiffTip: { type: 'number', minimum: 0, maximum: 1 },
        mode: { type: 'string', enum: ['designed', 'simulated'] },
        // Designed: how it moves (Client/resources/studio/lash_design.cjs DESIGN).
        design: {
          type: 'object',
          properties: {
            turnSeconds: { type: 'number', minimum: 0.02, maximum: 0.5 },
            rollAfter: { type: 'number', minimum: 0, maximum: 0.6 },
            rollPower: { type: 'number', minimum: 0.5, maximum: 4 },
            fallSpeed: { type: 'number', minimum: 0, maximum: 20 },
            rootFallShare: { type: 'number', minimum: 0, maximum: 1 },
            rootKeep: { type: 'number', minimum: 0, maximum: 0.9 },
            throwShare: { type: 'number', minimum: 0.05, maximum: 0.9 },
            stopSeconds: { type: 'number', minimum: 0.02, maximum: 1 },
          },
          additionalProperties: false,
        },
        // Designed: when the throw is (its fastest moment, seconds), instead of finding it.
        throw: {
          type: 'object',
          properties: { at: { type: 'number', minimum: 0 } },
          required: ['at'],
          additionalProperties: false,
        },
        taper: { type: 'number', minimum: 0, maximum: 0.95 },
        inner: { type: 'number', minimum: 0, maximum: 0.5 },
        airTip: { type: 'number', minimum: 1, maximum: 50 },
        friction: { type: 'number', minimum: 0, maximum: 50 },
        rate: { type: 'number', minimum: 10, maximum: 60 },
      },
      additionalProperties: false,
    },
    step: (args) =>
      Object.keys(args).length > 0 ? `lashmotion ${JSON.stringify(args)}` : 'lashmotion',
  },
  {
    name: 'studio_lash_check',
    description:
      'How the whip’s lash plays, measured (null when nothing shapes it): what shapes it (a lash ' +
      'track or the keys), its length all along as [shortest, longest] metres (a lash that ' +
      'shrinks between shapes coils into the hand), its sharpest fold (degrees, when, which ' +
      'piece: over 110° is a kink), the crack (when the tip is fastest and how sharply its last ' +
      'two pieces turn over then, a whip’s own, not a kink), whether it strikes (reach: how ' +
      'straight it rolls out at most, under 0.95 no strike, and coiled: the share of the round ' +
      'it is balled up near the hand, over 0.15 it dangles), snaps (frames in which the whole ' +
      'lash turns over 30° on average at once: it teleports into its next shape instead of ' +
      'rolling), single frames that jump (when, ' +
      'metres), the loop’s seam ' +
      '(metres from its end to its start), its fastest point against the handle (m/s), and the ' +
      'problems in words. studio_check_animation lists these problems too.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'lashcheck',
  },
  {
    name: 'editor_ui',
    description:
      'Hides the whole editor or shows it again: hidden, only the scene is drawn (no panels or ' +
      'their texts, no gizmo, no grid, no bones), to watch or record the animation; shown, all ' +
      'of it comes back as it was. Presses still reach the editor while it is hidden.',
    inputSchema: {
      type: 'object',
      properties: { hidden: { type: 'boolean' } },
      required: ['hidden'],
      additionalProperties: false,
    },
    step: (args) => (args.hidden ? 'ui hide' : 'ui show'),
  },
  {
    name: 'studio_lash_pose',
    description:
      'The whip’s lash posed by hand, frame by frame (one undo step each): lash keys at their own ' +
      'times, each the lash’s whole shape (12 pieces’ ways in the body’s frame, [yaw, pitch] ' +
      'degrees: yaw from the body’s forward toward its right, pitch up); between them every ' +
      'piece moves smoothly through the keys, and the lash plays exactly so on every player, no ' +
      'gravity or physics. time (seconds) moves the playhead first; then one of: add (a key of ' +
      'the lash as it shows there), shape { aimSide, aimUp, curl, sway, sag, tipTwist } (the key ' +
      'there changed by that many degrees, sag metres; made first: aimSide/aimUp turn the whole ' +
      'lash toward the body’s right/up, curl bends it up toward the tip, sway toward the right, ' +
      'tipTwist its last third), ways (the key there exactly so), remove, fromLash (seconds: ' +
      'keys of the lash shown, e.g. a Lash from motion, that far apart, to change only some ' +
      'frames), clear. Answers the keys (time, first and last piece’s way).',
    inputSchema: {
      type: 'object',
      properties: {
        time: { type: 'number', minimum: 0 },
        add: { type: 'boolean' },
        shape: {
          type: 'object',
          properties: {
            aimSide: { type: 'number' },
            aimUp: { type: 'number' },
            curl: { type: 'number' },
            sway: { type: 'number' },
            sag: { type: 'number' },
            tipTwist: { type: 'number' },
          },
          additionalProperties: false,
        },
        ways: {
          type: 'array',
          items: { type: 'array', items: { type: 'number' }, minItems: 2, maxItems: 2 },
        },
        remove: { type: 'boolean' },
        fromLash: { type: 'number', minimum: 0.03 },
        clear: { type: 'boolean' },
      },
      additionalProperties: false,
    },
    step: (args) => `lashpose ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_lash_live_check',
    description:
      'The lash as the game draws it, measured: the animation plays (it is started if it was ' +
      'not) while the game records the lash’s world points for a round (up to 10 s), then the ' +
      'whole lash’s turn is measured in the world from one drawn frame to the next: its worst ' +
      '(degrees, when), the snaps (frames where all of it turns over 30° at once: it ' +
      'teleports), how many ' +
      'frames the game drew a second, and the problems. Unlike studio_lash_check (which measures ' +
      'the track) this sees what a player sees, the hand’s real turns included.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    timeoutMs: 40000,
    step: () => 'lashlive',
  },
  {
    name: 'studio_lash_path',
    description:
      'The hand’s path the last studio_lash_motion made the lash on (null before one): rate ' +
      '(frames a second), frames (the whip handle’s tip, world metres) and handles (the ' +
      'handle’s long side and up side, x y z each), to the millimetre: to see how fast and ' +
      'where the hand throws, or to try the lash away from the game.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'lashpath',
  },
  {
    name: 'studio_patch_animation',
    description:
      'Changes some keys of the animation (one undo step), not all of it: which keys by keys ' +
      '[index, ...], times [seconds] or from/to (seconds; none: every key). pose { joint: [x, y, ' +
      'z] } sets those joints (null takes one out), add { joint: [x, y, z] } adds degrees to what ' +
      'they have, remove [joints] takes joints out, lift (metres, null out) and lash (points, ' +
      'null out) set those. Answers what the animation is now and its problems (as ' +
      'studio_check_animation).',
    inputSchema: {
      type: 'object',
      properties: {
        keys: { type: 'array', items: { type: 'integer', minimum: 0 } },
        times: { type: 'array', items: { type: 'number', minimum: 0 } },
        from: { type: 'number', minimum: 0 },
        to: { type: 'number', minimum: 0 },
        pose: { type: 'object' },
        add: { type: 'object' },
        remove: { type: 'array', items: { type: 'string' } },
        lift: { type: ['number', 'null'], minimum: -1, maximum: 0.5 },
        lash: { type: ['array', 'null'], maxItems: 16 },
      },
      additionalProperties: false,
    },
    step: (args) => `patch ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_track',
    description:
      'Changes one joint’s whole track (one undo step), over the keys chosen as ' +
      'studio_patch_animation chooses them. op "copy" (joint to `to`, signs [x, y, z] multiply), ' +
      '"mirror" (the other side’s track onto joint, signs [1, -1, -1] by default; joint "all" ' +
      'mirrors every Left/Right pair from `from`, "Right" by default), "scale" and "offset" (by ' +
      'a number or [x, y, z]), "smooth" (each key averaged with its neighbours, passes times, ' +
      'round the loop) or "remove".',
    inputSchema: {
      type: 'object',
      properties: {
        op: { type: 'string', enum: ['copy', 'mirror', 'scale', 'offset', 'smooth', 'remove'] },
        joint: { type: 'string' },
        to: { type: 'string' },
        from: { type: 'string', enum: ['Left', 'Right'] },
        signs: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
        by: {},
        passes: { type: 'integer', minimum: 1, maximum: 10 },
        keys: { type: 'array', items: { type: 'integer', minimum: 0 } },
        times: { type: 'array', items: { type: 'number', minimum: 0 } },
      },
      required: ['op'],
      additionalProperties: false,
    },
    step: (args) => `track ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_retime',
    description:
      'The animation’s timing (one undo step): op "speed" (factor: 2 plays twice as fast), ' +
      '"trim" (only from..to seconds, starting at 0) or "shift" (every key after `from` later by ' +
      '`by` seconds, less with a negative by).',
    inputSchema: {
      type: 'object',
      properties: {
        op: { type: 'string', enum: ['speed', 'trim', 'shift'] },
        factor: { type: 'number', exclusiveMinimum: 0 },
        from: { type: 'number', minimum: 0 },
        to: { type: 'number', minimum: 0 },
        by: { type: 'number' },
      },
      required: ['op'],
      additionalProperties: false,
    },
    step: (args) => `retime ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_check_animation',
    description:
      'What the animation is and could have wrong, without changing it: keys and joints against ' +
      'the 32 each a player takes, joints jumping more than 90° between keys (they spin), a ' +
      'loop whose last key is not its first (a seam), lash shapes longer or shorter than the ' +
      'whip, and its size as sent (bytes; in the player field up to 2000, by name up to 30000).',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'check',
  },
  {
    name: 'studio_compare_animation',
    description:
      'How far another animation (joints by name, as studio_get_animation answers it) is from ' +
      'this one, joint by joint: the most degrees apart and when, most apart first. A baked clip ' +
      'against its first bake, a change against the one before.',
    inputSchema: {
      type: 'object',
      properties: { keys: { type: 'array', minItems: 1 }, blend: {}, loop: {} },
      required: ['keys'],
      additionalProperties: false,
    },
    step: (args) => `compare ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_lash_shape',
    description:
      'Shapes the whip’s lash on the keys chosen (as studio_patch_animation chooses them, one ' +
      'undo step) from a few numbers: curl (degrees toward the handle’s up side, from the handle ' +
      'to the tip), sway (degrees sideways) and sag (metres down at the tip), count points ' +
      '(12 at first); the whip’s length is kept. Played, the lash takes exactly that shape (no ' +
      'gravity). Answers the points.',
    inputSchema: {
      type: 'object',
      properties: {
        curl: { type: 'number', minimum: -720, maximum: 720 },
        sway: { type: 'number', minimum: -720, maximum: 720 },
        sag: { type: 'number', minimum: -3, maximum: 3 },
        count: { type: 'integer', minimum: 2, maximum: 16 },
        keys: { type: 'array', items: { type: 'integer', minimum: 0 } },
        times: { type: 'array', items: { type: 'number', minimum: 0 } },
        from: { type: 'number', minimum: 0 },
        to: { type: 'number', minimum: 0 },
      },
      additionalProperties: false,
    },
    step: (args) => `lashshape ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_sample',
    description:
      'Where joints are as the animation poses the body at each time (up to 20 times, 32 ' +
      'joints): in the body’s frame [right, forward, up] metres from its place on the floor. ' +
      'Feet on the ground have up near 0; a foot that keeps its place has the same right and ' +
      'forward at the times it stands.',
    inputSchema: {
      type: 'object',
      properties: {
        times: { type: 'array', items: { type: 'number', minimum: 0 }, minItems: 1, maxItems: 20 },
        joints: { type: 'array', items: { type: 'string' }, minItems: 1, maxItems: 32 },
      },
      required: ['times', 'joints'],
      additionalProperties: false,
    },
    timeoutMs: 40000,
    step: (args) => `sample ${JSON.stringify(args)}`,
  },
  {
    name: 'editor_frame',
    description:
      'Puts the studio’s camera by a part of the character, looking at it: target a joint’s ' +
      'name, "legs", "hand" (the one holding the prop) or "whole"; view "front", "back", ' +
      '"right", "left" or "top" (as the character faces); distance metres (2.5 at first). Then ' +
      'editor_screenshot or editor_burst shows it.',
    inputSchema: {
      type: 'object',
      properties: {
        target: { type: 'string' },
        view: { type: 'string', enum: ['front', 'back', 'right', 'left', 'top'] },
        distance: { type: 'number', minimum: 0.3, maximum: 20 },
      },
      additionalProperties: false,
    },
    step: (args) => `frame ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_cvar',
    description:
      'Reads an engine console variable of the studio’s game, or sets it with value (an ' +
      'integer), and answers its value (null when the game has none): ca_UseLookIK, ' +
      'ca_LockFeetWithIK (the studio has both 0: the keys are what the body does), and others.',
    inputSchema: {
      type: 'object',
      properties: {
        name: { type: 'string', pattern: '^[A-Za-z0-9_]{1,63}$' },
        value: { type: 'integer' },
      },
      required: ['name'],
      additionalProperties: false,
    },
    step: (args) => `cvar ${args.name}${args.value === undefined ? '' : ` ${args.value}`}`,
  },
  {
    name: 'studio_history',
    description:
      'History as the dock’s History tab shows it (newest first; a change the assistant made ' +
      'over the MCP is marked "MCP ·"), and the steps studio_undo would take back and studio_redo ' +
      'do again, the next first.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'history',
  },
  {
    name: 'editor_tab',
    description:
      'The dock’s tab shown: "Content", "Timeline", "History", "Saves" or "AI" (as a press on ' +
      'it), so a screenshot shows it.',
    inputSchema: {
      type: 'object',
      properties: {
        tab: { type: 'string', enum: ['Content', 'Timeline', 'History', 'Saves', 'AI'] },
      },
      required: ['tab'],
      additionalProperties: false,
    },
    step: (args) => `tab ${args.tab}`,
  },
  {
    name: 'studio_wait',
    description:
      'Waits until a bake (studio_game_clip path "bake"), a Lash to keys (studio_lash_keys) or ' +
      'a Lash from motion (studio_lash_motion) is over, at most 30 s, and answers what it made ' +
      '(lastBake or lastLash).',
    inputSchema: {
      type: 'object',
      properties: { for: { type: 'string', enum: ['bake', 'lash'] } },
      required: ['for'],
      additionalProperties: false,
    },
    timeoutMs: 40000,
    step: (args) => `wait ${args.for}`,
  },
  {
    name: 'studio_cloth',
    description:
      'Your mesh as cloth (one undo step): on, the game simulates it as cloth hanging from its ' +
      'highest vertices (turn the mesh first to choose that edge; a plane needs subdivisions to ' +
      'bend); off, solid again. mass: its weight in kg (0.25 to 10, 1 at first). A mesh must be ' +
      'shown (Static props).',
    inputSchema: {
      type: 'object',
      properties: {
        on: { type: 'boolean' },
        mass: { type: 'number', minimum: 0.25, maximum: 10 },
      },
      required: ['on'],
      additionalProperties: false,
    },
    step: (args) => `cloth ${args.on ? 'on' : 'off'}${args.mass ? ` ${args.mass}` : ''}`,
  },
  {
    name: 'studio_import_gltf',
    description:
      'Imports a glTF 2.0 file (.gltf or .glb, on this machine) as one of your meshes and shows ' +
      'it beside the character, no Blender needed: every mesh of its scene in world space (y up ' +
      'becomes the game’s z up), its materials’ colours, PNG base colour textures and normal ' +
      'maps as PNG or baseline JPEG (at most 1024 pixels a side; progressive JPEG not), at most ' +
      '65535 vertices. Static meshes ' +
      'only (studio_import_gltf_skin for a skinned mesh, studio_import_gltf_animation for an ' +
      'animation). path: the file; name: the mesh’s name. Answers { name, ' +
      'vertices, triangles, materials, textures, files } and the editor’s answer.',
    inputSchema: {
      type: 'object',
      properties: {
        path: { type: 'string', minLength: 1, maxLength: 1024 },
        name: { type: 'string', pattern: '^[A-Za-z0-9_-]{1,48}$' },
      },
      required: ['path', 'name'],
      additionalProperties: false,
    },
    run: (args, link) => importGltf(args, link),
  },
  {
    name: 'studio_import_gltf_skin',
    description:
      'Imports the skinned meshes of a glTF 2.0 file (.gltf or .glb, on this machine) as a skin ' +
      'the studio’s character wears, moving with its bones: a .skin bound to the game’s male ' +
      'skeleton (read from the game’s Data/Characters.pak; KCDS_GAME_DIR names another ' +
      'installation). The file’s joints match the game’s bones by name, any case, without a ' +
      'rig’s prefix; map: { <game bone>: <glTF joint> } names the rest; a joint matching no bone ' +
      'weighs on its nearest matching ancestor. Each vertex is moved by its joints’ offsets to ' +
      'the game’s bones (moved: the largest, metres). Answers { name, vertices, triangles, ' +
      'materials, textures, bones, mapped, moved, files } and the editor’s answer.',
    inputSchema: {
      type: 'object',
      properties: {
        path: { type: 'string', minLength: 1, maxLength: 1024 },
        name: { type: 'string', pattern: '^[A-Za-z0-9_-]{1,48}$' },
        map: { type: 'object', additionalProperties: { type: 'string', maxLength: 128 } },
      },
      required: ['path', 'name'],
      additionalProperties: false,
    },
    run: (args, link) => importGltfSkin(args, link),
  },
  {
    name: 'studio_import_fbx',
    description:
      'Imports an FBX file (on this machine) as one of your meshes (as: "mesh", shown beside the ' +
      'character, as studio_import_gltf) or as a skin the character wears (as: "skin", as ' +
      'studio_import_gltf_skin, map for joints the names miss). Blender, installed on this ' +
      'machine (KCDS_BLENDER names blender.exe elsewhere), reads the FBX headless and gives ' +
      'glTF; the glTF import does the rest. Answers what that import answers.',
    inputSchema: {
      type: 'object',
      properties: {
        path: { type: 'string', minLength: 1, maxLength: 1024 },
        name: { type: 'string', pattern: '^[A-Za-z0-9_-]{1,48}$' },
        as: { type: 'string', enum: ['mesh', 'skin'] },
        map: { type: 'object', additionalProperties: { type: 'string', maxLength: 128 } },
      },
      required: ['path', 'name'],
      additionalProperties: false,
    },
    timeoutMs: 180000,
    run: (args, link) => importFbx(args, link),
  },
  {
    name: 'studio_import_gltf_animation',
    description:
      'Puts an animation of a glTF 2.0 file (.gltf or .glb with a skin: Mixamo, Blender and the ' +
      'like) on the studio’s character and plays it (two undo steps: the body stood still to ' +
      'measure it, then the animation). The file’s joints match the game’s by name (any case, ' +
      'without a rig’s prefix such as mixamorig:); map adds or changes pairs { <game joint>: ' +
      '<file joint> }. Each game bone is laid along the file’s bone at rest, then turns as the ' +
      'file’s joint turns; the hips’ rise is each key’s lift. At most 32 keys (rate keys a ' +
      'second, 10 at first) and 32 joints, the body’s before the fingers, still joints left out. ' +
      'animation: its index or name (0 at first). Answers what matched, what did not, what was ' +
      'left out, and the editor’s answer.',
    inputSchema: {
      type: 'object',
      properties: {
        path: { type: 'string', minLength: 1, maxLength: 1024 },
        animation: { type: ['integer', 'string'] },
        rate: { type: 'number', minimum: 1, maximum: 60 },
        map: { type: 'object', additionalProperties: { type: 'string' } },
      },
      required: ['path'],
      additionalProperties: false,
    },
    timeoutMs: 40000,
    run: (args, link) => importGltfAnimation(args, link),
  },
  {
    name: 'studio_mesh_key',
    description:
      'Your mesh in the animation (one undo step): on, the key at the playhead (studio_playhead ' +
      'to a key’s time first) places the mesh where it stands now (place it with studio_place ' +
      'mesh first); off, that key no longer places it. Between mesh keys the mesh moves and turns ' +
      'as the keys blend, while playing and in the export (the server moves each player’s mesh). ' +
      'studio_set_animation takes a key’s mesh too: { at: [x, y, z], turn: [x, y, z] }. ' +
      'Without on it answers the mesh keys { key, time, at, turn }.',
    inputSchema: {
      type: 'object',
      properties: { on: { type: 'boolean' } },
      additionalProperties: false,
    },
    step: (args) => (args.on === undefined ? 'meshkey' : `meshkey ${args.on ? 'on' : 'off'}`),
  },
  {
    name: 'studio_wear_skin',
    description:
      'Puts a skinned mesh of yours on the studio’s character: <name>.skin in ' +
      '%LOCALAPPDATA%\\KCDS\\studio\\meshes (with <name>.mtl beside it), made by ' +
      'Dev/tools/assets/blender/kcds_skin.py bound to the character’s skeleton. It moves with ' +
      'the character’s bones as the game’s own clothes do. An empty name takes it off.',
    inputSchema: {
      type: 'object',
      properties: { name: { type: 'string', pattern: '^[A-Za-z0-9_-]{0,64}$' } },
      required: ['name'],
      additionalProperties: false,
    },
    step: (args) => (args.name ? `skin ${args.name}` : 'skin'),
  },
  {
    name: 'studio_mesh_body',
    description:
      'Your mesh’s body in the game’s physics (one undo step): "none" (it stands as placed and ' +
      'nothing hits it, at first), "solid" (it stands and the character and things hit it) or ' +
      '"falls" (a rigid body of mass kg, 0.1 to 200, 5 at first: it falls from where it is ' +
      'placed and rests; placing it again drops it again). A body makes a cloth mesh solid; a ' +
      'mesh held in a joint has none. A mesh must be shown. Without kind it answers the body.',
    inputSchema: {
      type: 'object',
      properties: {
        kind: { type: 'string', enum: ['none', 'solid', 'falls'] },
        mass: { type: 'number', minimum: 0.1, maximum: 200 },
      },
      additionalProperties: false,
    },
    step: (args) => (args.kind ? `body ${args.kind}${args.mass ? ` ${args.mass}` : ''}` : 'body'),
  },
  {
    name: 'studio_clothes',
    description:
      'What the studio’s character wears: an outfit ("henry" at first, "traveller", "none") and ' +
      'the names of its pieces taken off (off). Without arguments it answers the outfits, ' +
      'their pieces and what the character wears now.',
    inputSchema: {
      type: 'object',
      properties: {
        outfit: { type: 'string', enum: ['henry', 'traveller', 'none'] },
        off: { type: 'array', items: { type: 'string' } },
      },
      additionalProperties: false,
    },
    step: (args) => (args.outfit ? `clothes ${JSON.stringify(args)}` : 'clothes'),
  },
  {
    name: 'studio_prop_physics',
    description:
      'How the rope or the whip’s lash moves (one undo step); give any of: gravity (m/s², 0 to ' +
      '30, 9.8 at first), damping (0 to 5: how fast swinging dies down, 0.3), mass (kg, 0.05 ' +
      'to 10, 0.5), air (air drag, 0 to 2, 0.1), bend (degrees a joint may bend, 0 to 90; 0 ' +
      'is free like a limp rope, 10 to 25 is a springy whip), wind (m/s, 0 to 20), stiffness ' +
      '(0 to 500, 0 the game’s own: how hard it resists stretching), friction (0 to 2, 0.5) and ' +
      'thickness (metres across it collides, 0.005 to 0.2, 0.02), collide (1 with the ' +
      'character, props and floor, 2 the floor only so a lash swings past the body, 0 through ' +
      'everything); for a whip ' +
      'also pitch (grip angle, 0 to 180: 0 points the handle down the forearm, 90 out of the ' +
      'fist) and twist (-180 to 180, about the forearm), and the lash posed instead of moved ' +
      'by physics: shape (0 physics, 1 the posed shape alone with no gravity; between, it is ' +
      'pulled toward the shape), curl and sway (-540 to 540: degrees the posed lash bends from ' +
      'the handle to the tip toward the handle’s up side and sideways; 0 and 0 straight out of ' +
      'the handle). Set shape, curl and sway in prop keys (studio_set_animation) to animate a ' +
      'swing exactly, and shape back to 0 to let the lash fly on with its speed. Answers the ' +
      'settings now.',
    inputSchema: {
      type: 'object',
      properties: {
        shape: { type: 'number', minimum: 0, maximum: 1 },
        curl: { type: 'number', minimum: -540, maximum: 540 },
        sway: { type: 'number', minimum: -540, maximum: 540 },
        gravity: { type: 'number', minimum: 0, maximum: 30 },
        damping: { type: 'number', minimum: 0, maximum: 5 },
        mass: { type: 'number', minimum: 0.05, maximum: 10 },
        air: { type: 'number', minimum: 0, maximum: 2 },
        bend: { type: 'number', minimum: 0, maximum: 90 },
        wind: { type: 'number', minimum: 0, maximum: 20 },
        stiffness: { type: 'number', minimum: 0, maximum: 500 },
        friction: { type: 'number', minimum: 0, maximum: 2 },
        thickness: { type: 'number', minimum: 0.005, maximum: 0.2 },
        collide: { type: 'integer', minimum: 0, maximum: 2 },
        pitch: { type: 'number', minimum: 0, maximum: 180 },
        twist: { type: 'number', minimum: -180, maximum: 180 },
      },
      additionalProperties: false,
    },
    step: (args) => `physics ${JSON.stringify(args)}`,
  },
  {
    name: 'studio_find_joints',
    description:
      'Finds the character’s joints whose names hold a text (case does not matter), at most 40: ' +
      '{ id, name, parent }. The body’s main joints: Hips, Spine, Spine1, Spine2, Neck, Head, ' +
      'LeftShoulder/RightShoulder, LeftArm/RightArm, LeftForeArm/RightForeArm, ' +
      'LeftHand/RightHand, LeftUpLeg/RightUpLeg, LeftLeg/RightLeg, LeftFoot/RightFoot. Show ' +
      'Bones (studio_bones) first.',
    inputSchema: {
      type: 'object',
      properties: { text: { type: 'string', minLength: 1 } },
      required: ['text'],
      additionalProperties: false,
    },
    step: (args) => `find ${args.text}`,
  },
  {
    name: 'studio_set_animation',
    description:
      'Sets the character’s whole animation in one step (it can be undone) and plays it. Keys ' +
      'in time order, the first at 0 s, up to 60 s and 64 keys; each key turns joints by name ' +
      '[x, y, z] degrees about the joint’s parent’s axes, on top of the body standing still. A ' +
      'joint a key leaves out is not turned in it. blend: "spline" (default: a smooth curve ' +
      'through the keys, best for swings), "smooth" (eased at each key) or "linear". loop: ' +
      'true by default (make the last key equal the first for a seamless loop). events: effects ' +
      '({ time, kind: "effect", name: "Library.group.name" of the game’s catalogue, e.g. ' +
      '"WH_Particels.dust.sweep", joint: e.g. "RightHand", scale }) and sounds ({ time, kind: ' +
      '"sound", name: a game sound trigger }) played as the playhead passes them. Answers the ' +
      'keys, the length, unknownJoints (names not found) and eventsLeftOut. A key’s lift raises ' +
      'the hips (metres, -1 to 0.5) and its lash shapes the whip’s lash (up to 16 points ' +
      '[along, right, up] from the handle’s tip in its frame), as a baked clip’s keys have ' +
      'them (studio_get_animation shows them). A key’s prop ' +
      '(the prop in the hand) sets from that key on: hand ("RightHand" or "LeftHand"), a ' +
      'whip’s grip pitch and twist, and gravity, damping, mass, air, bend, stiffness and ' +
      'collide as studio_prop_physics; numbers blend toward the next key that sets them, the ' +
      'hand and collide switch at the key. The export carries them.',
    inputSchema: {
      type: 'object',
      properties: {
        keys: {
          type: 'array',
          minItems: 1,
          maxItems: 64,
          items: {
            type: 'object',
            properties: {
              time: { type: 'number', minimum: 0, maximum: 60 },
              pose: {
                type: 'object',
                additionalProperties: {
                  type: 'array',
                  items: { type: 'number' },
                  minItems: 3,
                  maxItems: 3,
                },
              },
              prop: {
                type: 'object',
                properties: {
                  hand: { type: 'string', enum: ['RightHand', 'LeftHand'] },
                  pitch: { type: 'number', minimum: 0, maximum: 180 },
                  twist: { type: 'number', minimum: -180, maximum: 180 },
                  gravity: { type: 'number', minimum: 0, maximum: 30 },
                  damping: { type: 'number', minimum: 0, maximum: 5 },
                  mass: { type: 'number', minimum: 0.05, maximum: 10 },
                  air: { type: 'number', minimum: 0, maximum: 2 },
                  bend: { type: 'number', minimum: 0, maximum: 90 },
                  stiffness: { type: 'number', minimum: 0, maximum: 500 },
                  collide: { type: 'integer', minimum: 0, maximum: 2 },
                  shape: { type: 'number', minimum: 0, maximum: 1 },
                  curl: { type: 'number', minimum: -540, maximum: 540 },
                  sway: { type: 'number', minimum: -540, maximum: 540 },
                },
                additionalProperties: false,
              },
              // The hips raised or lowered (metres) and the whip's lash shape (points from the
              // handle's tip in its frame: along, right, up), as a baked clip's keys have them.
              lift: { type: 'number', minimum: -1, maximum: 0.5 },
              lash: {
                type: 'array',
                minItems: 1,
                maxItems: 16,
                items: {
                  type: 'array',
                  items: { type: 'number', minimum: -6, maximum: 6 },
                  minItems: 3,
                  maxItems: 3,
                },
              },
            },
            required: ['time', 'pose'],
            additionalProperties: false,
          },
        },
        blend: { type: 'string', enum: ['spline', 'smooth', 'linear'] },
        loop: { type: 'boolean' },
        play: { type: 'boolean' },
        events: {
          type: 'array',
          maxItems: 32,
          items: {
            type: 'object',
            properties: {
              time: { type: 'number', minimum: 0, maximum: 60 },
              kind: { type: 'string', enum: ['effect', 'sound'] },
              name: { type: 'string' },
              joint: { type: 'string' },
              scale: { type: 'number', minimum: 0.01, maximum: 32 },
            },
            required: ['time', 'kind', 'name'],
            additionalProperties: false,
          },
        },
      },
      required: ['keys'],
      additionalProperties: false,
    },
    steps: (args) => ['bones on', `animation ${JSON.stringify(args)}`],
  },
  {
    name: 'studio_get_animation',
    description: 'The character’s animation now: its keys with the joints by name, blend, loop.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'animation',
  },
  {
    name: 'studio_play',
    description: 'Plays (on: true) or pauses the animation, from the playhead.',
    inputSchema: {
      type: 'object',
      properties: { on: { type: 'boolean' } },
      required: ['on'],
      additionalProperties: false,
    },
    step: (args) => `play ${args.on ? 'on' : 'off'}`,
  },
  {
    name: 'studio_playhead',
    description: 'Puts the playhead at a time (seconds): the body shows the animation there.',
    inputSchema: {
      type: 'object',
      properties: { time: { type: 'number', minimum: 0, maximum: 60 } },
      required: ['time'],
      additionalProperties: false,
    },
    step: (args) => `time ${args.time}`,
  },
  {
    name: 'studio_undo',
    description: 'Undoes the studio’s last change.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'undo',
  },
  {
    name: 'studio_redo',
    description: 'Redoes the change undone last.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'redo',
  },
  {
    name: 'editor_camera',
    description:
      'Puts the open editor’s free camera at a world position, looking at another (metres). ' +
      'Works in World Architect and in the studio.',
    inputSchema: {
      type: 'object',
      properties: {
        position: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
        look_at: { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 },
      },
      required: ['position', 'look_at'],
      additionalProperties: false,
    },
    // One pose: a separate lookat would aim from the camera's last reported place.
    steps: (args) => [`view ${args.position.join(' ')} ${args.look_at.join(' ')}`],
  },
  {
    name: 'editor_camera_position',
    description: 'Where the open editor’s free camera is (World Architect or the studio).',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    step: () => 'camera',
  },
  {
    name: 'studio_joint_info',
    description:
      'The geometry of joints by name, to pose them right: in the body’s frame [right, ' +
      'forward, up] metres, each joint’s position from the body’s root, its bone (to its ' +
      'longest child), its parent’s axes x, y, z (the axes studio_set_animation’s [x, y, z] ' +
      'degrees turn it about) and plus30moves: where a turn of +30 degrees about each axis moves ' +
      'the bone’s end. Example: if plus30moves.z of RightArm is [0, 0.2, 0.1], a positive z ' +
      'turn swings the arm forward and up. Turns add on top of the pose the body has now.',
    inputSchema: {
      type: 'object',
      properties: {
        joints: { type: 'array', items: { type: 'string' }, minItems: 1, maxItems: 32 },
      },
      required: ['joints'],
      additionalProperties: false,
    },
    step: (args) => `jointinfo ${args.joints.join(' ')}`,
  },
  {
    name: 'editor_screenshot',
    description:
      'A picture of the open editor’s game window (World Architect or the studio) as it is ' +
      'now, to check what a change looks like. width: at most this many pixels across (1280).',
    inputSchema: {
      type: 'object',
      properties: { width: { type: 'integer', minimum: 320, maximum: 1920 } },
      additionalProperties: false,
    },
    run: (args) => screenshot(args.width ?? 1280),
  },
  {
    name: 'editor_burst',
    description:
      'Motion in one picture: count pictures of the game window (2 to 16, 8 at first) every_ms ' +
      'apart (40 to 2000, 150 at first) put in a contact sheet, columns across (4 at first), each ' +
      'tile width pixels wide (160 to 960, 480 at first) and labelled with its number and time. ' +
      'view_only (true at first) crops each to the 3D view without the editor’s panels. Use it ' +
      'to judge how something moves: a whip’s swing, a game clip, a prop key.',
    inputSchema: {
      type: 'object',
      properties: {
        count: { type: 'integer', minimum: 2, maximum: 16 },
        every_ms: { type: 'integer', minimum: 40, maximum: 2000 },
        columns: { type: 'integer', minimum: 1, maximum: 8 },
        width: { type: 'integer', minimum: 160, maximum: 960 },
        view_only: { type: 'boolean' },
      },
      additionalProperties: false,
    },
    run: (args) =>
      screenshot(args.width ?? 480, {
        count: args.count ?? 8,
        everyMs: args.every_ms ?? 150,
        columns: args.columns ?? 4,
        viewOnly: args.view_only !== false,
      }),
  },
]);

// A picture of the editor's game window (screenshot.ps1: PrintWindow), as MCP image content;
// with `burst` ({ count, everyMs, columns, viewOnly }) a contact sheet of several.
async function screenshot(width, burst = null) {
  const pipe = findPipe();
  const pid = pipe ? /kcds-editor-(\d+)$/.exec(pipe)?.[1] : null;
  if (!pid) {
    throw new Error('No KCDS editor is open on this machine.');
  }
  const out = path.join(os.tmpdir(), `kcds-editor-shot-${pid}-${Date.now()}.png`);
  const more = burst
    ? [
        '-Count',
        String(burst.count),
        '-EveryMs',
        String(burst.everyMs),
        '-Columns',
        String(burst.columns),
        ...(burst.viewOnly ? ['-ViewOnly'] : []),
      ]
    : [];
  await new Promise((resolve, reject) => {
    execFile(
      'powershell.exe',
      [
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        path.join(__dirname, 'screenshot.ps1'),
        '-ProcessId',
        pid,
        '-Out',
        out,
        '-Width',
        String(width),
        ...more,
      ],
      { timeout: 60000 },
      (error) => (error ? reject(new Error(`no picture: ${error.message}`)) : resolve()),
    );
  });
  try {
    const data = fs.readFileSync(out).toString('base64');
    return [{ type: 'image', data, mimeType: 'image/png' }];
  } finally {
    fs.rmSync(out, { force: true });
  }
}

// studio_make_mesh: the shape's files written by studio_shapes.py (Python, KCDS_PYTHON or
// python), then the editor's meshes asked for anew and the mesh shown. `folder`: where the files
// go (the studio's meshes folder at first; the tests' own).
async function makeMesh(args, link, environment = process.env, folder = null) {
  const { name, ...options } = args;
  const command = ['-I', SHAPES_SCRIPT, name, JSON.stringify(options), ...(folder ? [folder] : [])];
  return runMeshTool('studio_make_mesh', command, name, link, environment);
}

// studio_import_gltf: the glTF's meshes written by kcds_gltf.py, then shown as studio_make_mesh's.
async function importGltf(args, link, environment = process.env, folder = null) {
  const command = ['-I', GLTF_SCRIPT, args.path, args.name, ...(folder ? [folder] : [])];
  return runMeshTool('studio_import_gltf', command, args.name, link, environment);
}

// An FBX file as glTF through Blender headless (kcds_fbx.py), then a mesh or a skin of it.
async function importFbx(args, link, environment = process.env) {
  const python = environment.KCDS_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
  const glb = path.join(os.tmpdir(), `kcds-fbx-${args.name}.glb`);
  const printed = await new Promise((resolve, reject) => {
    execFile(python, ['-I', FBX_SCRIPT, args.path, glb], { timeout: 150000 }, (error, stdout) => {
      if (error && !stdout) {
        reject(new Error(`studio_import_fbx: Python did not run (${error.message})`));
        return;
      }
      resolve(String(stdout));
    });
  });
  const made = JSON.parse(printed.trim().split(/\r?\n/).at(-1) || '{}');
  if (!made.glb) {
    throw new Error(`studio_import_fbx: ${made.error || printed.trim() || 'no answer'}`);
  }
  const given = { path: glb, name: args.name, ...(args.map ? { map: args.map } : {}) };
  return args.as === 'skin'
    ? importGltfSkin(given, link, environment)
    : importGltf(given, link, environment);
}

// A skinned glTF mesh as a .skin on the character's skeleton (kcds_gltf_skin.py), then worn.
async function importGltfSkin(args, link, environment = process.env, folder = null) {
  const map = Object.entries(args.map ?? {})
    .map(([game, gltf]) => `${game}=${gltf}`)
    .join(',');
  const command = [
    '-I',
    GLTF_SKIN_SCRIPT,
    args.path,
    args.name,
    ...(folder ? [folder] : []),
    ...(map ? ['--map', map] : []),
    ...(environment.KCDS_GAME_DIR ? ['--game', environment.KCDS_GAME_DIR] : []),
  ];
  return runMeshTool(
    'studio_import_gltf_skin',
    command,
    args.name,
    link,
    environment,
    `skin ${args.name}`,
  );
}

// The result of an editor step's answer ("<line> -> <result JSON> <state JSON>").
function resultOf(answer) {
  const start = answer.indexOf(' -> ');
  if (start < 0) {
    throw new Error(`the editor answered: ${answer.slice(0, 200)}`);
  }
  const text = answer.slice(start + 4);
  let depth = 0;
  let inString = false;
  for (let at = 0; at < text.length; at += 1) {
    const character = text[at];
    if (inString) {
      if (character === '\\') {
        at += 1;
      } else if (character === '"') {
        inString = false;
      }
    } else if (character === '"') {
      inString = true;
    } else if (character === '{' || character === '[') {
      depth += 1;
    } else if (character === '}' || character === ']') {
      depth -= 1;
    }
    if (!inString && depth === 0 && (character === ' ' || at === text.length - 1)) {
      return JSON.parse(text.slice(0, character === ' ' ? at : at + 1));
    }
  }
  return JSON.parse(text);
}

// One Python tool's printed JSON line; throws its { error }.
async function runPythonTool(tool, command, environment) {
  const python = environment.KCDS_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
  const printed = await new Promise((resolve, reject) => {
    execFile(python, command, { timeout: 60000, maxBuffer: 16 * 1024 * 1024 }, (error, stdout) => {
      if (error && !stdout) {
        reject(new Error(`${tool}: Python did not run (${error.message})`));
        return;
      }
      resolve(String(stdout));
    });
  });
  let made;
  try {
    made = JSON.parse(printed.trim().split(/\r?\n/).at(-1));
  } catch {
    throw new Error(`${tool}: ${printed.trim() || 'no answer from the Python tool'}`);
  }
  if (made.error) {
    throw new Error(`${tool}: ${made.error}`);
  }
  return made;
}

// studio_import_gltf_animation: the game's skeleton from the editor, the names matched and the
// joints measured standing still, the file converted (kcds_gltf_anim.py), then the animation set.
async function importGltfAnimation(args, link, environment = process.env) {
  if (!link) {
    throw new Error('No KCDS editor is open on this machine.');
  }
  await link.step('bones on', LONG_STEP_TIMEOUT_MS);
  const joints = resultOf(await link.step('joints'));
  if (!Array.isArray(joints) || joints.length === 0) {
    throw new Error('studio_import_gltf_animation: the editor has no joints yet (show Bones once)');
  }
  const named = joints.map((joint) => ({
    name: joint.name,
    parent: joints[joint.parent]?.name ?? '',
  }));
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gltf-animation-'));
  try {
    const requestPath = path.join(folder, 'request.json');
    const request = {
      joints: named,
      map: args.map ?? {},
      animation: args.animation ?? 0,
      rate: args.rate ?? 10,
    };
    fs.writeFileSync(requestPath, JSON.stringify(request));
    const matched = await runPythonTool(
      'studio_import_gltf_animation',
      ['-I', GLTF_ANIMATION_SCRIPT, 'names', args.path, requestPath],
      environment,
    );
    const wanted = Object.keys(matched.mapped);
    if (wanted.length === 0) {
      throw new Error(
        'studio_import_gltf_animation: no joint of the file matches the game’s (give map)',
      );
    }
    // Standing still, so each parent's axes are the ones the keys turn about.
    await link.step(`animation ${JSON.stringify({ keys: [{ time: 0, pose: {} }], play: false })}`);
    const info = {};
    for (let at = 0; at < wanted.length; at += 32) {
      const answer = resultOf(
        await link.step(`jointinfo ${wanted.slice(at, at + 32).join(' ')}`, LONG_STEP_TIMEOUT_MS),
      );
      for (const joint of answer.joints ?? []) {
        if (joint.found) {
          info[joint.name] = joint;
        }
      }
    }
    fs.writeFileSync(requestPath, JSON.stringify({ ...request, info }));
    const made = await runPythonTool(
      'studio_import_gltf_animation',
      ['-I', GLTF_ANIMATION_SCRIPT, 'convert', args.path, requestPath],
      environment,
    );
    const set = await link.step(
      `animation ${JSON.stringify(made.animation)}`,
      LONG_STEP_TIMEOUT_MS,
    );
    const summary = {
      keys: made.animation.keys.length,
      mapped: made.mapped,
      unmappedGltf: made.unmappedGltf,
      unmappedGame: made.unmappedGame,
      leftOut: made.leftOut,
    };
    return [
      { type: 'text', text: JSON.stringify(summary) },
      { type: 'text', text: set },
    ];
  } finally {
    fs.rmSync(folder, { recursive: true, force: true });
  }
}

// A Python tool that writes one of your meshes and prints one JSON line ({ error } when it
// could not), then the mesh shown in the editor.
// `show`: the editor step that shows what was made (a mesh beside the character, or a skin on it).
async function runMeshTool(tool, command, name, link, environment, show = `mesh ${name}`) {
  const python = environment.KCDS_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
  const printed = await new Promise((resolve, reject) => {
    execFile(python, command, { timeout: 30000 }, (error, stdout) => {
      if (error && !stdout) {
        reject(new Error(`${tool}: Python did not run (${error.message})`));
        return;
      }
      resolve(String(stdout));
    });
  });
  const lines = printed.trim().split(/\r?\n/);
  let made;
  try {
    made = JSON.parse(lines.at(-1));
  } catch {
    throw new Error(`${tool}: ${printed.trim() || 'no answer from the Python tool'}`);
  }
  if (made.error) {
    throw new Error(`${tool}: ${made.error}`);
  }
  const content = [{ type: 'text', text: JSON.stringify(made) }];
  if (link) {
    await link.step('meshes');
    content.push({ type: 'text', text: await link.step(show) });
  } else {
    content.push({
      type: 'text',
      text: 'No KCDS editor is open on this machine: the files are made.',
    });
  }
  return content;
}

// The game's pipe: KCDS_EDITOR_PID's, or the first KCDS editor pipe on this machine.
function findPipe(environment = process.env, listPipes = () => fs.readdirSync('\\\\.\\pipe\\')) {
  if (environment.KCDS_EDITOR_PID) {
    return `\\\\.\\pipe\\kcds-editor-${environment.KCDS_EDITOR_PID}`;
  }
  const names = listPipes().filter((name) => /^kcds-editor-\d+$/.test(name));
  return names.length > 0 ? `\\\\.\\pipe\\${names[0]}` : null;
}

// The connection to the game: one step at a time, answered by id.
class EditorLink {
  constructor(path, connect = (where) => net.connect(where)) {
    this.path = path;
    this.connect = connect;
    this.socket = null;
    this.buffer = '';
    this.nextId = 1;
    this.waiting = new Map();
  }

  open() {
    if (this.socket) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      const socket = this.connect(this.path);
      socket.setEncoding('utf8');
      socket.once('connect', () => {
        this.socket = socket;
        resolve();
      });
      socket.once('error', (error) => {
        this.socket = null;
        reject(error);
      });
      socket.on('data', (text) => this.receive(text));
      socket.on('close', () => {
        this.socket = null;
        for (const { reject: fail } of this.waiting.values()) {
          fail(new Error('the game closed the editor bridge'));
        }
        this.waiting.clear();
      });
    });
  }

  receive(text) {
    this.buffer += text;
    let end;
    while ((end = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, end);
      this.buffer = this.buffer.slice(end + 1);
      let answer = null;
      try {
        answer = JSON.parse(line);
      } catch {
        continue;
      }
      const pending = this.waiting.get(answer.id);
      if (pending) {
        this.waiting.delete(answer.id);
        clearTimeout(pending.timer);
        pending.resolve(String(answer.text ?? ''));
      }
    }
  }

  // A step longer than the game's pipe takes (4 KB a line, Client/runtime/architect/
  // editor_pipe.cpp: a whole baked animation was lost) goes in parts the editor puts together
  // (`part <index> <count> <text>`); the answer is the last part's.
  // timeoutMs: how long the step may take (a bake or a sample of many times takes longer). The
  // skeleton's first showing sends every joint's name: `bones on` gets longer too (it timed out).
  async step(line, timeoutMs = STEP_TIMEOUT_MS) {
    const slow = line.startsWith('bones on');
    const wait = slow ? Math.max(timeoutMs, LONG_STEP_TIMEOUT_MS) : timeoutMs;
    if (line.length <= STEP_PART_CHARS) {
      return this.stepLine(line, wait);
    }
    const count = Math.ceil(line.length / STEP_PART_CHARS);
    let answer = '';
    for (let index = 0; index < count; index += 1) {
      const text = line.slice(index * STEP_PART_CHARS, (index + 1) * STEP_PART_CHARS);
      answer = await this.stepLine(`part ${index} ${count} ${text}`, wait);
    }
    return answer;
  }

  async stepLine(line, timeoutMs = STEP_TIMEOUT_MS) {
    await this.open();
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(id);
        const shown = line.length > 120 ? `${line.slice(0, 120)}…` : line;
        reject(new Error(`the editor did not answer "${shown}" within ${timeoutMs / 1000} s`));
      }, timeoutMs);
      this.waiting.set(id, { resolve, reject, timer });
      this.socket.write(`${JSON.stringify({ id, line })}\n`);
    });
  }

  close() {
    this.socket?.end();
    this.socket = null;
  }
}

// One JSON-RPC request: its result, or an error { code, message }.
async function handle(request, link) {
  const { method, params } = request;
  if (method === 'initialize') {
    return {
      result: {
        protocolVersion: PROTOCOL_VERSION,
        capabilities: { tools: {} },
        serverInfo: SERVER,
      },
    };
  }
  if (method === 'ping') {
    return { result: {} };
  }
  if (method === 'tools/list') {
    return {
      result: {
        tools: TOOLS.map(({ name, description, inputSchema }) => ({
          name,
          description,
          inputSchema,
        })),
      },
    };
  }
  if (method === 'tools/call') {
    const tool = TOOLS.find((one) => one.name === params?.name);
    if (!tool) {
      return { error: { code: -32602, message: `unknown tool ${params?.name}` } };
    }
    const args = params.arguments ?? {};
    // The arguments' names as the tool's schema has them: a call missing one or naming another
    // is answered with what is wrong (it threw out of the server before, ending it).
    const schema = tool.inputSchema;
    const missing = (schema.required ?? []).filter((key) => args[key] === undefined);
    const unknown =
      schema.additionalProperties === false
        ? Object.keys(args).filter((key) => !(key in (schema.properties ?? {})))
        : [];
    if (missing.length > 0 || unknown.length > 0) {
      const wrong = [
        ...(missing.length > 0 ? [`missing ${missing.join(', ')}`] : []),
        ...(unknown.length > 0 ? [`unknown ${unknown.join(', ')}`] : []),
      ];
      const known = Object.keys(schema.properties ?? {}).join(', ') || 'none';
      return {
        result: {
          isError: true,
          content: [
            { type: 'text', text: `${tool.name}: ${wrong.join('; ')} (arguments: ${known})` },
          ],
        },
      };
    }
    if (tool.run) {
      // A tool with its own result (a picture of the game's window, a mesh made on this machine).
      try {
        return { result: { content: await tool.run(args, link) } };
      } catch (error) {
        return {
          result: { isError: true, content: [{ type: 'text', text: String(error.message) }] },
        };
      }
    }
    let steps;
    try {
      steps = tool.steps ? tool.steps(args) : [tool.step(args)];
    } catch (error) {
      return {
        result: {
          isError: true,
          content: [{ type: 'text', text: `${tool.name}: ${error.message}` }],
        },
      };
    }
    if (!link) {
      return {
        result: {
          isError: true,
          content: [{ type: 'text', text: 'No KCDS editor is open on this machine.' }],
        },
      };
    }
    try {
      const answers = [];
      for (const step of steps) {
        answers.push(await link.step(step, tool.timeoutMs ?? STEP_TIMEOUT_MS));
      }
      return { result: { content: [{ type: 'text', text: answers.join('\n') }] } };
    } catch (error) {
      return {
        result: { isError: true, content: [{ type: 'text', text: String(error.message) }] },
      };
    }
  }
  return { error: { code: -32601, message: `unknown method ${method}` } };
}

module.exports = {
  TOOLS,
  SURFACES,
  findPipe,
  EditorLink,
  handle,
  makeMesh,
  importGltf,
  importGltfAnimation,
};
