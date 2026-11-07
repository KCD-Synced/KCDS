'use strict';

// World Architect's editor (Client/resources/architect/editor.cjs): its modes, tools,
// content, details and undo stack, driven with stand-ins for the game's pieces.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  ArchitectEditor,
  architectFacade,
} = require('../../../../Client/resources/architect/editor.cjs');
const { Vector3 } = require('./math.cjs');
const { Shell } = require('../../../../Client/resources/architect/shell.cjs');

// A press in the middle of the world view (theme.cjs LAYOUT.view).
const VIEW_X = 900;
const VIEW_Y = 400;

// The last call, past the grass carpet a placed building hides under itself (architectGrass).
function lastCall(calls) {
  return calls.filter((call) => call[1]?.op !== 'architectGrass').at(-1);
}

function setup() {
  const calls = [];
  const down = new Set();
  let poll = null;
  const world = { hit: { position: { x: 10.4, y: 19.8, z: 5 }, levelObject: null } };
  const saves = [];
  const handle = (kind, args) => {
    const item = {
      kind,
      args,
      destroy() {
        calls.push(['destroy', kind]);
      },
      remove() {
        calls.push(['remove', kind]);
      },
      undo() {
        calls.push(['undo', kind]);
      },
      setPose(position, turn) {
        calls.push(['setPose', position, turn]);
      },
    };
    calls.push([kind, ...args]);
    return item;
  };
  const screen = {
    on() {},
    focus() {
      calls.push(['focus']);
    },
    blur() {
      calls.push(['blur']);
    },
    close() {},
    setVisible() {},
    stage: () => null,
  };
  const api = {
    NativeUI: { createScreen: () => screen },
    NoClip: {
      enable: () => 'enabled',
      disable() {},
      setSpeed() {},
      setInputEnabled(enabled) {
        calls.push(['look', enabled]);
      },
      getPosition: () => ({ x: 0, y: 0, z: 100 }),
      screenLine: () => ({ x: 0, y: 0, z: -1 }),
      toScreen: () => ({ x: 0, y: 0 }),
      focus: (target, distance) => calls.push(['focus', target.x, target.y, target.z, distance]),
    },
    World: {
      // The modes the game client's World.raycast takes (Client/resources/world.cjs).
      raycast: (from, to, options = {}) => {
        if (options.mode !== undefined && !['cover', 'anything', 'ground'].includes(options.mode)) {
          throw new TypeError(`World.raycast: mode ${options.mode}`);
        }
        return world.hit;
      },
    },
    Prefab: { place: (name, at, turn) => handle('prefab', [name, at.x, at.y, at.z, turn.z]) },
    Terrain: {
      flatten: (at, radius, height, fade) => handle('flatten', [at.x, at.y, radius, height, fade]),
      raise: (at, radius, amount, fade) => handle('raise', [at.x, at.y, radius, amount, fade]),
      paint: (at, radius, material, fade) => handle('paint', [at.x, at.y, radius, material, fade]),
    },
    Vegetation: {
      kinds: () => ['objects/natural/trees/oak/oak_a.cgf'],
      plant: (name, at, options) => handle('plant', [name, at.x, at.y, options.turn]),
    },
    Collision: {
      addBox: ({ center, size }) => {
        calls.push(['box', center.x, center.y, center.z, size.x, size.y, size.z]);
        return {
          rotation: { x: 0, y: 0, z: 0, w: 1 },
          set({ center: c, size: s }) {
            calls.push(['setBox', c.x, c.y, c.z, s.x, s.y, s.z]);
          },
          remove() {
            calls.push(['removeBox']);
          },
        };
      },
    },
    Gizmos: {
      set: (id) => calls.push(['gizmo', id]),
      remove: (id) => calls.push(['gizmoRemove', id]),
      clear: () => calls.push(['gizmoClear']),
    },
    Key: { isDown: (key) => down.has(key) },
    Vector3,
    prefabNames: () => ['structures/living/houses/house_1s_2r_a', 'door/door_village_left'],
    log() {},
    gameUi: (shown) => calls.push(['gameUi', shown]),
    send: (message) => calls.push(['send', message]),
    later: (ms, callback) => {
      saves.push(callback);
      return 1;
    },
    every: (ms, callback) => {
      poll = callback;
      return () => {
        poll = null;
      };
    },
  };
  const editor = new ArchitectEditor(api);
  return { editor, calls, down, world, saves, tick: () => poll?.() };
}

test('content tiles arm placing; a press places on the grid with the ground fitted', () => {
  const { editor, calls, down } = setup();
  // Shift held keeps placing.
  down.add('shift');
  assert.deepEqual(
    editor.items().map((item) => item.name),
    ['house_1s_2r_a'],
  );
  editor.pressTile(0);
  assert.equal(editor.state().chosen, 'structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  // The pad covers the house's footprint (its box's half diagonal) and fades over 3 m.
  assert.deepEqual(calls[0], ['flatten', 10, 20, 4 * Math.SQRT2, 5, 3]);
  assert.deepEqual(calls[1], ['prefab', 'structures/living/houses/house_1s_2r_a', 10, 20, 5, 0]);
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.placed.length, 2);
  editor.undo();
  assert.equal(editor.placed.length, 1);
  editor.redo();
  assert.equal(editor.placed.length, 2);
  editor.input('kcds_ui_back', 'press');
  assert.equal(editor.state().chosen, null);
});

test('a press outside the world view does nothing', () => {
  const { editor, calls } = setup();
  editor.pressTile(0);
  editor.pressWorld(100, 400);
  editor.pressWorld(900, 900);
  assert.equal(calls.length, 0);
});

test('Select picks, Move puts it where you press, Rotate turns it; the details step it', () => {
  const { editor, calls } = setup();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.useTool('select');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.state().selected, 'house_1s_2r_a');
  editor.useTool('rotate');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), ['setPose', { x: 10, y: 20, z: 5 }, { x: 0, y: 0, z: 15 }]);
  editor.useTool('move');
  editor.gridStep = 0;
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), ['setPose', { x: 10.4, y: 19.8, z: 5 }, { x: 0, y: 0, z: 15 }]);
  editor.undo();
  assert.deepEqual(lastCall(calls), ['setPose', { x: 10, y: 20, z: 5 }, { x: 0, y: 0, z: 15 }]);

  // Location X is the details' second row; its + end steps it by the grid (0.5 m with the grid
  // off).
  editor.pressDetail(1, 1);
  assert.deepEqual(lastCall(calls), ['setPose', { x: 10.5, y: 20, z: 5 }, { x: 0, y: 0, z: 15 }]);
  editor.deleteSelected();
  assert.equal(editor.placed.length, 0);
  editor.undo();
  assert.equal(editor.placed.length, 1);
});

test('Terrain mode: the brush tiles choose the brush, the details size it', () => {
  const { editor, calls } = setup();
  editor.useMode('terrain');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), ['raise', 10.4, 19.8, 6, 1, 6]);
  editor.pressTile(1);
  editor.pressDetail(2, 1);
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), ['raise', 10.4, 19.8, 7, -1, 7]);
  editor.undo();
  assert.deepEqual(lastCall(calls), ['undo', 'raise']);
});

test('Zones places areas that block nothing, with their labels, kept as Area.create definitions', () => {
  const { editor, calls } = setup();
  assert.equal(editor.useMode('zones'), true);
  assert.deepEqual(
    editor.items().map((item) => item.name),
    ['Safe zone', 'Spawn area', 'Trigger area', 'Private ground'],
  );
  editor.pressTile(0);
  editor.pressWorld(VIEW_X, VIEW_Y);
  // No collision box: a zone is only drawn.
  assert.equal(calls.filter((call) => call[0] === 'box').length, 0);
  assert.equal(editor.collision.shapes.length, 1);
  const rows = editor.details().rows;
  assert.equal(rows.find((row) => row.label === 'Labels').value, 'safe');
  const area = editor.project().areas[0];
  assert.equal(area.type, 'box');
  assert.deepEqual(area.labels, ['safe']);
  assert.deepEqual(area.max, { x: 6, y: 6, z: 2.5 });
  assert.deepEqual(
    editor
      .outlinerLines()
      .filter((line) => line.group)
      .map((line) => line.name)
      .at(-1),
    'Zones',
  );
});

test('Foliage plants the chosen kind; Move plants it again at the new place', () => {
  const { editor, calls } = setup();
  assert.equal(editor.choose('objects/natural/trees/oak/oak_a.cgf'), true);
  assert.equal(editor.mode, 'foliage');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), ['plant', 'objects/natural/trees/oak/oak_a.cgf', 10, 20, 0]);
  editor.useTool('move');
  editor.pressDetail(2, -1);
  assert.deepEqual(calls.slice(-2), [
    ['remove', 'plant'],
    ['plant', 'objects/natural/trees/oak/oak_a.cgf', 10, 19, 0],
  ]);
});

test('Collision mode adds a wall, sizes, scales and deletes it, all undoable', () => {
  const { editor, calls } = setup();
  editor.choose('wall');
  assert.equal(editor.mode, 'collision');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(calls.find((call) => call[0] === 'box').slice(1), [10, 20, 6.5, 6, 0.3, 3]);
  assert.ok(calls.some((call) => call[0] === 'gizmo' && call[1] === 'architect-collision-1'));
  assert.equal(editor.state().selected, 'Invisible wall 1');

  // Height is the details' ninth row (TRANSFORM, X, Y, Z, Rotation, SIZE, Width, Depth, Height).
  editor.pressDetail(8, 1);
  assert.deepEqual(calls.at(-2), ['setBox', 10, 20, 6.75, 6, 0.3, 3.5]);
  editor.useTool('scale');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(calls.at(-2), ['setBox', 10, 20, 7, 6.5, 0.4, 4]);
  editor.undo();
  editor.undo();
  assert.deepEqual(calls.at(-2), ['setBox', 10, 20, 6.5, 6, 0.3, 3]);
  editor.deleteSelected();
  assert.equal(editor.collision.shapes.length, 0);
  editor.undo();
  assert.equal(editor.collision.shapes.length, 1);
  editor.useMode('place');
  assert.ok(calls.some((call) => call[0] === 'gizmoRemove' && call[1] === 'architect-collision-1'));
});

test('the right mouse button looks around; shortcuts change tools and modes', () => {
  const { editor, calls, down, tick } = setup();
  editor.open();
  assert.deepEqual(calls[0], ['gameUi', false]);
  down.add('mouse2');
  tick();
  assert.deepEqual(calls.slice(-2), [['look', true], ['blur']]);
  down.add('e');
  tick();
  assert.equal(editor.tool, 'select');
  down.clear();
  tick();
  assert.deepEqual(calls.slice(-2), [['look', false], ['focus']]);
  down.add('e');
  tick();
  assert.equal(editor.tool, 'rotate');
  down.add('4');
  tick();
  assert.equal(editor.mode, 'collision');
  editor.close();
  assert.deepEqual(lastCall(calls), ['gameUi', true]);
});

test('scripts drive the editor through WorldArchitect', () => {
  const { editor, calls } = setup();
  const facade = architectFacade(editor);
  assert.equal(facade.pressAt(VIEW_X, VIEW_Y), false);
  assert.equal(facade.open(), true);
  assert.equal(facade.choose('door/door_village_left'), true);
  assert.equal(facade.choose('no/such/thing'), false);
  assert.equal(facade.state().chosen, 'door/door_village_left');
  assert.equal(facade.pressAt(VIEW_X, VIEW_Y), true);
  assert.deepEqual(lastCall(calls), ['prefab', 'door/door_village_left', 10, 20, 5, 0]);
  assert.equal(facade.state().placed, 1);
  assert.equal(facade.undo(), 0);
  assert.equal(facade.redo(), 1);
  assert.equal(facade.useMode('terrain'), true);
  assert.equal(facade.useTool('rotate'), true);
  assert.equal(facade.useTool('paint'), false);
  assert.equal(facade.state().mode, 'terrain');
});

test('search boxes take typed keys; the outliner filters and folds its groups', () => {
  const { editor, down, tick } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.choose('wall');
  editor.pressWorld(VIEW_X, VIEW_Y + 200);
  // Nine groups (Buildings, Foliage, Props, Effects, Lights, Sounds, People, Collision, Hidden)
  // and two things.
  assert.equal(editor.outlinerLines().length, 11);

  editor.startTyping('outliner');
  for (const key of ['w', 'a', 'l', 'l']) {
    down.add(key);
    tick();
    down.delete(key);
    tick();
  }
  assert.equal(editor.outlinerQuery, 'wall');
  assert.equal(editor.tool, 'select');
  assert.deepEqual(
    editor
      .outlinerLines()
      .filter((line) => line.thing)
      .map((line) => line.name),
    ['Invisible wall 1'],
  );
  down.add('enter');
  tick();
  assert.equal(editor.typing, null);

  editor.outlinerQuery = '';
  editor.turnOutlinerFilter();
  assert.deepEqual(
    editor.outlinerLines().map((line) => line.name),
    ['Buildings', 'house_1s_2r_a'],
  );
  editor.pressOutliner(0);
  assert.deepEqual(
    editor.outlinerLines().map((line) => line.name),
    ['Buildings'],
  );

  editor.useMode('place');
  editor.contentQuery = 'zzz';
  assert.equal(editor.items().length, 0);
});

test("Select picks the level's own tree; hiding it is a local level edit, undone by showing it", () => {
  const { editor, calls, world } = setup();
  world.hit = {
    position: { x: 50, y: 60, z: 7 },
    kind: 'vegetation',
    levelObject: {
      kind: 'brush',
      name: 'objects/natural/trees/oak/oak_a.cgf',
      at: { x: 50.2, y: 59.9, z: 6.5 },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.state().selected, 'oak_a');
  assert.match(editor.details().kind, /from the game/);
  editor.deleteSelected();
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  assert.deepEqual(sent[0], {
    op: 'localLevelEdit',
    kind: 'set',
    id: 1,
    object: 'brush',
    name: 'objects/natural/trees/oak/oak_a.cgf',
    guid: '',
    at: { x: 50.2, y: 59.9, z: 6.5 },
    hidden: true,
  });
  assert.equal(editor.outlinerLines().at(-1).name, 'oak_a');
  // A level plant also takes the grass carpet's clutter at it (sticks drawn in the carpet).
  assert.deepEqual(sent[1], {
    op: 'architectGrass',
    kind: 'hide',
    id: 1,
    x: 50.2,
    y: 59.9,
    radius: 0.6,
  });
  editor.undo();
  const shown = calls.slice(-2).map((call) => call[1]);
  assert.deepEqual(shown, [
    { op: 'localLevelEdit', kind: 'remove', id: 1 },
    { op: 'architectGrass', kind: 'show', id: 1 },
  ]);
  // Moved, a level object is hidden and your own copy stands at the new place, one step.
  editor.useTool('move');
  const steps = editor.undoStack.length;
  editor.moveSelected({ x: 55, y: 60, z: 6.5 }, 0);
  assert.equal(editor.undoStack.length, steps + 1);
  assert.equal(editor.placed.length, 1);
  assert.equal(editor.placed[0].entry.name, 'objects/natural/trees/oak/oak_a.cgf');
  assert.match(editor.message, /your copy stands there/);
  editor.undo();
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.hiddenLevel.length, 0);
});

test('a level door opens and closes from Details; a gate near it steps through its states', () => {
  const { editor, calls, world } = setup();
  const asked = [];
  editor.api.doorState = (guid) => {
    asked.push(guid);
    return { door: true, open: false, locked: true };
  };
  editor.api.gatesNear = () => [
    { gate: 7, key: 'kutnohorsko/gates/series_gate_west', state: -1, states: ['closed', 'open'] },
  ];
  world.hit = {
    position: { x: 80, y: 90, z: 3 },
    kind: 'entity',
    levelObject: {
      kind: 'entity',
      name: 'door_village_left',
      class: 'AnimDoor',
      guid: '4cede3c2b332b298',
      at: { x: 80, y: 90, z: 3 },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  const rows = () => editor.allDetails().rows;
  const open = rows().find((row) => row.label === 'Open');
  assert.equal(open.toggle, false);
  assert.equal(rows().find((row) => row.label === 'Locked in the level').value, 'yes');
  open.press();
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  assert.deepEqual(sent.at(-1), { op: 'localDoor', guid: '4cede3c2b332b298', open: true });
  assert.equal(rows().find((row) => row.label === 'Open').toggle, true);
  // The door's state is asked once a selection.
  assert.deepEqual(asked, ['4cede3c2b332b298']);

  const gate = rows().find((row) => row.label === 'series gate west');
  assert.equal(gate.value, "the level's");
  gate.step(1);
  assert.deepEqual(calls.at(-1)[1], { op: 'localGate', gate: 7, state: 1 });
  assert.equal(rows().find((row) => row.label === 'series gate west').value, 'open');
});

test('a level entity (a door) moves where it stands by its level edit; Undo puts it back', () => {
  const { editor, calls, world } = setup();
  world.hit = {
    position: { x: 80, y: 90, z: 3 },
    kind: 'entity',
    levelObject: {
      kind: 'entity',
      name: 'door_village_left32',
      class: 'AnimDoor',
      guid: '77d417c28be44afd',
      at: { x: 80, y: 90, z: 3 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  const door = editor.selected;
  editor.moveSelected({ x: 82, y: 90, z: 3 }, 90);
  const sent = () => calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  const edit = sent().at(-1);
  assert.equal(edit.op, 'localLevelEdit');
  assert.equal(edit.object, 'entity');
  assert.equal(edit.guid, '77d417c28be44afd');
  assert.deepEqual(edit.position, { x: 82, y: 90, z: 3 });
  assert.ok(Math.abs(edit.rotation.z - Math.SQRT1_2) < 1e-9);
  // No copy: the door itself moved, and the project keeps it.
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.project().moved.length, 1);
  assert.equal(editor.selected, door);
  editor.undo();
  assert.deepEqual(sent().at(-1), { op: 'localLevelEdit', kind: 'remove', id: edit.id });
  assert.equal(editor.project().moved.length, 0);
});

test("Lower takes your plant and the level's loose stone down with the ground; Undo brings them up", () => {
  const { editor, calls } = setup();
  editor.open();
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const plant = editor.placed[0];
  let ground = 5;
  editor.api.groundHeights = (points) => points.map(() => ground);
  editor.api.levelBrushes = () => [
    { mesh: 'objects/natural/rocks/stone_a.cgf', at: { x: plant.at.x + 1, y: plant.at.y, z: 5 } },
  ];
  editor.api.Terrain.raise = (at, radius, amount) => {
    ground += amount;
    return { undo: () => (ground -= amount) };
  };
  const startZ = plant.at.z;
  editor.useMode('terrain');
  editor.terrainMode = 'lower';
  editor.brushStrength = 1;
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(plant.at.z, startZ - 1);
  const stone = calls
    .filter((call) => call[0] === 'send' && call[1].op === 'localLevelEdit')
    .at(-1)[1];
  assert.equal(stone.object, 'brush');
  assert.equal(stone.position.z, 4);
  assert.equal(editor.project().grounded.length, 1);
  editor.undo();
  assert.equal(plant.at.z, startZ);
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1], {
    op: 'localLevelEdit',
    kind: 'remove',
    id: stone.id,
  });
  assert.equal(editor.project().grounded.length, 0);
});

test('Play as player keeps the screen hidden, its overlay cleared; back in the editor it is shown again, not built anew', () => {
  const { editor } = setup();
  let made = 0;
  const create = editor.api.NativeUI.createScreen;
  editor.api.NativeUI.createScreen = () => {
    made += 1;
    return create();
  };
  editor.open();
  const screen = editor.screen;
  // The panels' text and pictures are the game client's overlay: cleared while playing.
  let cleared = 0;
  editor.shell = {
    render() {},
    clear() {
      cleared += 1;
    },
  };
  editor.close({ park: true });
  assert.equal(editor.screen, null);
  assert.equal(cleared, 1);
  editor.open();
  assert.equal(editor.screen, screen);
  assert.equal(made, 1);
  // A map switch closes it for good: the overlay is cleared too, and the next opening builds a
  // new one.
  editor.close();
  assert.equal(cleared, 2);
  editor.open();
  assert.equal(made, 2);
});

test("the AI's close is Play as player: the editor is kept for the way back", () => {
  const { editor } = setup();
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  assert.equal(ask(1, 'open').ok, true);
  assert.equal(ask(2, 'close').ok, true);
  assert.equal(editor.isOpen(), false);
  assert.ok(editor.parked);
  assert.equal(ask(3, 'open').ok, true);
  assert.equal(editor.isOpen(), true);
});

test("a mark's line is drawn only where the world view shows it, not over the panels", () => {
  const { editor } = setup();
  editor.open();
  // A camera looking down: a world point (x, y) shows at stage (960 + 10 x, 540 - 10 y).
  editor.api.NoClip.toScreen = (point) => ({ x: (point.x * 10) / 960, y: (point.y * 10) / 540 });
  const line = { from: { x: 0, y: 0, z: 0 }, to: { x: 200, y: 0, z: 0 }, color: 1, width: 2 };
  const parts = editor.inView(line);
  assert.equal(parts.length, 1);
  assert.equal(parts[0].from.x, 0);
  const right = editor.stagePoint(parts[0].to).x;
  // The world view's right edge (theme.cjs LAYOUT.view: x 326, width 1210).
  assert.ok(Math.abs(right - 1536) < 1, `cut at ${right}`);
});

test("Scale grows a level object where it stands by its level edit; Undo gives back the level's size", () => {
  const { editor, calls, world } = setup();
  world.hit = {
    position: { x: 60, y: 70, z: 4 },
    kind: 'brush',
    levelObject: {
      kind: 'brush',
      name: 'objects/manmade/fences/fence_a.cgf',
      at: { x: 60, y: 70, z: 4 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
      scale: { x: 2, y: 2, z: 2 },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.useTool('scale');
  editor.useToolAt({ x: 60, y: 70, z: 4 });
  const edit = calls.filter((call) => call[0] === 'send').at(-1)[1];
  assert.equal(edit.object, 'brush');
  assert.equal(edit.name, 'objects/manmade/fences/fence_a.cgf');
  // The level's own size (2) times one step (1.1).
  assert.ok(Math.abs(edit.scale - 2.2) < 1e-9);
  assert.equal(edit.position, undefined);
  assert.equal(editor.project().moved[0].scale, 1.1);
  editor.undo();
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1], {
    op: 'localLevelEdit',
    kind: 'remove',
    id: edit.id,
  });
  assert.equal(editor.project().moved.length, 0);
});

test("your own models are in Props' Yours folder; a prop takes one of your materials, one undo step", () => {
  const { editor, calls } = setup();
  editor.api.propMeshes = () => ['objects/manmade/common_furniture/barrels/barrel_a.cgf'];
  editor.api.userAssets = () => ({
    folder: 'C:UsersmeDocumentsKCDSWorld Architectassets',
    meshes: ['objects/kcds/mine/stall.cgf'],
    materials: ['objects/kcds/mine/red_wood'],
    refused: [],
  });
  editor.open();
  editor.useMode('props');
  editor.pressFolder(7);
  // The textures drawn by code come first (New stone blocks ...), then your models.
  const names = editor.items().map((item) => item.name);
  assert.equal(names.at(-1), 'stall');
  assert.equal(names[0], 'New stone blocks');
  editor.pressTile(names.length - 1);
  editor.pressWorld(VIEW_X, VIEW_Y);
  const prop = editor.placed.at(-1);
  assert.equal(prop.entry.name, 'objects/kcds/mine/stall.cgf');
  const material = () => editor.details().rows.find((row) => row.label === 'Material');
  assert.equal(material().value, 'Its own');
  material().step(1);
  assert.equal(prop.material, 'objects/kcds/mine/red_wood');
  const sent = calls.filter((call) => call[0] === 'send' && call[1].op === 'localProp');
  assert.equal(sent.at(-1)[1].material, 'objects/kcds/mine/red_wood');
  assert.equal(editor.project().props.at(-1).material, 'objects/kcds/mine/red_wood');
  editor.undo();
  assert.equal(prop.material, null);
});

test('an AI assistant lists your assets and draws a prop with your material', () => {
  const { editor } = setup();
  let looked = 0;
  editor.api.userAssets = () => {
    looked += 1;
    return {
      folder: 'C:/assets',
      meshes: ['objects/kcds/mine/stall.cgf'],
      materials: looked > 1 ? ['objects/kcds/mine/red_wood'] : [],
      refused: [],
    };
  };
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  ask(1, 'open');
  // Asked again, the folder is looked at again: a material made since is there.
  assert.deepEqual(ask(2, 'assets').result.materials, []);
  assert.deepEqual(ask(7, 'assets').result.materials, ['objects/kcds/mine/red_wood']);
  // A model made while the editor is open is placed by its name once assets looked again.
  editor.catalogue.entries('props');
  const before = editor.api.userAssets;
  editor.api.userAssets = () => ({
    ...before(),
    meshes: ['objects/kcds/mine/stall.cgf', 'objects/kcds/mine/new.cgf'],
  });
  assert.equal(
    ask(8, 'place', { name: 'objects/kcds/mine/new.cgf', at: { x: 1, y: 2, z: 3 } }).ok,
    false,
  );
  ask(9, 'assets');
  assert.equal(
    ask(10, 'place', { name: 'objects/kcds/mine/new.cgf', at: { x: 1, y: 2, z: 3 } }).ok,
    true,
  );
  const prop = ask(3, 'place', { name: 'objects/kcds/mine/stall.cgf', at: { x: 1, y: 2, z: 3 } });
  assert.equal(prop.ok, true, prop.error);
  const drawn = ask(4, 'set_material', {
    id: prop.result.id,
    material: 'objects/kcds/mine/red_wood',
  });
  assert.equal(drawn.ok, true, drawn.error);
  assert.equal(editor.placed.at(-1).material, 'objects/kcds/mine/red_wood');
  assert.match(
    ask(5, 'set_material', { id: prop.result.id, material: 'objects/kcds/x' }).error,
    /no material/,
  );
  ask(6, 'undo');
  assert.equal(editor.placed.at(-1).material, null);
});

test('Export for Blender writes the ground, the scene and the import script into one folder', () => {
  const { editor, calls } = setup();
  const written = [];
  editor.api.groundHeights = (points) => points.map(() => 5);
  editor.api.exportFiles = (folder, files) => {
    written.push({ folder, files });
    return `C:\exports\${folder}`;
  };
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const answer = editor.exportForBlender();
  assert.equal(answer.things, 1);
  // Each file in pieces (a message holds 64 KiB), joined again here.
  const files = {};
  for (const call of written) {
    for (const file of call.files) {
      files[file.name] = file.append ? files[file.name] + file.text : file.text;
    }
  }
  assert.deepEqual(Object.keys(files), ['ground.obj', 'scene.json', 'import_kcds.py']);
  const scene = JSON.parse(files['scene.json']);
  assert.equal(scene.things[0].model, 'structures/living/houses/house_1s_2r_a');
  assert.equal(scene.things[0].kind, 'prefab');
  // The house stands 20 m in from the area's corner (the work and 20 m around it).
  assert.deepEqual(scene.things[0].at, { x: 20, y: 20, z: 5 });
  assert.match(files['ground.obj'], /^v 0 0 5$/m);
  assert.match(files['import_kcds.py'], /bpy\.ops\.wm\.obj_import/);
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1], {
    op: 'architectCopy',
    text: answer.folder,
  });
});

test("a level entity's properties are in Details: a bool toggles, a number steps, a text shows", () => {
  const { editor, calls, world } = setup();
  editor.api.entityProperties = () => [
    { key: 'Lock.bLocked', type: 'bool', value: true },
    { key: 'fRadius', type: 'number', value: 8 },
    { key: 'object_Model', type: 'text', value: 'objects/doors/door_a.cgf' },
  ];
  world.hit = {
    position: { x: 80, y: 90, z: 3 },
    kind: 'entity',
    levelObject: {
      kind: 'entity',
      name: 'lamp_1',
      class: 'Light',
      guid: '0123456789abcdef',
      at: { x: 80, y: 90, z: 3 },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  const row = (label) => editor.allDetails().rows.find((entry) => entry.label === label);
  assert.ok(row('PROPERTIES'));
  assert.equal(row('Lock.bLocked').toggle, true);
  row('Lock.bLocked').press();
  const sent = () => calls.filter((call) => call[0] === 'send').at(-1)[1];
  assert.deepEqual(sent(), {
    op: 'localEntityProperty',
    guid: '0123456789abcdef',
    key: 'Lock.bLocked',
    value: false,
  });
  row('fRadius').step(1);
  assert.equal(sent().value, 9);
  assert.equal(row('object_Model').value, 'objects/doors/door_a.cgf');
});

test('a project larger than a message is saved in parts and comes back from parts', () => {
  const { editor, calls } = setup();
  editor.open();
  for (let index = 0; index < 400; index += 1) {
    editor.placed.push({
      entry: { label: 'oak_a', name: 'objects/natural/trees/oak/oak_a.cgf', source: 'vegetation' },
      at: { x: index, y: index, z: 1 },
      turn: 0,
      handle: null,
      pad: null,
    });
  }
  editor.save();
  const saves = calls
    .filter((call) => call[0] === 'send' && call[1].op === 'architectSave')
    .map((call) => call[1]);
  assert.ok(saves.length > 1);
  assert.ok(saves.every((save) => save.data.length <= 20000 && save.parts === saves.length));
  const whole = saves.map((save) => save.data).join('');
  assert.equal(JSON.parse(whole).plants.length, 400);
  // Back from the game in parts, joined before it is read.
  saves.forEach((save, index) => editor.projectKept('trosecko', save.data, index, saves.length));
  // Whole again (built once the editor's screen stands).
  assert.equal(editor.keptProject.plants.length, 400);
});

test('a panel drawing longer than a draw call takes goes in parts, the first clearing the clip', () => {
  const drawn = [];
  const clip = {
    draw(commands) {
      assert.ok(commands.length <= 1024);
      drawn.push(commands);
      return true;
    },
  };
  const box = [
    ['beginFill', 0, 100],
    ['moveTo', 0, 0],
    ['lineTo', 1, 0],
    ['lineTo', 1, 1],
    ['endFill'],
  ];
  const commands = [];
  for (let index = 0; index < 500; index += 1) {
    commands.push(...box);
  }
  Shell.prototype.paint.call({ shown: new Map() }, clip, commands);
  assert.ok(drawn.length >= 3);
  assert.deepEqual(drawn[0][0], ['clear']);
  assert.ok(drawn.slice(1).every((part) => part[0][0] === 'beginFill'));
  assert.equal(drawn.flat().length, commands.length + 1);
});

test('Export for server writes a resource whose scripts make the work on the server and each client', () => {
  const vm = require('node:vm');
  const { editor, world } = setup();
  const written = {};
  editor.api.exportFiles = (folder, files) => {
    for (const file of files) {
      written[file.name] = file.append ? written[file.name] + file.text : file.text;
    }
    return `C:\exports\${folder}`;
  };
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y + 300);
  world.hit = {
    position: { x: 50, y: 60, z: 7 },
    kind: 'brush',
    levelObject: { kind: 'brush', name: 'objects/fences/fence_a.cgf', at: { x: 50, y: 60, z: 7 } },
  };
  editor.select(null);
  editor.useTool('select');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.deleteSelected();
  const answer = editor.exportForServer();
  assert.equal(answer.buildings, 1);
  assert.equal(answer.plants, 1);
  assert.equal(answer.levelEdits, 1);
  assert.deepEqual(Object.keys(written).sort(), [
    'README.txt',
    'client.js',
    'package.json',
    'server.js',
    'work.json',
  ]);
  assert.equal(JSON.parse(written['package.json']).name, 'architect_trosecko');
  // The scripts run against stand-ins of the SDK.
  const calls = [];
  const recorder =
    (name) =>
    (...args) =>
      calls.push([name, ...args]);
  const listeners = {};
  const kcds = {
    world: {
      Prop: { spawn: recorder('Prop.spawn') },
      LevelEdit: { apply: recorder('LevelEdit.apply') },
      Area: { create: recorder('Area.create') },
      Vfx: { spawn: recorder('Vfx.spawn') },
      Light: { spawn: recorder('Light.spawn') },
      Terrain: {
        flatten: recorder('Terrain.flatten'),
        raise: recorder('Terrain.raise'),
        paint: recorder('Terrain.paint'),
      },
      Vegetation: {
        plantMany: (list) => {
          calls.push(['Vegetation.plantMany', list]);
          return list.map(() => ({}));
        },
      },
      Collision: { addBox: recorder('Collision.addBox') },
    },
    characters: { Npc: { create: recorder('Npc.create') } },
    math: { Vector3: Vector3 },
    core: { Events: { on: (name, handler) => (listeners[name] = handler) } },
  };
  const run = (source) =>
    vm.runInNewContext(source, {
      kcds,
      console: { info() {}, warn() {} },
      require: (path) => (path === './work.json' ? JSON.parse(written['work.json']) : null),
    });
  run(written['server.js']);
  assert.deepEqual(
    calls.map((call) => call[0]),
    ['Prop.spawn', 'LevelEdit.apply'],
  );
  assert.equal(calls[0][1], 'prefab:structures/living/houses/house_1s_2r_a');
  assert.equal(calls[1][1].hidden, true);
  // No spawn area on the game's own map: the server's own spawn stands.
  assert.equal(listeners.playerSpawning, undefined);
  run(written['client.js']);
  listeners.playerReady();
  // The plants many to a question: the one plant in one call.
  const planted = calls.filter((call) => call[0] === 'Vegetation.plantMany');
  assert.equal(planted.length, 1);
  assert.equal(planted[0][1].length, 1);
  // The fitted house's pad.
  assert.ok(calls.some((call) => call[0] === 'Terrain.flatten'));
});

test('an exported server lets players appear in the spawn areas, on a custom map under the camera', () => {
  const vm = require('node:vm');
  const { editor, world } = setup();
  // The custom maps the export asked the game client to copy in.
  const maps = [];
  const exported = () => {
    const written = {};
    editor.api.exportFiles = (folder, files, map) => {
      if (map) {
        maps.push(map);
      }
      for (const file of files) {
        written[file.name] = file.append ? written[file.name] + file.text : file.text;
      }
      return `C:\exports\${folder}`;
    };
    editor.exportForServer();
    const listeners = {};
    const spawned = [];
    const ignored = () => {};
    vm.runInNewContext(written['server.js'], {
      kcds: {
        world: {
          Prop: { spawn: ignored },
          LevelEdit: { apply: ignored },
          Area: { create: ignored },
          Vfx: { spawn: ignored },
          Light: { spawn: ignored },
        },
        characters: { Npc: { create() {} } },
        math: { Vector3: Vector3 },
        core: { Events: { on: (name, handler) => (listeners[name] = handler) } },
      },
      console: { info() {}, warn() {} },
      require: () => JSON.parse(written['work.json']),
    });
    const player = { spawn: (at, rotation) => spawned.push([at.x, at.y, at.z, rotation.z]) };
    listeners.playerSpawning?.(player);
    listeners.playerSpawning?.(player);
    return spawned;
  };
  // A custom map has no spawn point of its own: the ground under the camera (0, 0), lifted.
  editor.map = 'kcds_map_prague';
  world.hit = { position: { x: 0, y: 0, z: 32 }, levelObject: null };
  assert.deepEqual(exported(), [
    [0, 0, 32.5, 0],
    [0, 0, 32.5, 0],
  ]);
  // The map itself goes with the work.
  assert.deepEqual(maps, ['kcds_map_prague']);
  // A spawn area wins, on any map.
  editor.map = 'trosecko';
  world.hit = { position: { x: 10.4, y: 19.8, z: 5 }, levelObject: null };
  editor.useMode('zones');
  editor.pressTile(1);
  editor.pressWorld(VIEW_X, VIEW_Y);
  const spawned = exported();
  assert.equal(spawned.length, 2);
  assert.deepEqual(spawned[0].slice(0, 3), spawned[1].slice(0, 3));
  assert.equal(spawned[0][2], 5.5);
  // The game's own map stays out of the export.
  assert.deepEqual(maps, ['kcds_map_prague']);
});

test('one press without Shift places one and goes back to Select', () => {
  const { editor } = setup();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.placed.length, 1);
  assert.equal(editor.state().chosen, null);
  assert.equal(editor.tool, 'select');
});

test('a selection is marked with its box and the X, Y and Z arrows', () => {
  const { editor, calls } = setup();
  editor.shell = { render() {}, clear() {} };
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  // The box's 12 edges and the X, Y and Z arrows, each a stroke and a two-stroke head.
  assert.equal(editor.marks.lines().length, 12 + 3 * 3);
  assert.deepEqual(
    editor.marks.arrows.map((arrow) => arrow.axis),
    ['x', 'y', 'z'],
  );
  // Drawn as the game's world drawings (they stay on the thing while the camera moves).
  const marks = (call) => String(call[1]).startsWith('architect-mark-');
  assert.ok(calls.some((call) => call[0] === 'gizmo' && marks(call)));
  editor.input('kcds_ui_back', 'press');
  assert.equal(editor.marks.lines().length, 0);
  assert.equal(calls.filter((call) => call[0] === 'gizmo' && !marks(call)).length, 0);
  assert.ok(calls.some((call) => call[0] === 'gizmoRemove' && marks(call)));
});

test('F focuses the camera on the selection; Ctrl+D duplicates it beside it, undoable', () => {
  const { editor, calls } = setup();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.focusSelected();
  assert.deepEqual(lastCall(calls)[0], 'focus');
  editor.duplicateSelected();
  assert.equal(editor.placed.length, 2);
  assert.equal(editor.placed[1].at.x, 19);
  editor.undo();
  assert.equal(editor.placed.length, 1);

  editor.choose('wall');
  editor.pressWorld(VIEW_X, VIEW_Y + 300);
  editor.duplicateSelected();
  assert.equal(editor.collision.shapes.length, 2);
  assert.equal(editor.collision.shapes[1].ground.x, 10 + 7);
});

test('E while placing turns the item by the angle snap; otherwise it is the Rotate tool', () => {
  const { editor, calls } = setup();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.shortcut('e', false);
  assert.equal(editor.turn, 15);
  assert.equal(editor.tool, 'select');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.deepEqual(lastCall(calls), [
    'prefab',
    'structures/living/houses/house_1s_2r_a',
    10,
    20,
    5,
    15,
  ]);
  editor.shortcut('e', false);
  assert.equal(editor.tool, 'rotate');
});

test('Validation finds overlapping buildings and plants inside them; a press goes to one', () => {
  const { editor, calls, down } = setup();
  down.add('shift');
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const issues = editor.validate();
  assert.equal(issues.filter((issue) => issue.level === 'Warning').length, 2);
  assert.match(issues[0].text, /Two buildings overlap/);
  editor.pressIssue(0);
  assert.equal(lastCall(calls)[0], 'focus');
  assert.equal(editor.issuesView().issues.length, 2);
});

test('Validation passes an empty world', () => {
  const { editor } = setup();
  assert.deepEqual(editor.validate(), []);
  assert.equal(editor.issuesView().issues[0].level, 'Passed');
});

test('the project is saved after a change and built again when the editor opens', () => {
  const first = setup();
  const { editor, calls, saves } = first;
  editor.shell = { render() {}, clear() {} };
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.useMode('terrain');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.choose('wall');
  editor.pressWorld(VIEW_X, VIEW_Y + 300);
  assert.equal(editor.unsaved, 3);
  assert.equal(saves.length, 1);
  saves[0]();
  const saved = calls.filter((call) => call[0] === 'send' && call[1].op === 'architectSave');
  assert.equal(saved.length, 1);
  assert.equal(editor.unsaved, 0);
  const project = JSON.parse(saved[0][1].data);
  assert.equal(project.buildings.length, 1);
  assert.equal(project.buildings[0].fit, true);
  // The pad under the house is the building's own (fit), not a ground edit.
  assert.equal(project.terrain.length, 1);
  assert.equal(project.shapes[0].kind, 'wall');

  const second = setup();
  second.editor.shell = { render() {}, clear() {} };
  second.editor.projectKept('trosecko', saved[0][1].data);
  assert.equal(second.editor.placed.length, 1);
  assert.equal(second.editor.collision.shapes.length, 1);
  assert.equal(second.editor.terrainEdits.length, 1);
  assert.equal(second.editor.undoStack.length, 0);
  assert.ok(second.calls.some((call) => call[0] === 'prefab'));
});

test('the worker reaches the kept project and pictures through the facade', () => {
  const { editor } = setup();
  const facade = architectFacade(editor);
  assert.equal(typeof facade.projectKept, 'function');
  assert.equal(typeof facade.picturesKept, 'function');
  facade.projectKept('kutnohorsko', '');
  assert.equal(editor.map, 'kutnohorsko');
});

test('the paint brush scatters the plant around the press, one undo step for all', () => {
  const { editor, calls } = setup();
  editor.random = () => 0.5;
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.paintCount = 5;
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(calls.filter((call) => call[0] === 'plant').length, 5);
  assert.equal(editor.placed.length, 5);
  assert.equal(editor.undoStack.length, 1);
  assert.equal(editor.state().chosen, 'objects/natural/trees/oak/oak_a.cgf');
  editor.undo();
  assert.equal(editor.placed.length, 0);
  assert.equal(calls.filter((call) => call[0] === 'remove').length, 5);
  const rows = editor.details().rows.map((row) => row.label);
  assert.ok(rows.includes('Plants a press'));
});

test('the WORLD rows set the time of day and the weather, and the project keeps them', () => {
  const { editor, calls } = setup();
  const rows = () => editor.details().rows;
  const row = (label) => rows().findIndex((entry) => entry.label === label);
  editor.pressDetail(row('Time of day'), 1);
  editor.pressDetail(row('Weather'), -1);
  const look = calls
    .filter((call) => call[0] === 'send')
    .map((call) => call[1])
    .filter((message) => message.op === 'architectWorld')
    .at(-1);
  assert.equal(look.hour, 12.5);
  assert.equal(look.weather, 'foggy_storm');
  assert.equal(look.speed, 0);
  assert.equal(rows()[row('Time of day')].value, '12:30');
  assert.deepEqual(editor.project().look, {
    hour: 12.5,
    clock: 0,
    weather: 7,
    wind: 1,
    weatherHeld: true,
  });
  // Freeze time off runs the clock (×60), on holds it; Freeze weather off gives the game's own.
  assert.equal(rows()[row('Freeze time')].toggle, true);
  rows()[row('Freeze time')].press();
  assert.equal(calls.at(-1)[1].speed, 60);
  rows()[row('Freeze weather')].press();
  assert.equal(calls.at(-1)[1].weather, '');
  assert.equal(rows()[row('Weather')].value, "The game's own");
  // WORLD is in Details with a selection too.
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.ok(editor.selected);
  assert.ok(row('Freeze weather') > 0);
  editor.restoreLook({ hour: 30, clock: 2, weather: 'x', wind: 0, weatherHeld: false });
  assert.deepEqual(editor.look, { hour: 12, weatherHeld: false, clock: 2, weather: 0, wind: 0 });
});

test('holding the button paints a terrain stroke, one undo step, under the brush ring', () => {
  const { editor, calls, down, world, tick } = setup();
  editor.open();
  editor.useMode('terrain');
  down.add('mouse1');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(calls.filter((call) => call[0] === 'raise').length, 1);
  editor.testCursor = { x: VIEW_X, y: VIEW_Y };
  world.hit = { position: { x: 20, y: 19.8, z: 5 }, levelObject: null };
  editor.stroke.last = 0;
  tick();
  assert.equal(calls.filter((call) => call[0] === 'raise').length, 2);
  assert.equal(editor.ringPoints.length, 64);
  down.delete('mouse1');
  tick();
  assert.equal(editor.stroke, null);
  assert.equal(editor.undoStack.length, 1);
  editor.undo();
  assert.equal(calls.filter((call) => call[0] === 'undo' && call[1] === 'raise').length, 2);
  assert.equal(editor.terrainEdits.length, 0);
});

test('a press through a prop of yours takes the prop, not the level object behind it', () => {
  const { editor } = setup();
  editor.open();
  // A doorway of yours at (5, 0) that the ray does not meet; the camera is at (0, 0, 100).
  const door = {
    entry: { source: 'prop', name: 'objects/doors/doorway.cgf', label: 'doorway' },
    at: { x: 5, y: 0, z: 0 },
    turn: 0,
    scale: 1,
  };
  editor.placed.push(door);
  const wall = { kind: 'brush', name: 'objects/walls/wall.cgf', at: { x: 5.3, y: 0, z: 0 } };
  const behind = { x: 5.3, y: 0, z: 0.5 };
  assert.equal(editor.thingAt(behind, { position: behind, levelObject: wall }), door);
  // A wall met beside the doorway (its box not crossed) is the wall's.
  const beside = { x: 5, y: 3, z: 0.5 };
  const level = editor.thingAt(beside, { position: beside, levelObject: { ...wall, at: beside } });
  assert.ok(level && level.level);
  // Your own prop met behind it (a church's invisible collision): the doorway,
  // which the ray went through first.
  const walls = {
    entry: { source: 'prop', name: 'objects/kcds/opened/church_walls.cgf', label: 'church_walls' },
    at: { x: 5.3, y: 0, z: 0 },
    turn: 0,
    scale: 1,
    handle: { id: 11 },
  };
  editor.placed.push(walls);
  const met = { position: behind, entityName: 'kcds_local_prop_11', levelObject: null };
  assert.equal(editor.ownThingHit(met), walls);
  assert.equal(editor.thingAt(behind, met), door);
  // A prop this large is not taken by its box: what the ray met is.
  editor.propBoxes = new Map([
    ['objects/doors/doorway.cgf', { box: { min: [-10, -10, 0], max: [10, 10, 20] } }],
  ]);
  assert.equal(editor.thingAt(behind, met), walls);
  editor.propBoxes = new Map();
  // The camera inside the doorway's box: the press goes to what the ray met.
  editor.placed.splice(editor.placed.indexOf(walls), 1);
  editor.api.NoClip.getPosition = () => ({ x: 5, y: 0, z: 0.5 });
  const inside = editor.thingAt(behind, { position: behind, levelObject: wall });
  assert.ok(inside && inside.level);
});

test("a turned level object's box is its own, not the world's upright box around it", () => {
  const { editor } = setup();
  // A door 3 m wide and 0.2 m deep turned 30° about the up axis, standing at (100, 200, 10):
  // its upright world box is 3 cos 30 + 0.2 sin 30 by 3 sin 30 + 0.2 cos 30 wide.
  const turn = Math.PI / 6;
  const halfX = 1.5 * Math.cos(turn) + 0.1 * Math.sin(turn);
  const halfY = 1.5 * Math.sin(turn) + 0.1 * Math.cos(turn);
  const thing = {
    level: true,
    kind: 'brush',
    at: { x: 100, y: 200, z: 10 },
    box: [100 - halfX, 200 - halfY, 10, 100 + halfX, 200 + halfY, 13],
    object: {
      kind: 'brush',
      name: 'objects/doors/door.cgf',
      at: { x: 100, y: 200, z: 10 },
      rotation: { x: 0, y: 0, z: Math.sin(turn / 2), w: Math.cos(turn / 2) },
    },
  };
  const box = editor.boxOf(thing);
  const near = (a, b) => Math.abs(a - b) < 1e-6;
  assert.ok(near(box.turn, 30));
  assert.ok(near(box.min[0], -1.5) && near(box.max[0], 1.5));
  assert.ok(near(box.min[1], -0.1) && near(box.max[1], 0.1));
  assert.ok(near(box.min[2], 0) && near(box.max[2], 3));
  // Hidden, it keeps its box in the project: loaded again, the box drawn is still its own.
  editor.shell = { render() {}, clear() {} };
  editor.open();
  editor.setLevelHidden(thing, true);
  const kept = editor.project();
  assert.equal(kept.hidden[0].box.length, 6);
  const again = setup();
  again.editor.shell = { render() {}, clear() {} };
  again.editor.projectKept('trosecko', JSON.stringify(kept));
  const loaded = again.editor.hiddenLevel[0];
  assert.ok(near(again.editor.boxOf(loaded).turn, 30));
  assert.ok(near(again.editor.boxOf(loaded).max[1], 0.1));
  // Turned 45°, the sides cannot be told apart from the world box: that box, upright.
  thing.object.rotation = { x: 0, y: 0, z: Math.sin(Math.PI / 8), w: Math.cos(Math.PI / 8) };
  assert.equal(editor.boxOf(thing).turn, 0);
});

test('Play as player starts the player where the camera looks, else under the camera', () => {
  const { editor, calls, world } = setup();
  editor.api.NoClip.getRotation = () => ({ yaw: 30, pitch: -40 });
  editor.open();
  const started = () => {
    const sent = calls.filter((call) => call[0] === 'send' && call[1].op === 'architectPlayer');
    return sent.at(-1)[1];
  };
  // The camera at (0, 0, 100) looks at a point 64 m away: the player starts 0.6 m short of it,
  // on what is under it, turned as the camera.
  world.hit = { position: { x: 30, y: 40, z: 60 }, levelObject: null };
  editor.pressAction(2);
  const near = started();
  assert.equal(near.hidden, false);
  assert.ok(Math.abs(near.start.x - (30 - 30 * (0.6 / 50))) < 1e-9);
  assert.ok(Math.abs(near.start.y - (40 - 40 * (0.6 / 50))) < 1e-9);
  assert.equal(near.start.z, 60);
  assert.equal(near.start.yaw, 30);
  // Back in the editor, looking at something over 80 m away: the ground under the camera.
  editor.open();
  world.hit = { position: { x: 10.4, y: 19.8, z: 5 }, levelObject: null };
  world.hit.position.x = 90;
  editor.pressAction(2);
  assert.deepEqual(started().start, { x: 0, y: 0, z: 5, yaw: 30 });
  // No ground at all: the player stays where they are.
  editor.open();
  world.hit = null;
  editor.pressAction(2);
  assert.equal(started().start, undefined);
});

test('the map button opens a scrolling list of every map; a row chooses, a press beside closes', () => {
  const { Shell, mapListBox } = require('../../../../Client/resources/architect/shell.cjs');
  const root = {
    createClip() {
      return {
        set() {},
        draw() {
          return true;
        },
        on() {},
        get() {
          return 0;
        },
        createClip: () => root.createClip(),
      };
    },
  };
  const asked = [];
  const handlers = new Proxy(
    {
      mapChoose: (index) => asked.push(['choose', index]),
      mapPicker: (state) => asked.push(['picker', state]),
    },
    { get: (target, name) => target[name] ?? (() => {}) },
  );
  const shell = new Shell(root, handlers, {}, () => {});
  shell.build();
  shell.render = function render(view) {
    this.lastView = view;
    this.runs = [{ x: 0, y: 0, w: 4000, h: 2000, t: 'under the list' }];
    this.pictures = [];
    this.renderMenu(view.menu);
    this.renderMapList(view.menu);
  };
  // The game's three and 17 custom maps, the current one the 19th.
  const maps = Array.from({ length: 20 }, (_, index) => ({
    key: `map${index}`,
    label: index < 3 ? `Game ${index}` : `Custom: map${index}`,
    current: index === 18,
  }));
  const menu = { picker: 'choose', maps, mapLabel: 'Custom: map18', unsaved: 0, saved: 'Saved' };
  shell.render({ menu });
  const list = mapListBox(maps.length);
  assert.equal(list.rows, 12);
  // Opened with the current map in sight, the text under the list left out.
  const labels = shell.runs.map((run) => run.t);
  assert.ok(labels.includes('Custom: map18'));
  assert.ok(!labels.includes('under the list'));
  assert.ok(!labels.includes('Game 0'));
  // A row chooses its map; the list starts at the last page (20 - 12 = 8).
  const rowAt = (row) => list.y + 4 + row * 30 + 10;
  shell.routePress(list.x + 40, rowAt(0), 0);
  assert.deepEqual(asked.at(-1), ['choose', 8]);
  // The wheel scrolls up to the top, and the first row is the first map.
  assert.equal(shell.wheel(20), true);
  shell.routePress(list.x + 40, rowAt(0), 1000);
  assert.deepEqual(asked.at(-1), ['choose', 0]);
  assert.equal(shell.wheel(1), false);
  // The scroll bar's lower half pages down.
  shell.routePress(list.x + list.width - 6, list.y + list.height - 10, 2000);
  shell.routePress(list.x + 40, rowAt(0), 3000);
  assert.deepEqual(asked.at(-1), ['choose', 8]);
  // A press beside the list, in the world view, closes it.
  shell.routePress(list.x + list.width + 200, list.y + 300, 4000);
  assert.deepEqual(asked.at(-1), ['picker', null]);
  // Closed, a press there is the view's again and the list draws nothing.
  shell.render({ menu: { ...menu, picker: null } });
  assert.equal(shell.mapCount, 0);
  const before = asked.length;
  shell.routePress(list.x + 40, rowAt(0), 5000);
  assert.equal(asked.length, before);
});

test('a kept project mounts your own assets before it builds the props made of them', () => {
  const build = (props) => {
    const { editor, calls } = setup();
    editor.api.userAssets = () => {
      calls.push(['assets']);
      return { folder: 'C:/assets', meshes: [], materials: [], refused: [] };
    };
    editor.shell = { render() {}, clear() {} };
    editor.projectKept('kutnohorsko', JSON.stringify({ version: 1, map: 'kutnohorsko', props }));
    const order = calls.map((call) => (call[0] === 'send' ? call[1].op : call[0]));
    return order.filter((name) => name === 'assets' || name === 'localProp');
  };
  const at = { x: 3066, y: 665, z: 50.9 };
  // The opened church's piece: the folder first, then the prop.
  assert.deepEqual(build([{ name: 'objects/kcds/opened/stjames5_0.cgf', at, turn: 311 }]), [
    'assets',
    'localProp',
  ]);
  // The game's own model drawn with your material: the folder first too.
  assert.deepEqual(
    build([{ name: 'objects/props/barrel_a.cgf', at, turn: 0, material: 'objects/kcds/mine/red' }]),
    ['assets', 'localProp'],
  );
  // Only the game's own: the folder is not asked for.
  assert.deepEqual(build([{ name: 'objects/props/barrel_a.cgf', at, turn: 0 }]), ['localProp']);
});

test('the map switch asks first, saves the work, takes it out of the world and asks for the map', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.chooseMap('trosecko');
  assert.equal(editor.view().menu.picker, null);
  editor.chooseMap('kutnohorsko');
  assert.equal(editor.view().menu.picker, 'confirm');
  assert.match(editor.view().menu.question, /Kutnohorsko/);
  editor.switchMap();
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  const save = sent.findLastIndex((message) => message.op === 'architectSave');
  const map = sent.findIndex((message) => message.op === 'architectMap');
  assert.ok(save >= 0 && save < map);
  assert.equal(JSON.parse(sent[save].data).buildings.length, 1);
  assert.deepEqual(sent[map], { op: 'architectMap', map: 'kutnohorsko' });
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.screen, null);
});

test('Leave saves the work, takes it out of the world, closes the editor and asks to leave', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.leave();
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  const save = sent.findLastIndex((message) => message.op === 'architectSave');
  const leave = sent.findIndex((message) => message.op === 'architectLeave');
  assert.ok(save >= 0 && save < leave);
  assert.equal(JSON.parse(sent[save].data).buildings.length, 1);
  assert.deepEqual(sent.at(-1), { op: 'architectLeave' });
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.screen, null);
  assert.ok(!editor.parked, 'nothing kept for a way back');
});

test('leaving a map takes its plants away many at a time, not one question each', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.random = () => 0.5;
  editor.api.grassKinds = () => [
    { kind: 4, mesh: 'objects/natural/vegetation/grass/meadow_a.cgf', count: 300 },
  ];
  editor.api.plantMany = (list) => list.map((item, index) => ({ id: index, remove() {} }));
  editor.api.removeMany = (handles) => calls.push(['removeMany', handles.length]);
  editor.useMode('terrain');
  editor.arm({ label: 'Add nature', name: 'nature', source: 'brush' });
  editor.brushSize = 8;
  // Dabs until the map has more plants than one question takes away.
  while (editor.placed.length <= 512) {
    editor.pressWorld(VIEW_X, VIEW_Y);
  }
  const planted = editor.placed.length;
  editor.leaveWorld();
  const removals = calls.filter((call) => call[0] === 'removeMany').map((call) => call[1]);
  assert.equal(
    removals.reduce((sum, count) => sum + count, 0),
    planted,
  );
  assert.ok(removals.every((count) => count <= 512));
  assert.equal(editor.placed.length, 0);
});

test('the nature brushes clear the level plants under them and scatter the chosen plant', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.random = () => 0.5;
  editor.api.levelPlants = () => [
    { mesh: 'objects/natural/bushes/hazel/hazel_a.cgf', at: { x: 11, y: 20, z: 5 } },
    { mesh: 'objects/natural/grass/meadow_a.cgf', at: { x: 9, y: 19, z: 5 } },
  ];
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.placed.length, 1);
  editor.useMode('terrain');
  editor.arm({ label: 'Clear nature', name: 'clear', source: 'brush' });
  editor.pressWorld(VIEW_X, VIEW_Y);
  const sent = () => calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  const hides = sent().filter((m) => m.op === 'localLevelEdit' && m.kind === 'set');
  assert.equal(hides.length, 2);
  assert.equal(hides[0].object, 'brush');
  assert.equal(editor.hiddenLevel.length, 2);
  assert.equal(editor.placed.length, 0);
  editor.undo();
  assert.equal(editor.hiddenLevel.length, 0);
  assert.equal(editor.placed.length, 1);
  editor.arm({ label: 'Add nature', name: 'nature', source: 'brush' });
  editor.natureCount = 3;
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.placed.length, 4);
  assert.ok(
    editor.details().rows.some((row) => row.label === 'Where none grows' && row.value === 'oak_a'),
  );
});

test("Add nature plants the ground's own grass, densely, in one question; undo in one", () => {
  const { editor, calls } = setup();
  editor.open();
  editor.random = () => 0.5;
  const asked = [];
  editor.api.grassKinds = () => [
    { kind: 4, mesh: 'objects/natural/vegetation/grass/meadow_a.cgf', count: 300 },
    { kind: 9, mesh: 'objects/natural/vegetation/grass/clover_a.cgf', count: 100 },
  ];
  editor.api.plantMany = (list) => {
    asked.push(list);
    return list.map((item, index) => ({ id: index, remove() {} }));
  };
  editor.api.removeMany = (handles) => calls.push(['removeMany', handles.length]);
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.useMode('terrain');
  editor.arm({ label: 'Add nature', name: 'nature', source: 'brush' });
  editor.brushSize = 4;
  editor.pressWorld(VIEW_X, VIEW_Y);
  // pi * 4 m * 4 m * 1.2 a square metre: 60 clumps of the ground's grass, no oak.
  assert.equal(asked.length, 1);
  assert.equal(asked[0].length, 60);
  assert.ok(asked[0].every((item) => item.kind.includes('/grass/')));
  assert.equal(editor.placed.length, 60);
  editor.undo();
  assert.equal(editor.placed.length, 0);
  assert.deepEqual(lastCall(calls), ['removeMany', 60]);
});

test('the player is hidden in the editor, and Clear nature hides the grass carpet too', () => {
  const { editor, calls } = setup();
  const sent = () => calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  editor.open();
  assert.deepEqual(
    sent().find((m) => m.op === 'architectPlayer'),
    { op: 'architectPlayer', hidden: true },
  );
  editor.api.levelPlants = () => [];
  editor.useMode('terrain');
  editor.arm({ label: 'Clear nature', name: 'clear', source: 'brush' });
  editor.pressWorld(VIEW_X, VIEW_Y);
  const hide = sent().find((m) => m.op === 'architectGrass');
  assert.equal(hide.kind, 'hide');
  assert.equal(hide.radius, editor.brushSize);
  assert.equal(editor.project().grass.length, 1);
  editor.undo();
  assert.deepEqual(sent().at(-1), { op: 'architectGrass', kind: 'show', id: hide.id });
  assert.equal(editor.project().grass.length, 0);
  editor.close();
  assert.ok(sent().some((m) => m.op === 'architectPlayer' && m.hidden === false));
});

test('a press goes to the panel under it, whichever panel clip heard it, and only once', () => {
  const { Shell } = require('../../../../Client/resources/architect/shell.cjs');
  const clips = [];
  const root = {
    createClip() {
      const clip = {
        handlers: {},
        mouse: { _xmouse: 0, _ymouse: 0 },
        set() {},
        draw() {
          return true;
        },
        on(event, handler) {
          this.handlers[event] = handler;
        },
        get(member) {
          return this.mouse[member];
        },
        // The panels are made in the stage clip (shell.cjs placePanels).
        createClip: () => root.createClip(),
      };
      clips.push(clip);
      return clip;
    },
  };
  const tiles = [];
  const handlers = new Proxy(
    { tile: (index) => tiles.push(index) },
    { get: (target, name) => target[name] ?? (() => {}) },
  );
  const shell = new Shell(root, handlers, {}, () => {});
  shell.build();
  // The world view's clip (at 326, 84), the first panel, hears a press 900 down: the Content
  // dock's second tile.
  const view = clips.find((clip) => clip.handlers.press);
  view.mouse = { _xmouse: 190, _ymouse: 844 };
  view.handlers.press();
  view.handlers.press();
  assert.deepEqual(tiles, [1]);
});

test('dragging a move arrow moves the selection along its axis, as one step', () => {
  const { editor, calls, down, world, tick } = setup();
  editor.open();
  // A camera looking down: a world point (x, y) shows at stage (960 + 10 x, 540 - 10 y).
  editor.api.NoClip.toScreen = (point) => ({ x: (point.x * 10) / 960, y: (point.y * 10) / 540 });
  world.hit = { position: { x: 0, y: 0, z: 5 }, levelObject: null };
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.marks.select(editor.boxOf(editor.selected));
  const arrow = editor.marks.arrows.find((entry) => entry.axis === 'x');
  assert.ok(arrow);
  const steps = editor.undoStack.length;
  // Press on the X arrow half way along it, then drag 4 m further along X.
  const startX = 960 + (arrow.from.x + arrow.length / 2) * 10;
  down.add('mouse1');
  editor.pressWorld(startX, 540);
  assert.ok(editor.drag);
  editor.testCursor = { x: startX + 40, y: 540 };
  tick();
  // While held, the house itself is shown at the arrow's place (no step yet).
  assert.deepEqual(lastCall(calls), ['setPose', { x: 4, y: 0, z: 5 }, { x: 0, y: 0, z: 0 }]);
  assert.equal(editor.undoStack.length, steps);
  down.delete('mouse1');
  tick();
  assert.equal(editor.drag, null);
  assert.equal(editor.undoStack.length, steps + 1);
  assert.equal(editor.selected.at.x, 4);
  assert.ok(calls.some((call) => call[0] === 'prefab'));
});

test('a dragged building is shown by a preview of its pieces and built once, where it is let go', () => {
  const { editor, calls, down, world, tick } = setup();
  editor.open();
  editor.api.NoClip.toScreen = (point) => ({ x: (point.x * 10) / 960, y: (point.y * 10) / 540 });
  editor.api.prefabPieces = () => [
    { mesh: 'objects/wall.cgf', offset: [1, 0, 0], rotation: [0, 0, 0, 1] },
  ];
  const previews = [];
  editor.api.PropGhost = {
    editorPreview: (spec) => {
      const ghost = {
        spec,
        poses: [],
        setHidden() {},
        setPose(position) {
          ghost.poses.push(position);
        },
        destroy() {
          ghost.destroyed = true;
        },
      };
      previews.push(ghost);
      return ghost;
    },
  };
  world.hit = { position: { x: 0, y: 0, z: 5 }, levelObject: null };
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.marks.select(editor.boxOf(editor.selected));
  const arrow = editor.marks.arrows.find((entry) => entry.axis === 'x');
  const startX = 960 + (arrow.from.x + arrow.length / 2) * 10;
  down.add('mouse1');
  editor.pressWorld(startX, 540);
  const before = calls.length;
  editor.testCursor = { x: startX + 40, y: 540 };
  tick();
  // While held: no pose of the house itself, the preview of its pieces at the arrow's place.
  assert.ok(!calls.slice(before).some((call) => call[0] === 'setPose'));
  const preview = previews.at(-1);
  assert.deepEqual(preview.spec.models, ['objects/wall.cgf']);
  assert.equal(preview.poses.at(-1).x, 4);
  down.delete('mouse1');
  tick();
  // Let go: the preview goes and the house is posed there once.
  assert.ok(preview.destroyed);
  const poses = calls.slice(before).filter((call) => call[0] === 'setPose');
  assert.equal(poses.length, 1);
  assert.deepEqual(poses[0][1], { x: 4, y: 0, z: 5 });
  assert.equal(editor.selected.at.x, 4);
});

test("the content's pictures are taken for the shown page first, then two pages ahead", () => {
  const { editor } = setup();
  editor.open();
  const entries = Array.from({ length: 45 }, (_, index) => ({ name: `item_${index}` }));
  editor.items = () => entries.map((entry) => ({ entry }));
  editor.page = 1;
  const names = () => editor.picturesMissing().map((entry) => entry.name);
  assert.deepEqual(
    names(),
    entries.slice(10, 20).map((entry) => entry.name),
  );
  for (let index = 10; index < 20; index += 1) {
    editor.pictures.set(`item_${index}`, `item3:item_${index}`);
  }
  assert.deepEqual(
    names(),
    entries.slice(20, 30).map((entry) => entry.name),
  );
  for (let index = 20; index < 45; index += 1) {
    editor.pictures.set(`item_${index}`, null);
  }
  // The page before the shown one is not taken ahead.
  assert.deepEqual(names(), []);
  assert.equal(editor.tileShown('item_12'), true);
  assert.equal(editor.tileShown('item_22'), false);
});

test("the game client's cursor stream answers hover without asking the game", () => {
  const { editor, calls } = setup();
  editor.open();
  assert.ok(
    calls.some((call) => call[0] === 'send' && call[1].op === 'architectCursor' && call[1].on),
  );
  let asked = 0;
  editor.api.World.raycast = () => {
    asked += 1;
    return null;
  };
  editor.api.hitOf = (raw) => ({ position: { x: raw.p[0], y: raw.p[1], z: raw.p[2] } });
  editor.cursorAt({ x: VIEW_X, y: VIEW_Y, over: true, hit: { p: [3, 4, 5], n: [0, 0, 1] } });
  const cursor = editor.cursorOverView();
  assert.deepEqual(cursor, { x: VIEW_X, y: VIEW_Y });
  assert.deepEqual(editor.pointAt(cursor.x, cursor.y).position, { x: 3, y: 4, z: 5 });
  assert.equal(asked, 0);
  editor.cursorAt({ x: 5, y: 5, over: true, hit: null });
  assert.equal(editor.cursorOverView(), null);
});

test('the Saves tab saves the work under a name, loads one and merges one into the work', () => {
  const { editor, calls, down, tick } = setup();
  editor.open();
  const files = new Map();
  editor.api.savesList = () =>
    [...files.keys()].map((name, index) => ({ name, bytes: 2048, time: 1000 + index }));
  editor.api.saveRead = (name) => files.get(name) ?? null;
  const sent = () => calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.pressTab(3);
  editor.startTyping('save');
  for (const key of ['v', 'i', 'l', 'l', 'a', 'g', 'e', '2']) {
    down.add(key);
    tick();
    down.delete(key);
    tick();
  }
  editor.saveAs();
  const saved = sent().find((message) => message.op === 'architectSaveAs');
  assert.equal(saved.name, 'village2');
  assert.equal(JSON.parse(saved.data).buildings.length, 1);
  files.set('village2', saved.data);
  editor.refreshSaves();
  // Merge adds the save's house to the one already there; Load puts the work back to the save.
  editor.pressSave(0, 'merge');
  assert.equal(editor.placed.length, 2);
  editor.pressSave(0, 'load');
  assert.equal(editor.placed.length, 1);
  editor.pressSave(0, 'delete');
  assert.ok(sent().some((m) => m.op === 'architectSaveDelete' && m.name === 'village2'));
  assert.equal(editor.view().dock.saves.rows[0].name, 'village2');
});

test("Reset map asks first, keeps the work as a save, then leaves the game's own map", () => {
  const { editor, calls } = setup();
  editor.open();
  const files = new Map();
  editor.api.savesList = () =>
    [...files.keys()].map((name, index) => ({ name, bytes: 2048, time: 1000 + index }));
  editor.api.saveRead = (name) => files.get(name) ?? null;
  const sent = () => calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.restoreLook({ hour: 21, clock: 2, weather: 3, wind: 2, weatherHeld: false });
  editor.pressTab(3);
  assert.equal(editor.view().dock.saves.reset, true);

  // The button asks in the menu bar; Cancel leaves everything as it was.
  editor.askReset();
  assert.equal(editor.view().menu.picker, 'confirm');
  assert.equal(editor.view().menu.yes, 'Reset');
  assert.match(editor.view().menu.question, /Reset .* to the game's map/);
  editor.setMapPicker(null);
  assert.equal(editor.view().menu.picker, null);
  assert.equal(editor.placed.length, 1);

  // Yes: the work is saved under a dated name first, then the map is the game's own again.
  editor.askReset();
  editor.answerYes();
  const kept = sent().find((message) => message.op === 'architectSaveAs');
  assert.match(kept.name, /^.+ before reset \d{4}-\d\d-\d\d \d\d-\d\d$/);
  assert.equal(JSON.parse(kept.data).buildings.length, 1);
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.view().menu.picker, null);
  const project = JSON.parse(sent().findLast((message) => message.op === 'architectSave').data);
  assert.equal(project.buildings.length, 0);
  assert.deepEqual(project.look, { hour: 12, clock: 0, weather: 0, wind: 1, weatherHeld: true });
  assert.equal(editor.undoStack.length, 0);

  // The kept save loads the work back.
  files.set(kept.name, kept.data);
  editor.refreshSaves();
  editor.pressSave(0, 'load');
  assert.equal(editor.placed.length, 1);

  // An empty map is reset without a save.
  editor.resetMap();
  const before = sent().filter((message) => message.op === 'architectSaveAs').length;
  assert.equal(editor.resetMap(), null);
  assert.equal(sent().filter((message) => message.op === 'architectSaveAs').length, before);
});

test("the Saves tab's Reset map button asks the editor; the menu bar's Reset answers yes", () => {
  const { Shell, DOCK } = require('../../../../Client/resources/architect/shell.cjs');
  const clip = () => ({
    set() {},
    draw: () => true,
    on() {},
    get: () => 0,
    createClip: () => clip(),
  });
  const asked = [];
  const handlers = new Proxy(
    {
      resetMap: () => asked.push('reset'),
      mapSwitch: () => asked.push('yes'),
    },
    { get: (target, name) => target[name] ?? (() => {}) },
  );
  const shell = new Shell({ createClip: clip }, handlers, {}, () => {});
  shell.pressSaves(1430, DOCK.top + 10);
  assert.deepEqual(asked, ['reset']);
  shell.menuPicker = 'confirm';
  shell.pressMenu(1100, 15);
  assert.deepEqual(asked, ['reset', 'yes']);
});

// A room for the resting checks: its floor at z 10, its ceiling at z 14 (rays up meet it, rays
// down meet the floor), with the game's props, lights, effects and pictures stood in for.
function roomSetup() {
  const made = setup();
  const { editor } = made;
  const FLOOR = 10;
  const CEILING = 14;
  // The east wall stands at x 6, facing west.
  const WALL = 6;
  editor.api.World.raycast = (from, to) => {
    if (Math.abs(to.z - from.z) < 1e-9) {
      if (from.x < WALL && to.x >= WALL) {
        const t = (WALL - from.x) / (to.x - from.x);
        const y = from.y + (to.y - from.y) * t;
        return { position: { x: WALL, y, z: from.z }, normal: { x: -1, y: 0, z: 0 } };
      }
      return null;
    }
    if (to.z > from.z) {
      return from.z < CEILING
        ? { position: { x: from.x, y: from.y, z: CEILING }, normal: { x: 0, y: 0, z: -1 } }
        : null;
    }
    return from.z >= FLOOR
      ? { position: { x: from.x, y: from.y, z: FLOOR }, normal: { x: 0, y: 0, z: 1 } }
      : null;
  };
  editor.api.propMeshes = () => [
    'objects/manmade/common_illumination/chandelier_a.cgf',
    'objects/manmade/common_furniture/tables/table_a.cgf',
  ];
  editor.api.propBounds = () => ({ min: [-0.5, -0.5, 0], max: [0.5, 0.5, 1] });
  editor.api.Light = { spawn: () => ({ destroy() {} }) };
  editor.api.Vfx = { spawn: () => ({ destroy() {} }), list: () => ['WH_Particels.fires.candle_a'] };
  editor.api.NoClip.setPosition = () => {};
  editor.api.Render = {
    view: async ({ position, lookAt, width }) => {
      made.views.push({ position, lookAt });
      return { png: 'data:image/png;base64,UElD', width, height: Math.round(width * 0.56) };
    },
  };
  made.views = [];
  const facade = architectFacade(editor);
  made.ask = (command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id: 1, command, args })));
  made.askLater = async (command, args = {}) =>
    JSON.parse(await facade.command(JSON.stringify({ id: 1, command, args })));
  editor.open();
  return made;
}

test('Validation names a prop that floats, a chandelier that hangs in the air and a lamp without light', () => {
  const { editor, ask } = roomSetup();
  const table = ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 0, y: 0, z: 10.5 },
  });
  assert.equal(table.ok, true, table.error);
  const chandelier = ask('place', {
    name: 'objects/manmade/common_illumination/chandelier_a.cgf',
    at: { x: 4, y: 0, z: 12 },
  });
  const texts = () => editor.validate().map((issue) => issue.text);
  const said = (pattern) => texts().some((text) => pattern.test(text));
  assert.ok(said(/table_a floats 0\.50 m above/), texts().join('; '));
  assert.ok(said(/chandelier_a hangs in the air 1\.00 m under the ceiling/), texts().join('; '));
  assert.ok(said(/chandelier_a gives no light/));
  // Put down on the floor, hung from the ceiling and lit, nothing is left to say about them.
  ask('move', { id: table.result.id, at: { x: 0, y: 0, z: 10 } });
  ask('move', { id: chandelier.result.id, at: { x: 4, y: 0, z: 13 } });
  ask('light', { preset: 'candle', at: { x: 4, y: 0, z: 13.2 } });
  assert.deepEqual(
    texts().filter((text) => /table|chandelier/.test(text)),
    [],
  );
  // A light put down on the floor lights a round spot there only.
  ask('light', { preset: 'lantern', at: { x: 8, y: 0, z: 10.1 } });
  assert.ok(said(/Lantern is 0\.10 m over the floor/), texts().join('; '));
  // A room light (ambient) lights its whole radius evenly, however low it hangs.
  ask('light', { preset: 'room', at: { x: -8, y: 0, z: 10.2 } });
  assert.ok(!said(/Room light is .* over the floor/), texts().join('; '));
});

test("an AI assistant's snap rests a prop on the floor, hangs it from the ceiling, sets it against a wall", () => {
  const { editor, ask } = roomSetup();
  const table = 'objects/manmade/common_furniture/tables/table_a.cgf';
  const onFloor = ask('place', { name: table, at: { x: 1, y: 1, z: 12 }, snap: 'floor' });
  assert.equal(onFloor.ok, true, onFloor.error);
  assert.equal(onFloor.result.at.z, 10);
  const hung = ask('place', {
    name: 'objects/manmade/common_illumination/chandelier_a.cgf',
    at: { x: 2, y: 2, z: 12 },
    snap: 'ceiling',
  });
  // Its top (1 m over its place) under the ceiling at 14.
  assert.equal(hung.result.at.z, 13);
  const shelf = ask('place', {
    name: table,
    at: { x: 2, y: 3, z: 11 },
    snap: 'wall',
    toward: { x: 10, y: 3 },
  });
  // Its back (+y, 0.5 m) against the wall at x 6, its front (-y) facing west into the room.
  assert.equal(shelf.result.at.x, 5.49);
  assert.equal(shelf.result.turn, -90);
  // The table on the floor and the chandelier under the ceiling rest; a table is no wall prop, so
  // set against the wall 1 m up it floats, and Validation says so.
  assert.deepEqual(
    editor
      .validate()
      .filter((issue) => /table_a|chandelier_a hangs/.test(issue.text))
      .map((issue) => issue.text),
    ['table_a floats 1.00 m above what is under it: lower it'],
  );
  assert.match(
    ask('place', { name: table, at: { x: 1, y: 1 }, snap: 'ceiling' }).error,
    /needs at with its height/,
  );
  assert.match(
    ask('place', { name: table, at: { x: 1, y: 1, z: 11 }, snap: 'wall', toward: { x: -9, y: 1 } })
      .error,
    /no wall found/,
  );
});

test('a snapped model the game has not measured is put down, measured, then moved to rest: one undo step', async () => {
  const { editor, calls, askLater } = roomSetup();
  // The game measures a model only once a prop of it stands.
  editor.api.propBounds = (mesh) =>
    calls.some((call) => call[0] === 'send' && call[1].op === 'localProp' && call[1].mesh === mesh)
      ? { min: [-0.5, -0.5, 0], max: [0.5, 0.5, 1] }
      : null;
  const hung = await askLater('place', {
    name: 'objects/manmade/common_illumination/chandelier_a.cgf',
    at: { x: 2, y: 2, z: 12 },
    snap: 'ceiling',
  });
  assert.equal(hung.ok, true, hung.error);
  assert.equal(hung.result.at.z, 13);
  assert.equal(editor.undoStack.length, 1);
  editor.undo();
  assert.equal(editor.placed.length, 0);
});

test("a snapped prop's floor is found past the prop's own faces (World.traceAll)", async () => {
  const { editor, calls, askLater } = roomSetup();
  editor.api.propBounds = (mesh) =>
    calls.some((call) => call[0] === 'send' && call[1].op === 'localProp' && call[1].mesh === mesh)
      ? { min: [-0.5, -0.5, 0], max: [0.5, 0.5, 1] }
      : null;
  // Down from over the table: its top, its underside, then the floor at 10. The game answers 8
  // hits at most (Client/resources/world.cjs).
  editor.api.World.traceAll = (from, to, options) => {
    if (!(Number.isInteger(options.maxHits) && options.maxHits >= 1 && options.maxHits <= 8)) {
      throw new RangeError('World.traceAll: maxHits must be a whole number from 1 to 8');
    }
    const table = editor.placed.at(-1);
    const own = `kcds_local_prop_${table.handle.id}`;
    if (to.z >= from.z) {
      return [];
    }
    return [
      {
        position: { x: from.x, y: from.y, z: 12.8 },
        normal: { x: 0, y: 0, z: 1 },
        entityName: own,
      },
      {
        position: { x: from.x, y: from.y, z: 12.7 },
        normal: { x: 0, y: 0, z: -1 },
        entityName: own,
      },
      { position: { x: from.x, y: from.y, z: 10 }, normal: { x: 0, y: 0, z: 1 } },
    ];
  };
  const table = await askLater('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 1, y: 1, z: 12 },
    snap: 'floor',
  });
  assert.equal(table.ok, true, table.error);
  assert.equal(table.result.at.z, 10);
});

test("the MCP server's drawn hits stand in where they are nearer than the game's (an opened vault)", async () => {
  const { ask, askLater } = roomSetup();
  // A drawn vault at 13, under the room's collision ceiling at 14.
  const hung = ask('place', {
    name: 'objects/manmade/common_illumination/chandelier_a.cgf',
    at: { x: 2, y: 2, z: 12 },
    snap: 'ceiling',
    drawnHit: { position: { x: 2, y: 2, z: 13 }, normal: { x: 0, y: 0, z: -1 } },
  });
  assert.equal(hung.result.at.z, 12);
  // A drawn hit farther than the game's is not taken.
  const kept = ask('place', {
    name: 'objects/manmade/common_illumination/chandelier_a.cgf',
    at: { x: 3, y: 2, z: 12 },
    snap: 'ceiling',
    drawnHit: { position: { x: 3, y: 2, z: 20 }, normal: { x: 0, y: 0, z: -1 } },
  });
  assert.equal(kept.result.at.z, 13);
  const review = await askLater('review', {
    at: { x: 0, y: 0, z: 10 },
    radius: 6,
    views: 0,
    drawnCeiling: 13.5,
  });
  assert.equal(review.result.ceiling, 3.5);
});

test('with Surface on, a press on a ceiling hangs the prop from it and one on a wall sets it against it', () => {
  const { editor } = roomSetup();
  editor.fitGround = true;
  editor.choose('objects/manmade/common_illumination/chandelier_a.cgf');
  const ceiling = { position: { x: 2, y: 2, z: 14 }, normal: { x: 0, y: 0, z: -1 } };
  assert.deepEqual(editor.surfaceSpot(editor.armed, ceiling).at, { x: 2, y: 2, z: 13 });
  editor.putDown({ x: 2, y: 2, z: 14 }, ceiling);
  assert.equal(editor.placed.at(-1).at.z, 13);
  editor.choose('objects/manmade/common_furniture/tables/table_a.cgf');
  const wall = { position: { x: 6, y: 3, z: 11 }, normal: { x: -1, y: 0, z: 0 } };
  editor.putDown({ x: 6, y: 3, z: 11 }, wall);
  assert.deepEqual(editor.placed.at(-1).at, { x: 5.49, y: 3, z: 11 });
  assert.equal(editor.placed.at(-1).turn, -90);
  // Surface off: the press point, as before.
  editor.fitGround = false;
  assert.equal(editor.surfaceSpot(editor.armed, wall), null);
});

test('Validation looks again after a load, also of a save as large as the work it replaced', () => {
  const { editor, ask } = roomSetup();
  const files = new Map();
  editor.api.savesList = () => [...files.keys()].map((name) => ({ name, bytes: 1, time: 1 }));
  editor.api.saveRead = (name) => files.get(name) ?? null;
  ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 0, y: 0, z: 10.5 },
  });
  files.set('room', JSON.stringify(editor.project()));
  for (let load = 0; load < 2; load += 1) {
    ask('load', { name: 'room' });
    const floating = editor.validate().find((issue) => /floats/.test(issue.text));
    assert.ok(editor.placed.includes(floating.thing), 'the issue names the loaded table');
  }
});

test("a ray the game refuses leaves Validation's room checks out, never the editor's commands", () => {
  const { editor, ask } = roomSetup();
  ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 0, y: 0, z: 10.5 },
  });
  editor.api.World.raycast = () => {
    throw new RangeError('World.raycast: refused');
  };
  ask('light', { preset: 'candle', at: { x: 1, y: 0, z: 10.2 } });
  const state = ask('state');
  assert.equal(state.ok, true, state.error);
  assert.ok(Array.isArray(editor.validate()));
});

test("an AI assistant's light is placed from a preset and set as Details sets it, one undo step", () => {
  const { editor, ask } = roomSetup();
  const lit = ask('light', {
    preset: 'torch',
    at: { x: 1, y: 2, z: 12 },
    color: 'Candle',
    intensity: 2.5,
    radius: 5,
    shadows: true,
  });
  assert.equal(lit.ok, true, lit.error);
  assert.deepEqual(lit.result.light, {
    color: [1, 0.8, 0.55],
    intensity: 2.5,
    radius: 5,
    shadows: true,
    ambient: false,
  });
  assert.equal(editor.undoStack.length, 1);
  const changed = ask('light', { id: lit.result.id, color: [0.9, 0.6, 0.3], intensity: 4 });
  assert.deepEqual(changed.result.light.color, [0.9, 0.6, 0.3]);
  assert.equal(changed.result.light.radius, 5);
  editor.undo();
  assert.equal(editor.placed.at(-1).light.intensity, 2.5);
  assert.match(ask('light', { preset: 'sun', at: { x: 0, y: 0 } }).error, /preset is one of/);
  assert.match(ask('light', { id: lit.result.id, color: [2, 0, 0] }).error, /each 0 to 1/);
  assert.match(ask('light', { id: lit.result.id, radius: 0 }).error, /radius is 0.5 to 100/);
});

test('a room light is spawned as one; its Room light row turns it into a point light and back', () => {
  const { editor, ask } = roomSetup();
  const spawned = [];
  editor.api.Light = {
    spawn: (position, options) => {
      const light = { position, ambient: options.ambient, destroyed: false };
      light.destroy = () => {
        light.destroyed = true;
      };
      spawned.push(light);
      return light;
    },
  };
  const room = ask('light', { preset: 'room', at: { x: 0, y: 0, z: 12 } });
  assert.equal(room.result.light.ambient, true);
  assert.equal(spawned[0].ambient, true);
  editor.select(editor.placed.at(-1));
  const row = editor.details().rows.find((entry) => entry.label === 'Room light');
  assert.equal(row.toggle, true);
  row.press();
  assert.equal(spawned[0].destroyed, true);
  assert.equal(spawned.at(-1).ambient, false);
  editor.undo();
  assert.equal(spawned.at(-1).ambient, true);
  assert.equal(ask('light', { id: room.result.id, ambient: false }).result.light.ambient, false);
});

test("an AI assistant's effect takes a size and how often it plays again", () => {
  const { ask } = roomSetup();
  const fire = ask('place', { name: 'WH_Particels.fires.candle_a', at: { x: 0, y: 0, z: 11 } });
  assert.equal(fire.ok, true, fire.error);
  const set = ask('effect', { id: fire.result.id, size: 2, repeat: 5 });
  assert.deepEqual(set.result.effect, { size: 2, repeat: 5 });
  assert.match(ask('effect', { id: fire.result.id, repeat: 3 }).error, /repeat is one of/);
  const table = ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 0, y: 0, z: 10 },
  });
  assert.match(ask('effect', { id: table.result.id, size: 2 }).error, /no effect of yours/);
});

test("an AI assistant's review answers the area's issues and pictures from inside it", async () => {
  const { editor, ask, askLater, views } = roomSetup();
  // The camera answers where it was put, as the game's does.
  const cameraAt = [{ x: 0, y: 0, z: 100 }];
  editor.api.NoClip.setPosition = (position) => cameraAt.push({ ...position });
  editor.api.NoClip.getPosition = () => cameraAt.at(-1);
  ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 1, y: 1, z: 11 },
  });
  // Outside the area: not reviewed.
  ask('place', {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    at: { x: 40, y: 0, z: 11 },
  });
  const answer = await askLater('review', { at: { x: 0, y: 0, z: 10 }, radius: 8 });
  assert.equal(answer.ok, true, answer.error);
  const review = answer.result;
  assert.equal(review.indoors, true);
  assert.equal(review.ceiling, 4);
  assert.equal(review.things, 1);
  assert.deepEqual(
    review.issues.filter((issue) => /floats/.test(issue.text)).map((issue) => typeof issue.id),
    ['number'],
  );
  assert.deepEqual(
    review.pictures.map((picture) => picture.label),
    [
      'from the north-east corner',
      'from the north-west corner',
      'from the south-west corner',
      'from the south-east corner',
      'from under the ceiling',
    ],
  );
  assert.equal(review.pictures[0].png, 'UElD');
  // The cameras stand at eye height over the floor, inside the room, looking at its middle.
  assert.ok(views.slice(0, 4).every((view) => Math.abs(view.position.z - 11.7) < 1e-9));
  assert.ok(views[4].position.z < 14);
  // The editor camera was brought into the room first (the game's eye adapts to its light), and
  // stays there.
  const last = cameraAt.at(-1);
  assert.deepEqual([last.x, last.y], [0, 0]);
  assert.ok(Math.abs(last.z - 11.7) < 1e-9);
  assert.equal(cameraAt.length, 2);
});

test("content pictures and an AI's screenshots leave the panels out in the game, never hiding them", async () => {
  const { editor, askLater } = roomSetup();
  const asked = [];
  const hidden = [];
  editor.shell = { setShown: (value) => hidden.push(value), render() {} };
  const selectedThen = [];
  editor.api.Render.view = async (options) => {
    asked.push(options.ui);
    selectedThen.push(editor.selected);
    return { png: 'data:image/png;base64,UElD', width: options.width, height: options.width };
  };
  const entry = {
    name: 'objects/manmade/common_furniture/tables/table_a.cgf',
    label: 'table_a',
    source: 'prop',
  };
  await editor.takePictures([entry], { distance: 3, height: 0.6, fit: true, spacing: 24 });
  assert.equal(editor.pictures.get(entry.name), `item3:${entry.name}`);
  // The selection's box and gizmo, drawn in the world, are let go of while a picture is taken.
  editor.choose(entry.name);
  editor.place({ x: 0, y: 0, z: 100 }, 0);
  const table = editor.selected;
  assert.ok(table);
  await askLater('screenshot', {
    position: { x: 1, y: 1, z: 102 },
    lookAt: { x: 0, y: 0, z: 100 },
  });
  assert.equal(selectedThen.at(-1), null);
  assert.equal(editor.selected, table);
  assert.deepEqual(asked, [false, false]);
  // The panels on screen are never hidden for a picture: that blinked.
  assert.deepEqual(hidden, []);
});

test('a slot changed on one piece of an opened building is changed on every piece of its material', () => {
  const { editor, calls, ask } = roomSetup();
  const material = 'objects/kcds/opened/church';
  const church = `<Material><SubMaterials><Material Name="glass"/><Material Name="plaster_b" Diffuse="0.4,0.4,0.4"/><Material Name="proxy_stone"/></SubMaterials></Material>`;
  const files = new Map([
    [`${material}.mtl`, church],
    ['objects/kcds/textures/stone.mtl', '<Material Shader="Illum" Diffuse="1,1,1"></Material>'],
  ]);
  let assets = [];
  editor.api.userAssets = () => ({
    folder: 'C:/assets',
    meshes: ['objects/kcds/opened/church_0.cgf', 'objects/kcds/opened/church_1.cgf'],
    materials: ['objects/kcds/textures/stone', ...assets],
    refused: [],
  });
  editor.api.modelSlots = () => ({ material, slots: ['glass', 'plaster_b', 'proxy_stone'] });
  editor.api.assetText = (path) => files.get(path) ?? null;
  const pieces = [0, 1].map((n) =>
    ask('place', { name: `objects/kcds/opened/church_${n}.cgf`, at: { x: n, y: 0, z: 10 } }),
  );
  assert.ok(pieces.every((piece) => piece.ok));
  const [first, second] = editor.placed.slice(-2);
  editor.select(first);
  // Details lists the slots it may change (not the collision's) and steps one to your stone.
  const slotRows = editor.details().rows;
  const plaster = slotRows.find((row) => row.label === 'plaster_b');
  assert.equal(plaster.value, 'Its own');
  assert.ok(!slotRows.some((row) => row.label === 'proxy_stone'));
  plaster.step(1);
  const written = calls
    .filter((call) => call[0] === 'send' && call[1].op === 'architectAssetWrite')
    .map((call) => call[1]);
  const made = written.at(-1).path;
  assert.equal(made, 'objects/kcds/materials/church_0_1.mtl');
  const text = Buffer.from(written.at(-1).data, 'base64').toString('utf8');
  assert.match(text, /<Material Name="plaster_b" Shader="Illum"/);
  assert.equal(first.material, 'objects/kcds/materials/church_0_1');
  assert.equal(second.material, 'objects/kcds/materials/church_0_1');
  assert.equal(first.slotLooks.plaster_b, 'stone');
  assert.deepEqual(editor.project().props.at(-1).slots, { plaster_b: 'stone' });
  // One undo step takes both back.
  editor.undo();
  assert.equal(first.material ?? null, null);
  assert.equal(second.material ?? null, null);
  assert.equal(first.slotLooks, undefined);
  // An assistant asks the slots and sets one like another slot.
  files.set('objects/kcds/materials/church_0_1.mtl', text);
  assets = ['objects/kcds/materials/church_0_1'];
  const slots = ask('slots', { id: pieces[0].result.id });
  assert.deepEqual(
    slots.result.slots.map((slot) => slot.name),
    ['glass', 'plaster_b', 'proxy_stone'],
  );
  assert.equal(slots.result.sharedBy.length, 2);
  const set = ask('set_slot', {
    id: pieces[0].result.id,
    slot: 'glass',
    like: 'plaster_b',
    tile: 3,
  });
  assert.equal(set.ok, true, set.error);
  assert.equal(set.result.props.length, 2);
  assert.match(ask('set_slot', { id: pieces[0].result.id, slot: 'glass' }).error, /give material/);
});

test("Yours' New ... tiles make a material of that kind under the next free name", () => {
  const { editor, calls } = roomSetup();
  editor.api.userAssets = () => ({
    folder: 'C:/assets',
    meshes: [],
    materials: ['objects/kcds/textures/plaster_1'],
    refused: [],
  });
  editor.random = () => 0.25;
  editor.useMode('props');
  editor.pressFolder(7);
  const index = editor.items().findIndex((item) => item.name === 'New plaster');
  editor.pressTile(index);
  const paths = calls
    .filter((call) => call[0] === 'send' && call[1].op === 'architectAssetWrite')
    .map((call) => call[1].path);
  assert.ok(paths.includes('objects/kcds/textures/plaster_2.mtl'), paths.join(', '));
  assert.equal(editor.armed, null, 'nothing is armed to put down');
});

test("the game's own interiors near a level building: Details' Go inside and an AI's enter_interior", () => {
  const { editor, ask } = roomSetup();
  const house = 'structures/living/houses/vlasska_11_house1_4d58f1d4-40f0-4282-a7f2-84ca7381ef2b';
  const near = [
    {
      class: 'EnvironmentLight',
      name: `envprobe_house_6[${house}]`,
      guid: 'p6',
      position: { x: 4, y: 0, z: 12 },
    },
    { class: 'AnimDoor', name: `door_a[${house}]`, guid: 'd1', position: { x: 0, y: -5, z: 10 } },
  ];
  editor.api.World.entitiesAround = (centre, radius, options) =>
    near.filter((entity) => entity.class === options.class);
  editor.api.entityProperties = () => [
    { key: 'BoxSizeX', type: 'number', value: 8 },
    { key: 'BoxSizeY', type: 'number', value: 6 },
    { key: 'BoxSizeZ', type: 'number', value: 4 },
  ];
  let camera = null;
  editor.api.NoClip.setPosition = (at) => {
    camera = at;
  };
  editor.api.NoClip.lookAt = () => {};
  // The house's own model (vlasska_11_house.cgf, under its prefab vlasska_11_house1) has these
  // rooms; a path beside it has none and is no building at all.
  editor.api.levelBrushes = () => [
    {
      mesh: 'objects/manmade/structures/living/houses/vlasska_11_house.cgf',
      at: { x: 2, y: 0, z: 10 },
      turn: 0,
      scale: 1,
    },
    {
      mesh: 'objects/manmade/structures/logistical/paths/soil_path_a.cgf',
      at: { x: 1, y: 0, z: 10 },
      turn: 0,
      scale: 1,
    },
  ];
  assert.deepEqual(
    ask('buildings_near', { at: { x: 0, y: 0, z: 10 } }).result.map((b) => [b.interior, b.closed]),
    [['vlasska_11_house1', false]],
  );
  const listed = ask('interiors_near', { at: { x: 0, y: 0, z: 10 } });
  assert.equal(listed.ok, true, listed.error);
  assert.equal(listed.result[0].building, 'vlasska_11_house1');
  assert.deepEqual(listed.result[0].rooms[0].box, { x: 8, y: 6, z: 4 });
  assert.equal(listed.result[0].doors.length, 1);
  const entered = ask('enter_interior', { at: { x: 0, y: 0, z: 10 } });
  assert.equal(entered.result.room, 'envprobe_house_6');
  // At a player's eye over the room's floor (10), not at the probe.
  assert.deepEqual([camera.x, camera.y, camera.z], [4, 0, 11.7]);
  // A picked level building shows it in Details.
  const thing = editor.levelThing({
    levelObject: {
      kind: 'brush',
      name: 'objects/houses/house_a.cgf',
      guid: '',
      class: '',
      at: { x: 1, y: 1, z: 10 },
    },
    kind: 'level',
  });
  editor.select(thing);
  const rows = editor.details().rows;
  assert.equal(rows.find((row) => row.label === 'Building').value, 'vlasska_11_house1');
  camera = null;
  rows.find((row) => row.label === 'Go inside').press();
  assert.equal(camera.x, 4);
  near.length = 0;
  assert.match(ask('enter_interior', { at: { x: 0, y: 0, z: 10 } }).error, /open_building/);
});

test("furniture arranged: an AI's arrange and catalogue tags, Details' ARRANGE by hand", async () => {
  const { editor, ask, askLater } = roomSetup();
  const table = 'objects/manmade/common_furniture/tables/table_a.cgf';
  const chair = 'objects/manmade/common_furniture/chairs/high/chair_a.cgf';
  editor.api.propMeshes = () => [
    'objects/manmade/common_illumination/chandelier_a.cgf',
    table,
    chair,
  ];
  editor.catalogue.refresh();
  // The catalogue answers what each thing is for, and finds a room's furniture by it.
  const seats = ask('catalogue', { tag: 'seating' });
  assert.deepEqual(
    seats.result.items.map((item) => [item.name, item.tags]),
    [[chair, ['seating']]],
  );
  // A tavern's: its tables and chairs, and its lights (candles, lamps), no collision shape.
  const tavern = ask('catalogue', { room: 'tavern' }).result.items.map((item) => item.name);
  assert.ok(tavern.includes(chair) && tavern.includes(table) && tavern.includes('candle'));
  assert.ok(!tavern.includes('box'));
  assert.match(ask('catalogue', { tag: 'nope' }).error, /tag is one of/);
  // Four chairs around a table, facing it, all one undo step.
  ask('place', { name: table, at: { x: 0, y: 0, z: 10 } });
  const undoAt = editor.undoStack.length;
  const around = await askLater('arrange', {
    layout: 'around',
    name: chair,
    at: { x: 0, y: 0, z: 10 },
    count: 4,
    radius: 1,
  });
  assert.equal(around.ok, true, around.error);
  const rounded = (value) => Math.round(value) + 0;
  assert.deepEqual(
    around.result.things.map((thing) => [rounded(thing.at.x), rounded(thing.at.y), thing.turn]),
    [
      [1, 0, 270],
      [0, 1, 0],
      [-1, 0, 90],
      [0, -1, 180],
    ],
  );
  assert.equal(editor.undoStack.length, undoAt + 1);
  // The same chairs moved into a row along a wall.
  const ids = around.result.things.map((thing) => thing.id);
  const row = await askLater('arrange', {
    layout: 'row',
    ids,
    from: { x: -3, y: 4, z: 10 },
    to: { x: 3, y: 4, z: 10 },
    turn: 180,
  });
  assert.equal(row.ok, true, row.error);
  assert.deepEqual(
    row.result.things.map((thing) => thing.at.x),
    [-3, -1, 1, 3],
  );
  assert.ok(row.result.things.every((thing) => thing.at.y === 4 && thing.turn === 180));
  // By hand: the table selected, Details' ARRANGE puts copies in a row and the chosen chair
  // around it.
  const tableThing = editor.placed.find((record) => record.entry.name === table);
  editor.select(tableThing);
  let rows = editor.details().rows;
  rows.find((entry) => entry.label === 'Copies').step(-2);
  rows = editor.details().rows;
  assert.equal(rows.find((entry) => entry.label === 'Copies').value, '2');
  const before = editor.placed.length;
  rows.find((entry) => entry.label === 'In a row').press();
  assert.deepEqual(
    editor.placed.slice(before).map((record) => [record.entry.name, record.at.x, record.at.y]),
    [
      [table, 1.5, 0],
      [table, 3, 0],
    ],
  );
  assert.equal(editor.selected, tableThing);
  editor.choose(chair);
  editor.select(tableThing);
  rows = editor.details().rows;
  assert.equal(rows.find((entry) => entry.label === 'Around it').value, 'chair_a');
  rows.find((entry) => entry.label === 'Around it').press();
  const ring = editor.placed.slice(before + 2);
  assert.equal(ring.length, 2);
  assert.ok(ring.every((record) => record.entry.name === chair));
  // Half the table's width (0.5) and a chair's half depth away.
  assert.ok(ring.every((record) => Math.abs(Math.hypot(record.at.x, record.at.y) - 0.95) < 0.01));
});

test("a closed level building opened where it stands: an AI's open_building and Details' Open it", () => {
  const { editor, calls, ask } = roomSetup();
  const { writeCgf } = require('../../../../Dev/tools/architect-mcp/models/cgf_writer.cjs');
  const vertex = (p) => ({ p, uv: [0, 0], t: [1, 0, 0], b: [0, 1, 0], w: 1 });
  // A church of the level: one drawn triangle, a box filling its nave and a wall box.
  const church = writeCgf({
    pieces: [
      {
        name: 'nave',
        at: [0, 0, 0],
        vertices: [vertex([0, 0, 0]), vertex([10, 0, 0]), vertex([0, 10, 0])],
        indices: [0, 1, 2],
        subsets: [
          {
            firstIndex: 0,
            indices: 3,
            firstVertex: 0,
            vertices: 3,
            material: 1,
            radius: 7,
            centre: [3, 3, 0],
          },
        ],
        min: [0, 0, 0],
        max: [10, 10, 0],
      },
    ],
    material: 'objects/manmade/church',
    subMaterials: ['proxy', 'walls'],
    boxes: [
      // Filling the nave from 2 m under its doorstep, as a house's box does.
      { centre: [0, 0, 4], half: [6, 4, 6] },
      { centre: [7, 0, 6], half: [0.5, 4, 6] },
    ],
  });
  const mesh = 'objects/manmade/structures/theological/church_a.cgf';
  editor.api.levelBrushes = () => [
    { mesh: 'objects/natural/rocks/stone_a.cgf', at: { x: 1, y: 1, z: 10 }, turn: 0, scale: 1 },
    { mesh, at: { x: 0, y: 0, z: 10 }, turn: 90, scale: 1 },
  ];
  editor.api.World.entitiesAround = () => [];
  const read = [];
  editor.api.assetBytes = (path) => {
    read.push(path);
    return path === mesh ? new Uint8Array(church) : null;
  };
  // The assets folder holds what the game wrote: the split pieces and the editor's files.
  const written = new Set();
  const split = [];
  editor.api.splitModel = (from, to, material) => {
    split.push({ from, to, material });
    const pieces = [`${to}_0.cgf`, `${to}_1.cgf`];
    pieces.forEach((piece) => written.add(piece));
    return { meshes: pieces };
  };
  const send = editor.api.send;
  editor.api.send = (message) => {
    if (message.op === 'architectAssetWrite') {
      written.add(message.path);
    }
    return send(message);
  };
  editor.api.userAssets = () => ({
    folder: 'C:/assets',
    meshes: [...written].filter((file) => file.endsWith('.cgf')),
    materials: [],
    refused: [],
  });
  const listed = ask('buildings_near', { at: { x: 0, y: 0 } });
  assert.equal(listed.ok, true, listed.error);
  assert.deepEqual(
    listed.result.map((building) => [building.mesh, building.closed]),
    [[mesh, true]],
  );
  const undoAt = editor.undoStack.length;
  const opened = ask('open_building', { at: { x: 0, y: 0, z: 10 } });
  assert.equal(opened.ok, true, opened.error);
  assert.deepEqual(read, [mesh]);
  assert.deepEqual(split, [
    {
      from: 'objects/manmade/structures/theological/church_a.cgfm',
      to: 'objects/kcds/opened/church_a',
      material: 'objects/manmade/church',
    },
  ]);
  assert.equal(opened.result.removed.length, 1);
  assert.equal(opened.result.floors, 1);
  assert.deepEqual(opened.result.meshes, [
    'objects/kcds/opened/church_a_0.cgf',
    'objects/kcds/opened/church_a_1.cgf',
    'objects/kcds/opened/church_a_walls.cgf',
    'objects/kcds/opened/church_a_floor.cgf',
  ]);
  // The collision model and the drawn floor went to the game in parts, the floor's planks made
  // once into your textures; then each piece stands where the church does.
  const writes = calls.filter((call) => call[0] === 'send' && call[1].op === 'architectAssetWrite');
  assert.ok(writes.some((call) => call[1].path === 'objects/kcds/opened/church_a_walls.cgf'));
  assert.ok(writes.some((call) => call[1].path === 'objects/kcds/textures/opened_floor.mtl'));
  assert.equal(opened.result.things.length, 4);
  for (const thing of opened.result.things) {
    assert.deepEqual([thing.at.x, thing.at.y, thing.at.z, thing.turn], [0, 0, 10, 90]);
  }
  // The level's own church is hidden, and the room light hangs in the nave.
  const hidden = calls.filter((call) => call[0] === 'send' && call[1].op === 'localLevelEdit');
  assert.equal(hidden.at(-1)[1].name, mesh);
  assert.equal(hidden.at(-1)[1].hidden, true);
  assert.ok(opened.result.light);
  // Over the floor put in at the church's origin (its box reached 2 m into the ground).
  assert.equal(opened.result.light.at.z, 10.02 + 3);
  assert.equal(opened.result.light.light.intensity, 2.5);
  // Warm lights filling its rooms, without shadows: one for this 12 x 8 m nave, halfway up it
  // (its box from 2 m under the doorstep to 10 m over it), reaching past its walls.
  assert.equal(opened.result.fills.length, 1);
  const [fill] = opened.result.fills;
  assert.deepEqual([fill.light.intensity, fill.light.shadows, fill.light.radius], [3, false, 15.6]);
  // A flame's colour, as the game's rooms are lit.
  assert.deepEqual(fill.light.color, [1, 0.55, 0.2]);
  assert.ok(Math.abs(fill.at.z - (10 + 0.02 + 4.99)) < 0.01, `${fill.at.z}`);
  // The ground it hid levelled just under that floor, fading inside its walls, kept in the work.
  const levelled = editor.project().terrain;
  assert.ok(levelled.length > 0);
  assert.ok(levelled.every((edit) => edit.mode === 'flatten' && edit.falloff === 0.5));
  assert.ok(levelled.every((edit) => Math.abs(edit.at.z - 9.72) < 1e-9));
  // All of it one undo step.
  assert.equal(editor.undoStack.length, undoAt + 1);
  editor.undo();
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.project().terrain.length, 0);
  // A failure half way (the ground refusing to change) takes the half-opened church back.
  const flatten = editor.api.Terrain.flatten;
  editor.api.Terrain.flatten = () => {
    throw new Error('the ground would not change');
  };
  const steps = editor.undoStack.length;
  const failed = ask('open_building', { at: { x: 0, y: 0, z: 10 } });
  assert.match(failed.error, /the ground would not change/);
  assert.equal(editor.placed.length, 0);
  assert.equal(editor.undoStack.length, steps);
  assert.equal(editor.hiddenLevel.length, 0);
  editor.api.Terrain.flatten = flatten;
  // Details of the closed church: Open it does the same, under a free name.
  const thing = editor.levelThing({
    levelObject: { kind: 'brush', name: mesh, guid: '', class: '', at: { x: 0, y: 0, z: 10 } },
    kind: 'level',
  });
  editor.select(thing);
  const open = editor.details().rows.find((row) => row.label === 'Open it');
  assert.ok(open);
  open.press();
  // The failed attempt's files took church_a_2.
  assert.equal(split.at(-1).to, 'objects/kcds/opened/church_a_3');
  assert.match(editor.message, /Opened church_a/);
  // With the copies' files deleted, a name the game loaded in this run is still not used again:
  // it would draw the old copy.
  editor.undo();
  written.clear();
  open.press();
  assert.equal(split.at(-1).to, 'objects/kcds/opened/church_a_4');
});

test("an AI assistant's reset_map needs confirm and answers the save it kept", () => {
  const { editor } = setup();
  editor.open();
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: [],
    WEATHERS: [],
    WINDS: [],
  });
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const refused = JSON.parse(ai.handle(JSON.stringify({ id: 1, command: 'reset_map' })));
  assert.equal(refused.ok, false);
  assert.match(refused.error, /confirm: true/);
  assert.equal(editor.placed.length, 1);
  const done = JSON.parse(
    ai.handle(JSON.stringify({ id: 2, command: 'reset_map', args: { confirm: true } })),
  );
  assert.equal(done.ok, true);
  assert.match(done.result.keptAs, /before reset/);
  assert.equal(editor.placed.length, 0);
});

test("a level object's box from the game client frames it, not a guessed one", () => {
  const { editor } = setup();
  editor.open();
  const object = {
    kind: 'brush',
    name: 'objects/buildings/house_a.cgf',
    at: { x: 10, y: 20, z: 5 },
  };
  editor.api.hitOf = (raw) => ({
    position: { x: raw.p[0], y: raw.p[1], z: raw.p[2] },
    kind: 'brush',
    levelObject: object,
  });
  editor.cursorAt({
    x: VIEW_X,
    y: VIEW_Y,
    over: true,
    hit: { p: [11, 21, 6], n: [0, 0, 1], box: [4, 14, 5, 16, 26, 13] },
  });
  const thing = editor.levelThing(editor.nativeCursor.hit);
  const box = editor.boxOf(thing);
  assert.deepEqual(box.min, [-6, -6, 0]);
  assert.deepEqual(box.max, [6, 6, 8]);
});

test('marks are drawn under the panels: a line into the dock stops at the view edge', () => {
  const { Shell } = require('../../../../Client/resources/architect/shell.cjs');
  const drawings = [];
  const root = {
    createClip() {
      const clip = {
        set() {},
        on() {},
        get() {
          return 0;
        },
        draw(commands) {
          drawings.push({ clip, commands });
          return true;
        },
        createClip: () => root.createClip(),
      };
      return clip;
    },
  };
  const shell = new Shell(root, new Proxy({}, { get: () => () => {} }), {}, () => {});
  shell.build();
  // From the view (x 400, y 400) straight down into the dock (y 900): the view ends at y 768.
  shell.drawLines([
    { from: { x: 400, y: 400 }, to: { x: 400, y: 900 }, color: 0xffffff, width: 2 },
  ]);
  const ring = drawings.at(-1).commands;
  const lineTo = ring.find((command) => command[0] === 'lineTo');
  // In the layer's own place (the view's corner at 326, 84): x 74, y 768 - 84.
  assert.deepEqual(lineTo, ['lineTo', 74, 684]);
});

test('an AI assistant sets the fly speed, freezes time and weather, and steps a gate', () => {
  const { editor, calls } = setup();
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  ask(1, 'open');
  assert.deepEqual(ask(2, 'fly_speed', { speed: 64 }).result, { speed: 64 });
  ask(3, 'look', { hour: 18.5, freeze_time: false, freeze_weather: false });
  assert.equal(editor.look.hour, 18.5);
  assert.notEqual(editor.look.clock, 0);
  assert.equal(editor.look.weatherHeld, false);
  editor.api.gatesNear = () => [
    { gate: 3, key: 'gate_west', state: -1, states: ['closed', 'open'] },
  ];
  assert.equal(ask(4, 'gates_near', { at: { x: 1, y: 2 } }).result.length, 1);
  assert.deepEqual(ask(5, 'gate', { gate: 3, state: 'open', at: { x: 1, y: 2 } }).result, {
    gate: 3,
    state: 'open',
  });
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1], {
    op: 'localGate',
    gate: 3,
    state: 1,
  });
  assert.equal(ask(6, 'gate', { gate: 3, state: 'barricaded', at: { x: 1, y: 2 } }).ok, false);
});

test('an AI assistant drives the editor with JSON lines, each change an undo step', () => {
  const { editor, calls } = setup();
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  // Closed: only open and state answer.
  assert.equal(ask(1, 'list').ok, false);
  assert.equal(ask(2, 'open').ok, true);
  // The content, searched.
  const found = ask(3, 'catalogue', { query: 'house' });
  assert.ok(
    found.result.items.some((item) => item.name === 'structures/living/houses/house_1s_2r_a'),
  );
  // A house at a point (the ground's height asked where no z is given).
  editor.api.groundHeights = (points) => points.map(() => 7);
  const placed = ask(4, 'place', {
    name: 'structures/living/houses/house_1s_2r_a',
    at: { x: 30, y: 40 },
    turn: 90,
  });
  assert.equal(placed.ok, true, placed.error);
  assert.equal(placed.result.kind, 'prefab');
  assert.deepEqual(placed.result.at, { x: 30, y: 40, z: 7 });
  assert.equal(placed.result.turn, 90);
  // Its box in its own frame, to line things up: as wide, deep and tall as the editor sees it.
  const box = editor.boxOf(editor.placed[0]);
  assert.deepEqual(
    placed.result.box.min,
    box.min.map((value) => Math.round(value * 10) / 10),
  );
  assert.deepEqual(
    placed.result.box.max,
    box.max.map((value) => Math.round(value * 10) / 10),
  );
  assert.ok(placed.result.box.max[2] > placed.result.box.min[2]);
  const id = placed.result.id;
  assert.equal(ask(5, 'list', { kind: 'prefab' }).result.length, 1);
  // Moved, then the move undone: back where it was.
  assert.deepEqual(ask(6, 'move', { id, at: { x: 35, y: 40, z: 7 } }).result.at, {
    x: 35,
    y: 40,
    z: 7,
  });
  assert.deepEqual(ask(7, 'history').result.done.slice(-2), ['house_1s_2r_a', 'move']);
  ask(8, 'undo');
  assert.deepEqual(ask(9, 'list').result[0].at, { x: 30, y: 40, z: 7 });
  // A raise stroke over three points: one step.
  const before = editor.undoStack.length;
  const raised = ask(10, 'terrain', {
    mode: 'raise',
    points: [
      { x: 0, y: 0, z: 5 },
      { x: 8, y: 0, z: 5 },
      { x: 16, y: 0, z: 5 },
    ],
    size: 4,
  });
  assert.equal(raised.ok, true, raised.error);
  assert.equal(raised.result.dabs, 3);
  assert.equal(editor.undoStack.length, before + 1);
  assert.equal(calls.filter((call) => call[0] === 'raise').length, 3);
  // The look by its labels.
  assert.equal(ask(11, 'look', { hour: 20, weather: 'Fog' }).result.weather, 'Fog');
  // Mistakes come back as errors with the request's id.
  assert.deepEqual(ask(12, 'fly'), { id: 12, ok: false, error: 'unknown command "fly"' });
  assert.equal(ask(13, 'move', { id: 999 }).ok, false);
  assert.equal(JSON.parse(facade.command('not json')).ok, false);
});

test('the AI tab shows how an AI app connects, each line to copy', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.api.aiBridge = () => ({
    port: 47870,
    listening: true,
    connected: false,
    server: 'C:KCDSarchitect-mcpserver.cjs',
    token: 'abc123',
  });
  editor.pressTab(4);
  const ai = editor.aiView();
  assert.match(ai.status, /Waiting for an AI assistant on 127\.0\.0\.1:47870/);
  assert.equal(
    ai.rows[0].text,
    'claude mcp add kcds-world-architect -- node "C:KCDSarchitect-mcpserver.cjs"',
  );
  assert.deepEqual(JSON.parse(ai.rows[1].text).mcpServers['kcds-world-architect'], {
    command: 'node',
    args: ['C:KCDSarchitect-mcpserver.cjs'],
  });
  editor.copyAi(0);
  assert.deepEqual(lastCall(calls)[1], { op: 'architectCopy', text: ai.rows[0].text });
  // Shown, the token is dots and the Windows user's folder %USERPROFILE% (streams, screenshots);
  // copied, both are whole.
  editor.ai.server = 'C:\\Users\\alice\\KCDS\\architect-mcp\\server.cjs';
  const shown = editor.aiView();
  assert.ok(
    shown.rows.every((row) => !row.shown.includes('alice') && !row.shown.includes('abc123')),
  );
  assert.ok(shown.rows[0].shown.includes('%USERPROFILE%\\KCDS'));
  assert.ok(shown.rows[0].text.includes('C:\\Users\\alice'));
  editor.copyAi(2);
  assert.deepEqual(lastCall(calls)[1], { op: 'architectCopy', text: 'abc123' });
});

test("Props places the game's catalog meshes solid in this game, moved and kept in the project", () => {
  const { editor, calls } = setup();
  editor.api.propMeshes = () => [
    'objects/manmade/common_furniture/tables/table_a.cgf',
    'objects/manmade/common_furniture/tables/table_a_proxy.cgf',
    'objects/natural/rocks/rock_big_a.cgf',
  ];
  editor.catalogue.propMeshes = editor.api.propMeshes;
  editor.open();
  editor.useMode('props');
  assert.deepEqual(
    editor.itemsOf('furniture').map((item) => item.name),
    ['table_a'],
  );
  assert.ok(editor.choose('objects/manmade/common_furniture/tables/table_a.cgf'));
  editor.turn = 90;
  editor.pressWorld(VIEW_X, VIEW_Y);
  const set = calls.filter((call) => call[0] === 'send' && call[1].op === 'localProp');
  assert.equal(set.length, 1);
  assert.equal(set[0][1].kind, 'set');
  assert.equal(set[0][1].mesh, 'objects/manmade/common_furniture/tables/table_a.cgf');
  assert.ok(Math.abs(set[0][1].rotation.z - Math.SQRT1_2) < 1e-9);
  const prop = editor.placed[0];
  assert.equal(editor.selectionKind(), 'prop');
  // Its box is the one the game measures for the placed mesh.
  editor.api.propBounds = () => ({ min: [-1.2, -0.4, 0], max: [1.2, 0.4, 0.8] });
  assert.deepEqual(editor.boxOf(prop).max, [1.2, 0.4, 0.8]);
  editor.moveSelected({ x: 20, y: 30, z: 5 }, 90);
  assert.deepEqual(lastCall(calls)[1].position, { x: 20, y: 30, z: 5 });
  assert.deepEqual(editor.project().props, [
    { name: prop.entry.name, at: { x: 20, y: 30, z: 5 }, turn: 90 },
  ]);
  editor.deleteSelected();
  assert.deepEqual(calls.filter((call) => call[1]?.op === 'localProp').at(-1)[1].kind, 'remove');
});

test('a door of yours turns about its hinge, lifts and slides along its own width', () => {
  const {
    moverPose,
    moverPath,
    moverUse,
    checkedMover,
  } = require('../../../../Client/resources/architect/movers.cjs');
  // A door leaf 1 m wide, 10 cm thick, 2 m tall, standing at (10, 0, 0) turned 0.
  const box = { min: [-0.5, -0.05, 0], max: [0.5, 0.05, 2] };
  const at = { x: 10, y: 0, z: 0 };
  const near = (a, b) => Math.abs(a - b) < 1e-9;
  const door = checkedMover({ hinge: 'left', turn: 90 });
  const open = moverPose(at, 0, box, door, 1);
  // Its left edge (9.5, 0) stays; its middle swings to (9.5, 0.5).
  assert.ok(near(open.at.x, 9.5) && near(open.at.y, 0.5) && near(open.at.z, 0));
  assert.equal(open.turn, 90);
  const half = moverPose(at, 0, box, door, 0.5);
  assert.ok(near(Math.hypot(half.at.x - 9.5, half.at.y), 0.5), 'on the arc about the hinge');
  assert.equal(moverPath(at, 0, box, door).length, 3, 'a 90° turn in steps of 30°');
  // The same door turned 90° in the world: its left edge is then at (10, -0.5), and open its
  // middle is half a metre to the west of it.
  const turned = moverPose(at, 90, box, door, 1);
  assert.ok(near(turned.at.x, 9.5) && near(turned.at.y, -0.5));
  // A portcullis: no turn, 3 m up in one glide; a sliding gate: along its width.
  const gate = checkedMover({ turn: 0, lift: 3 });
  assert.deepEqual(moverPath(at, 0, box, gate), [{ at: { x: 10, y: 0, z: 3 }, turn: 0 }]);
  const slider = moverPose(at, 0, box, checkedMover({ turn: 0, slide: 1.5 }), 1);
  assert.ok(near(slider.at.x, 11.5) && near(slider.at.y, 0));
  // Used at the foot of its middle, as far around as its footprint reaches and as tall as it is.
  const use = moverUse(at, 0, box);
  assert.deepEqual(use.at, { x: 10, y: 0, z: 0 });
  assert.equal(use.height, 2);
  // Out-of-range values are kept in range; no object is no mover.
  assert.equal(checkedMover({ turn: 500, seconds: 0 }).turn, 180);
  assert.equal(checkedMover({ seconds: 0 }).seconds, 0.5);
  assert.equal(checkedMover(null), null);
});

test('a prop made a door is kept in the project, previewed, and exported with its way open', () => {
  const { editor } = setup();
  editor.api.propMeshes = () => ['objects/manmade/common_furniture/tables/table_a.cgf'];
  editor.catalogue.propMeshes = editor.api.propMeshes;
  editor.api.propBounds = () => ({ min: [-0.5, -0.05, 0], max: [0.5, 0.05, 2] });
  editor.open();
  editor.useMode('props');
  assert.ok(editor.choose('objects/manmade/common_furniture/tables/table_a.cgf'));
  editor.pressWorld(VIEW_X, VIEW_Y);
  const prop = editor.placed[0];
  const rows = () => editor.moverRows(prop);
  assert.equal(rows().length, 2, 'a still prop: only the switch');
  rows()[1].press();
  assert.ok(prop.mover);
  const row = (label) => rows().find((one) => one.label === label);
  row('Closes after').step(1);
  assert.equal(prop.mover.closeAfter, 11);
  row('Hinge').step(1);
  assert.equal(prop.mover.hinge, 'right');
  assert.deepEqual(editor.project().props[0].mover, { ...prop.mover });
  // The preview moves it in this game to its open pose over its seconds.
  const sent = [];
  prop.handle = { setPose: (position, turn) => sent.push([position, turn.z]) };
  const now = Date.now;
  try {
    Date.now = () => 1000;
    editor.previewMover(prop, true);
    Date.now = () => 1000 + prop.mover.seconds * 1000;
    prop.moverStop?.();
    editor.previewMover(prop, true);
  } finally {
    Date.now = now;
  }
  assert.equal(prop.moverOpen, true);
  // Undo takes the mover away and shows it closed again.
  editor.undo();
  editor.undo();
  editor.undo();
  assert.equal(prop.mover, undefined);
  editor.redo();
  const work = editor.work();
  const door = work.props[0].door;
  assert.equal(door.path.length, 3);
  assert.equal(door.seconds, 1.5);
  assert.equal(door.closeAfter, 6);
  assert.equal(door.use.height, 2);
  assert.ok(door.path.every((pose) => Number.isFinite(pose.rotation.w)));
});

test("a level mesh's Make it open puts your copy in its place, and the copy opens", () => {
  const { editor, world } = setup();
  editor.api.meshKnown = () => true;
  editor.open();
  world.hit = {
    position: { x: 50, y: 60, z: 7 },
    kind: 'brush',
    levelObject: {
      kind: 'brush',
      name: 'objects/manmade/common_fixtures/doors/gate_a_doors.cgf',
      at: { x: 50, y: 60, z: 7 },
    },
  };
  editor.useTool('select');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const level = editor.selected;
  assert.ok(level?.level);
  const row = editor.levelDetails(level).rows.find((one) => one.label === 'Make it open');
  row.press();
  const copy = editor.selected;
  assert.equal(copy.entry.source, 'prop');
  assert.equal(copy.entry.name, 'objects/manmade/common_fixtures/doors/gate_a_doors.cgf');
  assert.deepEqual(copy.mover, {
    hinge: 'left',
    turn: 90,
    lift: 0,
    slide: 0,
    seconds: 1.5,
    closeAfter: 6,
  });
  assert.ok(level.hidden, "the level's own is hidden");
  assert.ok(editor.project().props[0].mover);
});

test('an exported door opens on use along its way, closes by itself, and its prompt says which', () => {
  const vm = require('node:vm');
  const { serverScript } = require('../../../../Client/resources/architect/server_export.cjs');
  const { Quaternion } = require('./math.cjs');
  const door = {
    name: 'objects/doors/door_wood_a.cgf',
    at: { x: 10, y: 0, z: 0 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    scale: 1,
    door: {
      path: [
        { at: { x: 9.6, y: 0.2, z: 0 }, rotation: { x: 0, y: 0, z: 0.5, w: 0.866 } },
        { at: { x: 9.5, y: 0.5, z: 0 }, rotation: { x: 0, y: 0, z: 0.707, w: 0.707 } },
      ],
      seconds: 1,
      closeAfter: 5,
      use: { at: { x: 10, y: 0, z: 0 }, reach: 0.5, height: 2 },
    },
  };
  const work = { map: 'trosecko', buildings: [], props: [door], levelEdits: [], areas: [] };
  Object.assign(work, { effects: [], lights: [], people: [], spawns: [] });
  const glides = [];
  const texts = [];
  let use = null;
  let interaction = null;
  // Timers run when the test says, in the order of their times.
  let clock = 0;
  let timers = [];
  const setTimeout = (callback, ms = 0) => {
    const timer = { at: clock + ms, callback };
    timers.push(timer);
    return timer;
  };
  const clearTimeout = (timer) => {
    timers = timers.filter((one) => one !== timer);
  };
  const runUntil = (until) => {
    for (;;) {
      timers.sort((a, b) => a.at - b.at);
      const next = timers[0];
      if (!next || next.at > until) {
        break;
      }
      timers.shift();
      clock = next.at;
      next.callback();
    }
    clock = until;
  };
  const kcds = {
    world: {
      Prop: {
        spawn: () => ({
          glideTo: (at, turn, options) => glides.push([at.x, at.y, turn.z, options.ms]),
        }),
      },
      LevelEdit: { apply() {} },
      Area: { create() {} },
      Vfx: { spawn() {} },
      Light: { spawn() {} },
      World: {
        addInteraction: (options, handler) => {
          interaction = options;
          use = handler;
          return { setText: (text) => texts.push(text) };
        },
      },
    },
    characters: { Npc: { create() {} } },
    math: { Vector3, Quaternion },
    core: { Events: { on() {} } },
  };
  vm.runInNewContext(serverScript(), {
    kcds,
    console: { info() {}, warn() {} },
    setTimeout,
    clearTimeout,
    require: () => work,
  });
  assert.equal(interaction.text, 'Open');
  assert.equal(interaction.targetHeight, 2);
  use();
  use();
  runUntil(1000);
  // Two steps of half a second each, once: a second use while it moves does nothing.
  assert.deepEqual(
    glides.map((glide) => [glide[0], glide[1], glide[3]]),
    [
      [9.6, 0.2, 500],
      [9.5, 0.5, 500],
    ],
  );
  assert.deepEqual(texts, ['Close']);
  // Five seconds later it closes by itself, back along its way to where it stands closed.
  runUntil(7000);
  assert.deepEqual(
    glides.slice(2).map((glide) => [glide[0], glide[1]]),
    [
      [9.6, 0.2],
      [10, 0],
    ],
  );
  assert.deepEqual(texts, ['Close', 'Open']);
  // Used again while open, it closes at once and no timer closes it a second time.
  use();
  runUntil(8000);
  use();
  runUntil(20000);
  assert.equal(glides.length, 8);
  assert.deepEqual(texts, ['Close', 'Open', 'Close', 'Open']);
});

test("an AI assistant picks the level's own object at a point and moves it as your copy", () => {
  const { editor, world } = setup();
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  ask(1, 'open');
  world.hit = {
    position: { x: 50, y: 60, z: 7 },
    kind: 'vegetation',
    levelObject: {
      kind: 'brush',
      name: 'objects/natural/trees/oak/oak_a.cgf',
      at: { x: 50.2, y: 59.9, z: 6.5 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
    },
  };
  const picked = ask(2, 'pick', { at: { x: 50, y: 60 } });
  assert.equal(picked.ok, true, picked.error);
  assert.equal(picked.result.kind, 'level');
  const moved = ask(3, 'move', { id: picked.result.id, at: { x: 55, y: 60, z: 6.5 } });
  assert.equal(moved.ok, true, moved.error);
  assert.equal(moved.result.kind, 'vegetation');
  assert.deepEqual(moved.result.at, { x: 55, y: 60, z: 6.5 });
  assert.equal(editor.hiddenLevel.length, 1);
  // Turned with the Rotate tool, a level object's copy stands turned by the angle step.
  ask(4, 'undo');
  editor.select(editor.thingAt({ x: 50, y: 60, z: 7 }, world.hit));
  editor.useTool('rotate');
  editor.useToolAt({ x: 50, y: 60, z: 7 });
  assert.equal(editor.placed.length, 1);
  assert.equal(editor.placed[0].turn, 15);
});

test('a placed building hides the grass carpet under its footprint; undo brings it back', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const grass = () =>
    calls.filter((call) => call[1]?.op === 'architectGrass').map((call) => call[1]);
  const hidden = grass().filter((message) => message.kind === 'hide');
  assert.ok(hidden.length >= 1);
  assert.ok(hidden.every((message) => message.radius >= 1));
  editor.undo();
  const shown = grass()
    .filter((message) => message.kind === 'show')
    .map((message) => message.id);
  assert.deepEqual(
    shown,
    hidden.map((message) => message.id),
  );
});

test('the Scale tool grows a plant a step, planted again at that size; undo takes it back', () => {
  const { editor, calls } = setup();
  const sizes = [];
  editor.api.Vegetation.plant = (name, at, options) => {
    sizes.push(options.scale ?? 1);
    return { remove() {} };
  };
  editor.open();
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const plant = editor.placed[0];
  editor.select(plant);
  editor.useTool('scale');
  editor.useToolAt(plant.at);
  assert.equal(plant.scale, 1.1);
  assert.equal(sizes.at(-1), 1.1);
  assert.ok(editor.details().rows.some((row) => row.label === 'Scale' && row.value === '×1.10'));
  editor.undo();
  assert.equal(plant.scale, 1);
  assert.equal(sizes.at(-1), 1);
  assert.ok(calls);
});

test('a Details section folds shut on its header and opens again; an Outliner double press flies there', () => {
  const { editor, calls } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const labels = () => editor.details().rows.map((row) => row.label);
  assert.ok(labels().includes('Location X'));
  const header = editor.details().rows.findIndex((row) => row.label === 'TRANSFORM');
  editor.pressDetail(header, 0);
  assert.ok(!labels().includes('Location X'));
  assert.equal(editor.details().rows[header].folded, true);
  editor.pressDetail(header, 0);
  assert.ok(labels().includes('Location X'));
  // Two presses on the house's Outliner line: the camera flies to it.
  const row = editor.outlinerLines().findIndex((line) => line.thing === editor.placed[0]);
  editor.pressOutliner(row);
  editor.pressOutliner(row);
  assert.ok(calls.some((call) => call[0] === 'focus'));
});

test('dragging sideways on a value steps it on as the cursor goes, one undo step', () => {
  const { editor, down, tick } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const house = editor.placed[0];
  const steps = editor.undoStack.length;
  const row = editor.details().rows.findIndex((entry) => entry.label === 'Rotation');
  editor.testCursor = { x: 1600, y: 300 };
  down.add('mouse1');
  editor.pressDetail(row, 0);
  editor.testCursor = { x: 1636, y: 300 };
  tick();
  assert.equal(house.turn, 45);
  down.delete('mouse1');
  tick();
  assert.equal(editor.undoStack.length, steps + 1);
  editor.undo();
  assert.equal(house.turn, 0);
});

test('the Rotate tool shows a ring; dragging round it turns the selection in angle steps, one step', () => {
  const { editor, down, tick } = setup();
  editor.open();
  // A camera looking down: a world point (x, y) shows at stage (960 + 10 x, 540 - 10 y).
  editor.api.NoClip.toScreen = (point) => ({ x: (point.x * 10) / 960, y: (point.y * 10) / 540 });
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const house = editor.placed[0];
  editor.useTool('rotate');
  // As refresh() does with the panels built (the stand-ins build none).
  editor.marks.ringMode = true;
  editor.marks.select(editor.boxOf(house));
  assert.ok(editor.marks.ring);
  assert.equal(editor.marks.arrows.length, 0);
  const { center, radius } = editor.marks.ring;
  // Press on the ring's east point, then go a quarter round (to its north point).
  const east = { x: 960 + (center.x + radius) * 10, y: 540 - center.y * 10 };
  const north = { x: 960 + center.x * 10, y: 540 - (center.y + radius) * 10 };
  const steps = editor.undoStack.length;
  down.add('mouse1');
  assert.equal(editor.grabArrow(east.x, east.y), true);
  editor.testCursor = north;
  tick();
  down.delete('mouse1');
  tick();
  assert.equal(house.turn, 90);
  assert.equal(editor.undoStack.length, steps + 1);
});

test('the Scale tool held keeps growing; moved, it grows or shrinks; all of it one undo step', () => {
  const { editor, down, tick } = setup();
  editor.open();
  editor.choose('objects/natural/trees/oak/oak_a.cgf');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const plant = editor.placed[0];
  editor.select(plant);
  editor.useTool('scale');
  const steps = editor.undoStack.length;
  editor.testCursor = { x: 500, y: 500 };
  down.add('mouse1');
  editor.useToolAt(plant.at);
  assert.equal(plant.scale, 1.1);
  // Held still past the delay: another step.
  editor.scaleHold.next = 0;
  tick();
  assert.equal(plant.scale, 1.21);
  // Moved two steps to the right: two more; then back left past the press: smaller.
  editor.testCursor = { x: 524, y: 500 };
  tick();
  assert.equal(plant.scale, 1.46);
  editor.testCursor = { x: 488, y: 500 };
  tick();
  assert.equal(plant.scale, 1.1);
  down.delete('mouse1');
  tick();
  assert.equal(editor.undoStack.length, steps + 1);
  editor.undo();
  assert.equal(plant.scale, 1);
});

test('a prop shows X and Y rings with Rotate; a tilt about its own axis is one undo step', () => {
  const { editor, calls } = setup();
  editor.open();
  const mesh = 'objects/manmade/common_furniture/barrels/barrel_a.cgf';
  const record = {
    entry: { label: 'barrel_a', name: mesh, source: 'prop' },
    at: { x: 4, y: 5, z: 1 },
    turn: 0,
    pad: null,
  };
  record.handle = editor.propHandle(mesh, record.at, 0);
  editor.placed.push(record);
  editor.select(record);
  editor.marks.ringMode = true;
  editor.marks.select(editor.boxOf(record));
  assert.deepEqual(
    editor.marks.rings.map((ring) => ring.axis),
    ['z', 'x', 'y'],
  );
  editor.tiltSelected('x', 90);
  const half = Math.SQRT1_2;
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  const rotation = sent.at(-1).rotation;
  assert.ok(Math.abs(rotation.x - half) < 1e-9 && Math.abs(rotation.w - half) < 1e-9);
  assert.ok(Math.abs(record.tilt.x - half) < 1e-9);
  editor.undo();
  assert.equal(record.tilt, null);
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1].rotation, {
    x: 0,
    y: 0,
    z: 0,
    w: 1,
  });
});

test("Effects places the game's particle effects, moved and kept in the project", () => {
  const { editor } = setup();
  const running = [];
  editor.api.Vfx = {
    list: () => ['WH_Particels.fires.candle', 'WH_Particels.weather.lightning_1'],
    spawn: (name, position) => {
      const effect = {
        name,
        position,
        destroyed: false,
        destroy() {
          effect.destroyed = true;
        },
      };
      running.push(effect);
      return effect;
    },
  };
  editor.open();
  editor.useMode('effects');
  assert.deepEqual(
    editor.itemsOf('fire').map((item) => item.name),
    ['candle'],
  );
  assert.ok(editor.choose('WH_Particels.weather.lightning_1'));
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(running.at(-1).name, 'WH_Particels.weather.lightning_1');
  assert.equal(editor.selectionKind(), 'effect');
  editor.moveSelected({ x: 1, y: 2, z: 3 }, 0);
  assert.equal(running.at(-2).destroyed, true);
  assert.equal(running.at(-1).position.x, 1);
  assert.equal(editor.project().effects.length, 1);
  editor.deleteSelected();
  assert.equal(running.at(-1).destroyed, true);
});

test('Lights in Effects: a torch placed as a point light, its intensity stepped as one undo step', () => {
  const { editor } = setup();
  const lights = [];
  editor.api.Light = {
    spawn: (position, settings) => {
      const light = {
        position,
        ...settings,
        destroyed: false,
        destroy() {
          light.destroyed = true;
        },
      };
      lights.push(light);
      return light;
    },
  };
  editor.open();
  editor.useMode('effects');
  assert.ok(editor.itemsOf('lights').some((item) => item.name === 'Torch'));
  assert.ok(editor.choose('torch'));
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(lights.length, 1);
  assert.equal(lights[0].radius, 7);
  assert.equal(editor.selectionKind(), 'light');
  const row = editor.details().rows.findIndex((entry) => entry.label === 'Intensity');
  editor.pressDetail(row, 1);
  assert.equal(lights[0].intensity, 4.5);
  editor.undo();
  assert.equal(lights[0].intensity, 4);
  assert.equal(editor.project().lights.length, 1);
});

test('Sounds in Effects: a world loop placed at a point, moved, undone and kept in the project', () => {
  const { editor, calls } = setup();
  editor.api.worldSounds = () => [
    { name: 'a_l_village_amb01', group: 'ambience' },
    { name: 'a_l_bees_amb', group: 'objects' },
  ];
  const sounds = () =>
    calls
      .filter((call) => call[0] === 'send' && String(call[1].op).startsWith('localSound'))
      .map((call) => call[1]);
  editor.open();
  assert.deepEqual(
    editor.itemsOf('sounds').map((item) => item.name),
    ['village amb01', 'bees amb'],
  );
  assert.ok(editor.choose('a_l_bees_amb'));
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.selectionKind(), 'sound');
  const [start] = sounds();
  assert.equal(start.op, 'localSoundStart');
  assert.equal(start.trigger, 'a_l_bees_amb');
  assert.deepEqual(start.at, { x: 10, y: 20, z: 5 });
  // Moved: the same sound goes along.
  editor.moveSelected({ x: 14, y: 20, z: 5 }, 0);
  assert.deepEqual(sounds().at(-1), {
    op: 'localSoundMove',
    id: start.id,
    at: { x: 14, y: 20, z: 5 },
  });
  // Kept in the project, and played again when it is built.
  assert.deepEqual(editor.project().sounds, [{ name: 'a_l_bees_amb', at: { x: 14, y: 20, z: 5 } }]);
  assert.equal(
    editor.outlinerLines().some((line) => line.group && line.name === 'Sounds'),
    true,
  );
  // Undone (the move, then the placing): it stops.
  editor.undo();
  editor.undo();
  assert.equal(sounds().at(-1).op, 'localSoundStop');
  assert.equal(editor.placed.length, 0);
  editor.redo();
  assert.equal(sounds().at(-1).op, 'localSoundStart');
  // Exported for a server: each player's game keeps it playing there (Audio.loopAt).
  const vm = require('node:vm');
  const written = {};
  editor.api.exportFiles = (folder, files) => {
    for (const file of files) {
      written[file.name] = file.append ? written[file.name] + file.text : file.text;
    }
    return 'C:/exports/' + folder;
  };
  editor.exportForServer();
  const work = JSON.parse(written['work.json']);
  assert.deepEqual(work.sounds, [{ name: 'a_l_bees_amb', at: { x: 10, y: 20, z: 5 } }]);
  const looped = [];
  const listeners = {};
  const ignored = () => {};
  vm.runInNewContext(written['client.js'], {
    kcds: {
      world: {
        Terrain: { flatten: ignored, raise: ignored },
        Vegetation: { plant: ignored },
        Collision: { addBox: ignored },
        Audio: { loopAt: (name, at) => looped.push([name, at.x, at.y, at.z]) },
      },
      math: { Vector3 },
      core: { Events: { on: (name, handler) => (listeners[name] = handler) } },
    },
    console: { info() {}, warn() {} },
    setTimeout: (callback) => callback(),
    require: () => work,
  });
  listeners.playerReady();
  assert.deepEqual(looped, [['a_l_bees_amb', 10, 20, 5]]);
});

test('People: a preview NPC placed, sent walking with a press, its walk shown; never kept', () => {
  const { editor, calls } = setup();
  let walk = 'walking';
  editor.api.npcWalkState = () => walk;
  const npc = () =>
    calls
      .filter((call) => call[0] === 'send' && String(call[1].op).startsWith('localNpc'))
      .map((call) => call[1]);
  editor.open();
  assert.deepEqual(
    editor.itemsOf('people').map((item) => item.name),
    ['Man', 'Woman'],
  );
  assert.ok(editor.choose('npc_man'));
  editor.pressWorld(VIEW_X, VIEW_Y);
  const [create] = npc();
  assert.equal(create.op, 'localNpcCreate');
  assert.equal(create.actorClass, 'NPC');
  assert.deepEqual(create.position, { x: 10, y: 20, z: 5 });
  assert.equal(editor.selectionKind(), 'npc');
  // Walk to: the next press in the world is where it walks.
  const rows = editor.details().rows;
  rows.find((row) => row.label === 'Walk to').press();
  editor.pressWorld(VIEW_X, VIEW_Y + 100);
  const order = npc().at(-1);
  assert.equal(order.op, 'localNpcWalk');
  assert.equal(order.id, create.id);
  assert.equal(editor.placed.length, 1);
  // The game says there is no way there (no navmesh): the editor tells.
  walk = 'no path';
  editor.poll();
  assert.match(editor.message, /no navmesh/);
  assert.equal(
    editor.details().rows.find((row) => row.label === 'Walk').value,
    'No path (no navmesh)',
  );
  // A preview, not part of the work.
  assert.equal(JSON.stringify(editor.project()).includes('npc_man'), false);
  editor.undo();
  assert.equal(npc().at(-1).op, 'localNpcRemove');
});

test('Bake navmesh on a custom map: the ground without your buildings, for the next start', () => {
  const { editor } = setup();
  let asked = null;
  editor.api.bakeNavmesh = (blocked) => {
    asked = blocked;
    return { tiles: 4, walkable: 60, leftOut: 4 };
  };
  editor.open();
  // Only a custom map bakes; a game map's section says it has the game's own.
  const rowsNow = () => editor.details().rows;
  assert.ok(rowsNow().some((row) => row.label === 'NAVMESH'));
  assert.ok(rowsNow().some((row) => row.label === 'Game map'));
  assert.equal(
    rowsNow().some((row) => row.label === 'Bake'),
    false,
  );
  editor.map = 'kcds_map_prague';
  assert.ok(rowsNow().some((row) => row.label === 'Bake'));
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  editor.select(null);
  editor
    .details()
    .rows.find((row) => row.label === 'Bake')
    .press();
  assert.equal(asked.length, 1);
  const [house] = asked;
  assert.ok(house.minX < 10 && house.maxX > 10 && house.minY < 20 && house.maxY > 20);
  assert.match(editor.message, /next start/);
});

test("the editor takes the window's shape: panels keep their size, the world view takes the rest", () => {
  const theme = require('../../../../Client/resources/architect/theme.cjs');
  const { LAYOUT, stageFor, fitStage } = theme;
  try {
    // A wide window: 1080 high, wider; the details stay 384 wide at the right end.
    const wide = stageFor(2560, 1080);
    assert.deepEqual(wide, { width: 2560, height: 1080 });
    assert.equal(fitStage(wide.width, wide.height), true);
    assert.equal(LAYOUT.details.x, 2560 - 384);
    assert.equal(LAYOUT.details.width, 384);
    assert.equal(LAYOUT.view.width, 2560 - 326 - 384);
    // A narrow window (1200 by 1000): the stage is made higher so the panels still fit.
    const narrow = stageFor(1200, 1000);
    assert.ok(narrow.width >= 1700);
    assert.ok(narrow.height > 1080);
    assert.ok(Math.abs(narrow.width / narrow.height - 1.2) < 0.01);
    fitStage(narrow.width, narrow.height);
    assert.equal(LAYOUT.status.y, narrow.height - 30);
    assert.equal(LAYOUT.dock.y + LAYOUT.dock.height, narrow.height - 30);
    assert.equal(LAYOUT.outliner.width, 326);
  } finally {
    fitStage(1920, 1080);
  }
});

test("a press on the level's table in your house takes the table, and hovering names it", () => {
  const { editor, world } = setup();
  editor.open();
  editor.choose('structures/living/houses/house_1s_2r_a');
  editor.pressWorld(VIEW_X, VIEW_Y);
  const house = editor.placed[0];
  editor.useTool('select');
  editor.select(null);
  // The level's table a metre from your house's foot: the ray met it, so it is taken.
  world.hit = {
    position: { x: house.at.x + 1, y: house.at.y + 1, z: house.at.z + 0.8 },
    kind: 'brush',
    levelObject: {
      kind: 'brush',
      name: 'objects/manmade/common_furniture/tables/table_shabby_d_160_rough.cgf',
      at: { x: house.at.x + 1, y: house.at.y + 1, z: house.at.z },
    },
  };
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.state().selected, 'table_shabby_d_160_rough');
  // Hovering over it names it beside the cursor.
  editor.select(null);
  editor.testCursor = { x: VIEW_X, y: VIEW_Y };
  editor.markTarget();
  assert.deepEqual(
    { label: editor.hover.label, kind: editor.hover.kind },
    { label: 'table_shabby_d_160_rough', kind: 'Level object' },
  );
  // A ray that met nothing of the level near your house still takes the house.
  world.hit = { position: { x: house.at.x + 1, y: house.at.y, z: house.at.z }, levelObject: null };
  editor.sightChanged();
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.selected, house);
});

test("a level object's tilt ring makes it your copy, which tilts as one step; the level's is hidden", () => {
  const { editor, calls, world, down, tick } = setup();
  editor.api.meshKnown = () => true;
  editor.open();
  // A camera looking from the side: a world point (x, z) shows at stage (960 + 10 x, 540 - 10 z).
  editor.api.NoClip.toScreen = (point) => ({ x: (point.x * 10) / 960, y: (point.z * 10) / 540 });
  world.hit = {
    position: { x: 4, y: 5, z: 1.5 },
    kind: 'brush',
    levelObject: {
      kind: 'brush',
      name: 'objects/manmade/common_furniture/benches/low/bench_shabby_c.cgf',
      at: { x: 4, y: 5, z: 1 },
    },
  };
  editor.useTool('select');
  editor.pressWorld(VIEW_X, VIEW_Y);
  assert.equal(editor.selectionKind(), 'level');
  editor.useTool('rotate');
  editor.marks.ringMode = true;
  editor.marks.select(editor.boxOf(editor.selected));
  const ring = editor.marks.rings.find((entry) => entry.axis === 'y');
  assert.ok(ring);
  // The Y ring stands in the X-Z plane: its top, then a quarter round to its east side.
  const { center, radius } = ring;
  const top = { x: 960 + center.x * 10, y: 540 - (center.z + radius) * 10 };
  const east = { x: 960 + (center.x + radius) * 10, y: 540 - center.z * 10 };
  down.add('mouse1');
  assert.equal(editor.grabArrow(top.x, top.y), true);
  // The level's bench is hidden and your copy stands where it stood, selected.
  assert.equal(editor.selectionKind(), 'prop');
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  assert.ok(sent.some((message) => message.op === 'localLevelEdit' && message.hidden === true));
  const copy = editor.selected;
  editor.testCursor = east;
  tick();
  down.delete('mouse1');
  tick();
  // A quarter round: tilted 90 degrees (about the ring the press was nearest; seen from the side
  // the X ring lies over the same points), as one undo step.
  const tilted = Math.max(Math.abs(copy.tilt.x), Math.abs(copy.tilt.y));
  assert.ok(Math.abs(tilted - Math.SQRT1_2) < 1e-6);
  assert.match(editor.message, /tilted -?90°/);
});

test('People sent walking in the editor are exported as server NPCs that walk that path back and forth', () => {
  const vm = require('node:vm');
  const { editor } = setup();
  const written = {};
  editor.api.exportFiles = (folder, files) => {
    for (const file of files) {
      written[file.name] = file.append ? written[file.name] + file.text : file.text;
    }
    return `C:\exports\${folder}`;
  };
  editor.open();
  const record = {
    entry: { label: 'Woman', name: 'npc_woman', source: 'npc' },
    at: { x: 10, y: 20, z: 5 },
    turn: 90,
    handle: {},
    npcId: 1000001,
  };
  editor.placed.push(record);
  editor.walkTo(record, { x: 30, y: 25, z: 6 });
  const answer = editor.exportForServer();
  assert.equal(answer.people, 1);
  const created = [];
  const walks = [];
  const kcds = {
    world: {
      Prop: { spawn() {} },
      LevelEdit: { apply() {} },
      Area: { create() {} },
      Vfx: { spawn() {} },
      Light: { spawn() {} },
    },
    characters: {
      Npc: {
        create: (options) => {
          created.push(options);
          return { patrol: (points, how) => walks.push({ points, how }) };
        },
      },
    },
    math: { Vector3: Vector3 },
    core: { Events: { on() {} } },
  };
  vm.runInNewContext(written['server.js'], {
    kcds,
    console: { info() {}, warn() {} },
    require: (path) => (path === './work.json' ? JSON.parse(written['work.json']) : null),
  });
  assert.equal(created.length, 1);
  assert.equal(created[0].class, 'NPC_Female');
  assert.deepEqual(
    { x: created[0].position.x, y: created[0].position.y, z: created[0].position.z },
    { x: 10, y: 20, z: 5 },
  );
  // Back and forth between where it stood and where it was sent.
  assert.equal(walks.length, 1);
  const [from, to] = walks[0].points;
  assert.deepEqual({ x: from.x, y: from.y, z: from.z }, { x: 10, y: 20, z: 5 });
  assert.deepEqual({ x: to.x, y: to.y, z: to.z }, { x: 30, y: 25, z: 6 });
  assert.equal(walks[0].how.mode, 'pingPong');
  assert.equal(walks[0].how.speed, 'walk');
});

test("an AI assistant reads a picked level entity's properties and sets one", () => {
  const { editor, calls, world } = setup();
  editor.api.entityProperties = () => [
    { key: 'Lock.bLocked', type: 'bool', value: true },
    { key: 'object_Model', type: 'text', value: 'objects/doors/door_a.cgf' },
  ];
  world.hit = {
    position: { x: 80, y: 90, z: 3 },
    kind: 'entity',
    levelObject: {
      kind: 'entity',
      name: 'door_1',
      class: 'AnimDoor',
      guid: '0123456789abcdef',
      at: { x: 80, y: 90, z: 3 },
    },
  };
  editor.open();
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: [],
    WEATHERS: [],
    WINDS: [],
  });
  const run = (command, args) => JSON.parse(ai.handle(JSON.stringify({ command, args })));
  const picked = run('pick', { at: { x: 80, y: 90 } });
  assert.equal(picked.ok, true);
  const listed = run('properties', { id: picked.result.id });
  assert.deepEqual(
    listed.result.map((entry) => entry.key),
    ['Lock.bLocked', 'object_Model'],
  );
  const set = run('set_property', { id: picked.result.id, key: 'Lock.bLocked', value: false });
  assert.deepEqual(set.result, { key: 'Lock.bLocked', value: false });
  assert.deepEqual(calls.filter((call) => call[0] === 'send').at(-1)[1], {
    op: 'localEntityProperty',
    guid: '0123456789abcdef',
    key: 'Lock.bLocked',
    value: false,
  });
  // A text is only shown, and an unknown key is refused.
  assert.equal(
    run('set_property', { id: picked.result.id, key: 'object_Model', value: 1 }).ok,
    false,
  );
  assert.equal(run('set_property', { id: picked.result.id, key: 'nope', value: true }).ok, false);
});

test('an AI assistant places a zone (a spawn area) and sizes it, as the Zones mode does', () => {
  const { editor } = setup();
  editor.open();
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: [],
    WEATHERS: [],
    WINDS: [],
  });
  const run = (command, args) => JSON.parse(ai.handle(JSON.stringify({ command, args })));
  const placed = run('place', { name: 'spawn', at: { x: 12, y: 18, z: 5 } });
  assert.equal(placed.ok, true, placed.error);
  const sized = run('shape_size', { id: placed.result.id, size: { x: 8, y: 6 } });
  assert.equal(sized.ok, true, sized.error);
  const zone = editor.project().shapes.find((shape) => shape.kind === 'spawn');
  assert.ok(zone);
  assert.equal(zone.size.x, 8);
  assert.equal(zone.size.y, 6);
});

// The ground's look (World Architect texturing): the AI paints a road as one stroke, dabs half a
// brush radius apart along its whole line; the project keeps it and an export repaints it.
test('an AI assistant paints a cobbled road as one undo step, kept in the project', () => {
  const { editor, calls } = setup();
  editor.open();
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: [],
    WEATHERS: [],
    WINDS: [],
  });
  const run = (command, args) => JSON.parse(ai.handle(JSON.stringify({ command, args })));
  assert.ok(run('materials').result.includes('cobblestones'));
  const painted = run('paint', {
    material: 'cobblestones',
    points: [
      { x: 0, y: 0, z: 5 },
      { x: 10, y: 0, z: 5 },
    ],
    size: 4,
  });
  assert.equal(painted.ok, true, painted.error);
  assert.equal(painted.result.steps, 1);
  assert.equal(painted.result.dabs, 6);
  const dabs = calls.filter((call) => call[0] === 'paint');
  assert.equal(dabs.length, 6);
  assert.deepEqual(dabs[0].slice(1), [0, 0, 2, 'cobblestones', 2]);
  assert.equal(editor.project().terrain[0].mode, 'paint:cobblestones');
  assert.equal(run('paint', { material: 'lava', points: [{ x: 0, y: 0 }] }).ok, false);
  editor.undo();
  assert.equal(editor.terrainEdits.length, 0);
});

// The custom maps on this PC (Dev/tools/architect/make_custom_map.py) are opened like the game's
// own: in the map choice and through an AI assistant's switch_map.
test('custom maps the game lists are in the map choice and the AI opens one', () => {
  const { editor, calls } = setup();
  editor.api.customMaps = () => ['kcds_map_praha', 'not a map'];
  editor.open();
  const keys = editor.allMaps().map((map) => map.key);
  assert.deepEqual(keys, ['trosecko', 'kutnohorsko', 'klaster', 'kcds_map_praha']);
  assert.equal(editor.allMaps()[3].label, 'Custom: praha');
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const labels = Array.from({ length: 10 }, () => ({ label: 'any' }));
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: labels,
    WEATHERS: labels,
    WINDS: labels,
  });
  const run = (command, args) => JSON.parse(ai.handle(JSON.stringify({ command, args })));
  assert.ok(run('state').result.maps.includes('kcds_map_praha'));
  assert.equal(run('switch_map', { map: 'kcds_map_nowhere' }).ok, false);
  assert.equal(run('switch_map', { map: 'kcds_map_praha' }).ok, true);
  const sent = calls.filter((call) => call[0] === 'send').map((call) => call[1]);
  assert.deepEqual(sent.at(-1), { op: 'architectMap', map: 'kcds_map_praha' });
});

// A door in a wall is never under a straight-down pick: an AI picks it along a line, as a click
// from a camera, and the answer says it is an entity of the level (its class).
test('an AI picks a level door along a line and its answer names the entity class', () => {
  const { editor, world } = setup();
  const lines = [];
  editor.api.World.raycast = (from, to) => {
    lines.push([from, to]);
    return world.hit;
  };
  world.hit = {
    position: { x: 80, y: 90, z: 3 },
    kind: 'entity',
    levelObject: {
      kind: 'entity',
      name: 'door_1',
      class: 'AnimDoor',
      guid: '0123456789abcdef',
      at: { x: 80, y: 90, z: 3 },
    },
  };
  editor.open();
  const { AiCommands } = require('../../../../Client/resources/architect/ai_commands.cjs');
  const ai = new AiCommands(editor, {
    MODES: [],
    MAPS: [],
    CLOCK_SPEEDS: [],
    WEATHERS: [],
    WINDS: [],
  });
  const run = (command, args) => JSON.parse(ai.handle(JSON.stringify({ command, args })));
  const picked = run('pick', { from: { x: 76, y: 90, z: 4 }, to: { x: 84, y: 90, z: 4 } });
  assert.equal(picked.ok, true, picked.error);
  assert.equal(picked.result.entity, 'AnimDoor');
  assert.deepEqual(lines.at(-1), [
    { x: 76, y: 90, z: 4 },
    { x: 84, y: 90, z: 4 },
  ]);
  world.hit = null;
  assert.equal(run('pick', { from: { x: 0, y: 0, z: 4 }, to: { x: 1, y: 0, z: 4 } }).ok, false);
});

test('AI: the catalogue gives buildings their size, place_many skips overlaps, fill_row lines a street', () => {
  const { editor } = setup();
  const facade = architectFacade(editor);
  const ask = (id, command, args = {}) =>
    JSON.parse(facade.command(JSON.stringify({ id, command, args })));
  ask(1, 'open');
  const house = 'structures/living/houses/house_1s_2r_a';
  const found = ask(2, 'catalogue', { query: 'house_1s_2r_a', folder: 'houses' }).result;
  const row = found.items.find((item) => item.name === house);
  assert.ok(row.size.width > 0 && row.size.depth > 0 && row.size.height > 0);
  const tooNarrow = ask(3, 'catalogue', { query: 'house_1s_2r_a', maxWidth: 0.5 }).result;
  assert.equal(tooNarrow.total, 0);
  const many = ask(4, 'place_many', {
    items: [
      { name: house, at: { x: 10, y: 10, z: 0 } },
      { name: house, at: { x: 10.5, y: 10, z: 0 } },
      { name: house, at: { x: 60, y: 10, z: 0 } },
    ],
    avoidOverlap: true,
  });
  assert.equal(many.ok, true, many.error);
  assert.equal(many.result.placed.length, 2);
  assert.deepEqual(
    many.result.skipped.map((item) => item.reason),
    ['stands in another'],
  );
  const street = ask(5, 'fill_row', {
    from: { x: 0, y: 100, z: 0 },
    to: { x: 120, y: 100, z: 0 },
    facing: 'left',
    names: [house],
  });
  assert.equal(street.ok, true, street.error);
  assert.ok(street.result.placed.length >= 2);
  assert.deepEqual(street.result.skipped, []);
});

test('Details rows that do not fit scroll with the footer buttons, and a press hits the row shown', () => {
  const { Shell, detailRows } = require('../../../../Client/resources/architect/shell.cjs');
  const { LAYOUT } = require('../../../../Client/resources/architect/theme.cjs');
  const root = {
    createClip() {
      return {
        set() {},
        draw() {
          return true;
        },
        on() {},
        get() {
          return 0;
        },
        createClip: () => root.createClip(),
      };
    },
  };
  const pressed = [];
  const handlers = new Proxy(
    { detail: (row, side) => pressed.push([row, side]) },
    { get: (target, name) => target[name] ?? (() => {}) },
  );
  const shell = new Shell(root, handlers, {}, () => {});
  shell.build();
  const rows = Array.from({ length: 40 }, (_, index) => ({ label: `Row ${index}`, value: '' }));
  const viewOf = (details) => ({
    camera: {},
    menu: {},
    toolbar: {},
    outliner: null,
    details,
    dock: null,
    status: null,
  });
  shell.render = function render(view) {
    this.lastView = view;
    this.runs = [];
    this.pictures = [];
    this.renderDetails(view.details);
  };
  shell.render(viewOf({ icon: 'select', title: 'Many', kind: '', rows }));
  const fit = detailRows(rows.length);
  assert.ok(fit < rows.length);
  // The first row, then down a page with the footer's down button: the first row is now `fit`.
  shell.pressDetails(20, 106 + 5);
  shell.pressDetails(LAYOUT.details.width - 10, LAYOUT.details.height - 10);
  shell.pressDetails(20, 106 + 5);
  // Down again stops at the last page; up goes back a page.
  shell.pressDetails(LAYOUT.details.width - 10, LAYOUT.details.height - 10);
  shell.pressDetails(LAYOUT.details.width - 10, LAYOUT.details.height - 10);
  shell.pressDetails(20, 106 + 5);
  shell.pressDetails(LAYOUT.details.width - 60, LAYOUT.details.height - 10);
  shell.pressDetails(20, 106 + 5);
  assert.deepEqual(pressed, [
    [0, 0],
    [fit, 0],
    [rows.length - fit, 0],
    [rows.length - 2 * fit, 0],
  ]);
  // Another selection starts at its top again.
  shell.render(viewOf({ icon: 'select', title: 'Other', kind: '', rows }));
  shell.pressDetails(20, 106 + 5);
  assert.deepEqual(pressed.at(-1), [0, 0]);
  // Rows that fit show no footer: a press low in the panel is not a page turn.
  shell.render(viewOf({ icon: 'select', title: 'Few', kind: '', rows: rows.slice(0, 3) }));
  assert.equal(detailRows(3) >= 3, true);
});
