'use strict';

// The Asset/Animation Studio's editor (Client/resources/studio/editor.cjs): its own
// screen description, the rope it asks the game client for, undo and redo, and the kept
// project, driven with stand-ins for the game's pieces.
const test = require('node:test');
const assert = require('node:assert/strict');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');

// How a new rope or whip moves (editor.cjs PHYSICS_FIELDS' first values).
// A whip's hand, grip and physics in an export (Player.setPhysicalProp).
const WHIP_SETTINGS = Object.freeze({
  hand: 'RightHand',
  grip: { pitch: 90, twist: 0 },
  physics: { gravity: 9.8, damping: 0.3, mass: 0.5, air: 0.1, bend: 0, stiffness: 0, collide: 2 },
});
// A prop key made from the studio's first whip.
const PROP_KEY = Object.freeze({
  gravity: 9.8,
  damping: 0.3,
  mass: 0.5,
  air: 0.1,
  bend: 0,
  stiffness: 0,
  collide: 2,
  shape: 0,
  curl: 0,
  sway: 0,
  hand: 'RightHand',
  pitch: 90,
  twist: 0,
});
const PHYSICS = Object.freeze({
  gravity: 9.8,
  damping: 0.3,
  mass: 0.5,
  air: 0.1,
  bend: 0,
  wind: 0,
  stiffness: 0,
  friction: 0.5,
  thickness: 0.02,
  collide: 1,
  shape: 0,
  curl: 0,
  sway: 0,
});
// A new whip's: it ignores characters (its lash swings past the bodies: the floor only).
const WHIP_PHYSICS = Object.freeze({ ...PHYSICS, collide: 2 });
// Where a new prop is held (placing.cjs holdOf): the right hand.
const HOLD = Object.freeze({ joint: 'RightHand', at: [32.6, 31.4, 33.3], turn: [0, 0, 0] });
// A rope's end at first (placing.cjs endOf): free, its pin place beyond the free place.
const END = Object.freeze({ pinned: false, at: [33.4, 31.4, 33.3] });
// The messages that tell where things are and what the gizmo is on (not the props' own).
const PLACING = Object.freeze(['studioPropHold', 'studioPropGizmo', 'studioRopeEnd']);

function setup() {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
  });
  // The rope's own messages (its end's, studioRopeEnd, are where it is held, as studioPropHold).
  const ropes = () =>
    sent.filter((message) => message.op.startsWith('studioRope') && message.op !== 'studioRopeEnd');
  const saves = () => sent.filter((message) => message.op === 'architectSave');
  return { editor, sent, ropes, saves };
}

test('the studio names itself and shows its own modes and content', () => {
  const { editor } = setup();
  const view = editor.view();
  assert.equal(view.menu.title, 'Asset/Animation Studio');
  assert.deepEqual(
    view.toolbar.modes.map((mode) => mode.label),
    ['Props  1', 'Physics  2', 'Animation  3', 'Textures  4', 'Bones  5'],
  );
  assert.deepEqual(
    view.toolbar.modes.map((mode) => mode.later),
    [false, false, false, false, false],
  );
  assert.deepEqual(
    view.dock.tiles.map((tile) => tile.name),
    ['Rope', 'Whip', 'Chain'],
  );
  assert.deepEqual(
    editor.outlinerLines().map((line) => line.name),
    ['Scene', 'Character'],
  );
});

test('Bones mode shows the skeleton, lists the joints by pages and chooses one', () => {
  const { editor, sent } = setup();
  const bones = () => sent.filter((message) => message.op === 'studioBones');
  editor.useMode('bones');
  // The world's view on the 1920 x 1080 stage (architect/theme.cjs LAYOUT.view), -1 to 1.
  const view = [-0.6604, -0.4222, 0.6, 0.8444];
  assert.deepEqual(bones(), [{ op: 'studioBones', on: true, joint: -1, view, ring: -1 }]);
  assert.equal(editor.details().title, 'Skeleton');
  // Content shows the timeline instead of tiles: the playhead at 0 on the first key.
  const dock = editor.view().dock;
  assert.deepEqual(dock.tiles, []);
  assert.equal(dock.custom.time, 0);
  assert.equal(dock.custom.key, 0);
  assert.deepEqual(dock.custom.keys, [0]);

  // 30 joints: a root, then a chain.
  const names = Array.from({ length: 30 }, (_, id) => (id === 0 ? 'Bip01' : `joint_${id}`));
  const parents = names.map((_, id) => id - 1);
  editor.jointsKept(names, parents);
  const first = editor.view().outliner;
  assert.equal(first.footer, '30 joints');
  assert.equal(first.lines[0].name, 'Bip01');
  assert.equal(first.lines[0].type, 'Root');
  assert.equal(first.lines[1].type, 'Bip01');
  const rows = first.lines.length;
  assert.ok(rows > 0 && rows < 30);
  assert.equal(first.pager[1], `1 / ${Math.ceil(30 / rows)}`);

  editor.turnOutlinerPage(1);
  assert.equal(editor.outlinerLines()[0].name, names[rows]);
  editor.pressOutliner(0);
  assert.equal(editor.joint, rows);
  assert.deepEqual(bones().at(-1), { op: 'studioBones', on: true, joint: rows, view, ring: -1 });
  assert.equal(editor.view().outliner.chosen, 0);
  assert.equal(editor.details().title, names[rows]);

  // The parent row chooses the parent.
  const parentRow = editor.details().rows.findIndex((row) => row.label === 'Parent');
  editor.pressDetail(parentRow, 0);
  assert.equal(editor.joint, rows - 1);

  editor.useMode('props');
  assert.deepEqual(bones().at(-1), { op: 'studioBones', on: false, joint: -1, view, ring: -1 });
  assert.deepEqual(
    editor.outlinerLines().map((line) => line.name),
    ['Scene', 'Character'],
  );
});

test('a press in the view asks the game client for the joint; its answer chooses it', () => {
  const { editor, sent } = setup();
  editor.api.NoClip = {
    getPosition: () => ({ x: 1, y: 2, z: 3 }),
    screenLine: (x, y) => ({ x, y, z: 1 }),
  };
  editor.jointsKept(['root', 'hips', 'spine'], [-1, 0, 1]);
  editor.pressView(931, 426);
  assert.equal(
    sent.filter((message) => message.op === 'studioPickJoint').length,
    0,
    'only Bones mode picks joints',
  );
  editor.useMode('bones');
  editor.pressView(960, 540);
  assert.deepEqual(sent.at(-1), {
    op: 'studioPickJoint',
    from: [1, 2, 3],
    direction: [0, 0, 1],
  });
  editor.pressView(100, 540);
  assert.equal(sent.at(-1).op, 'studioPickJoint', 'a press on the outliner is not the view');
  assert.deepEqual(sent.at(-1).direction, [0, 0, 1]);
  editor.jointPicked(2);
  assert.equal(editor.joint, 2);
  editor.jointPicked(-1);
  assert.equal(editor.joint, 2);
  assert.match(editor.message, /No joint there/);
});

test('dragging a ring of the turning gizmo turns the chosen joint about its axis, one undo', () => {
  const { editor, sent } = setup();
  let pointer = { x: 900, y: 400 };
  editor.shell = { render: () => {}, viewPointer: () => pointer };
  editor.jointsKept(['root', 'hips', 'spine'], [-1, 0, 1]);
  editor.useMode('bones');
  editor.chooseJoint(2);
  // The game client found the press on the Y ring of the chosen joint.
  editor.jointPicked(2, 1);
  assert.equal(editor.ringDrag.axis, 1);
  assert.equal(sent.filter((message) => message.op === 'studioBones').at(-1).ring, 1);
  pointer = { x: 960, y: 420 };
  editor.followRing();
  assert.deepEqual(editor.animation.turnOf(2), [0, 30, 0], '60 pixels across: 30 degrees');
  editor.releaseRing(980);
  assert.equal(editor.ringDrag, null);
  assert.deepEqual(editor.animation.turnOf(2), [0, 40, 0]);
  assert.equal(sent.filter((message) => message.op === 'studioBones').at(-1).ring, -1);
  editor.undo();
  assert.deepEqual(editor.animation.turnOf(2), [0, 0, 0], 'the whole drag is one undo step');
  // A ring of another joint than the chosen one is not taken: the joint is chosen instead.
  editor.jointPicked(1, 0);
  assert.equal(editor.joint, 1);
  assert.equal(editor.ringDrag, null);
});

test('turning a joint poses it in the chosen key; keys play, undo and come back', () => {
  let now = 0;
  const { editor, sent, saves } = setup();
  editor.api.now = () => now;
  const poses = () => sent.filter((message) => message.op === 'studioPose');
  editor.jointsKept(['root', 'hips', 'spine'], [-1, 0, 1]);
  editor.useMode('bones');
  editor.chooseJoint(2);
  const turnX = editor.details().rows.findIndex((row) => row.label === 'Turn X');
  editor.pressDetail(turnX, 1);
  editor.pressDetail(turnX, 1);
  assert.deepEqual(editor.animation.turnOf(2), [10, 0, 0]);
  const [joint, x, y, z, w] = poses().at(-1).joints[0];
  assert.equal(joint, 2);
  assert.ok(Math.abs(x - Math.sin(Math.PI / 36)) < 1e-6 && y === 0 && z === 0);
  assert.ok(Math.abs(w - Math.cos(Math.PI / 36)) < 1e-6);

  // The playhead to 0.5 s; a turn there adds a key (auto key) with the pose there, then turns.
  editor.setTime(0.5);
  assert.equal(editor.keyHere(), -1);
  for (let step = 0; step < 4; step += 1) {
    editor.turnJoint(0, -1);
  }
  assert.deepEqual(
    editor.animation.keys.map((one) => [one.time, one.pose]),
    [
      [0, { 2: [10, 0, 0] }],
      [0.5, { 2: [-10, 0, 0] }],
    ],
  );
  const view = editor.view().dock.custom;
  assert.deepEqual(view.keys, [0, 0.5]);
  assert.equal(view.key, 1);
  assert.deepEqual(view.tracks, [{ label: 'spine', joint: 2, chosen: true, keys: [0, 1] }]);

  // Linear blend: half way through, half way between the turns (0 degrees).
  editor.animation.blend = 'linear';
  editor.animation.loop = false;
  editor.setTime(0);
  editor.setPlaying(true);
  now = 250;
  editor.playFrame();
  assert.equal(editor.time, 0.25);
  const middle = poses().at(-1).joints[0];
  assert.ok(Math.abs(middle[1]) < 1e-6 && Math.abs(middle[4] - 1) < 1e-6, 'half way is 0');
  editor.setPlaying(false);

  // A key dragged on the timeline: pressed on its diamond, let go at 1 s.
  editor.setTime(0.5);
  const keyX = 432 + 0.5 * ((1896 - 432) / editor.timelineView().span);
  editor.pressTimeline(keyX, 70 + 12);
  assert.deepEqual({ key: editor.drag.key, time: editor.drag.time }, { key: 1, time: 0.5 });
  const oneSecondX = 432 + 1 * ((1896 - 432) / editor.timelineView().span);
  editor.releaseTimeline(oneSecondX, 70 + 12);
  assert.equal(editor.drag, null);
  assert.deepEqual(
    editor.animation.keys.map((one) => one.time),
    [0, 1],
  );
  assert.equal(editor.time, 1);

  // Undo puts the key back at 0.5 s; the project keeps the keys.
  editor.undo();
  assert.deepEqual(
    editor.animation.keys.map((one) => one.time),
    [0, 0.5],
  );
  const kept = JSON.parse(saves().at(-1).data);
  assert.deepEqual(kept.animation.keys, [
    { time: 0, pose: { 2: [10, 0, 0] } },
    { time: 0.5, pose: { 2: [-10, 0, 0] } },
  ]);
  const again = setup();
  again.editor.shell = { render: () => {} };
  again.editor.projectKept(saves().at(-1).data);
  assert.equal(again.editor.animation.keys.length, 2);
  assert.deepEqual(again.editor.animation.turnOf(2), [10, 0, 0]);
});

test('the rope tile makes a rope in the hand; the chain tile a rope with chain links', () => {
  const { editor, ropes, saves } = setup();
  editor.pressTile(2);
  assert.deepEqual(ropes(), [
    { op: 'studioRope', segments: 24, length: 1.5, look: 'chain' },
    { op: 'studioRopeSweep', on: false },
  ]);
  assert.equal(editor.details().title, 'Chain');
  assert.equal(editor.rope.end.pinned, false, 'a chain’s end can be pinned like a rope’s');
  editor.removeRope();
  editor.pressTile(0);
  assert.deepEqual(ropes().slice(-2), [
    { op: 'studioRope', segments: 16, length: 1.2 },
    { op: 'studioRopeSweep', on: false },
  ]);
  assert.equal(editor.selected, 'rope');
  assert.equal(editor.details().title, 'Rope');
  assert.deepEqual(JSON.parse(saves().at(-1).data), {
    version: 1,
    rope: {
      kind: 'rope',
      segments: 16,
      length: 1.2,
      sweep: false,
      physics: PHYSICS,
      hold: HOLD,
      end: END,
    },
    animation: { keys: [{ time: 0, pose: {} }], blend: 'spline', loop: true },
    clothing: { outfit: 'henry', off: [] },
    sky: { hour: 12, preset: 'cloudless_sunny', rain: 0 },
  });
  assert.equal(saves().at(-1).map, 'kcds_studio');
});

test('details change the rope; undo and redo bring each step back', () => {
  const { editor, ropes } = setup();
  editor.pressTile(0);
  editor.changeRope('length', 1);
  editor.setSweep(true);
  const rope = (length, sweep) => ({
    kind: 'rope',
    segments: 16,
    length,
    sweep,
    physics: PHYSICS,
    hold: HOLD,
    end: END,
  });
  assert.deepEqual(editor.rope, rope(1.3, true));
  editor.undo();
  assert.deepEqual(editor.rope, rope(1.3, false));
  editor.undo();
  assert.deepEqual(editor.rope, rope(1.2, false));
  editor.undo();
  assert.equal(editor.rope, null);
  assert.deepEqual(ropes().at(-1), { op: 'studioRopeOff' });
  editor.redo();
  assert.deepEqual(editor.rope, rope(1.2, false));
  editor.removeRope();
  assert.equal(editor.rope, null);
  assert.equal(editor.selected, 'character');
});

test('the physics details change how the prop moves without making it again; undo too', () => {
  const { editor, sent } = setup();
  editor.pressTile(1);
  // Physics mode shows how the prop moves.
  editor.useMode('physics');
  const before = sent.length;
  const gravity = editor.details().rows.findIndex((row) => row.label === 'Gravity');
  const bend = editor.details().rows.findIndex((row) => row.label === 'Bend limit');
  assert.equal(editor.details().rows[bend].value, 'Free');
  editor.pressDetail(gravity, -1);
  editor.pressDetail(bend, 1);
  const made = sent
    .slice(before)
    .filter((message) => message.op !== 'architectSave' && !PLACING.includes(message.op));
  // No studioWhip again: only the settings (the whip's grip goes with them). A whip ignores
  // characters at first (collide 2: the floor only).
  assert.deepEqual(made, [
    { op: 'studioPropPhysics', ...WHIP_PHYSICS, gravity: 9.3 },
    { op: 'studioPropGrip', pitch: 90, twist: 0 },
    { op: 'studioPropPhysics', ...WHIP_PHYSICS, gravity: 9.3, bend: 5 },
    { op: 'studioPropGrip', pitch: 90, twist: 0 },
  ]);
  assert.equal(editor.details().rows[bend].value, '5 °');
  editor.undo();
  assert.deepEqual(sent.filter((message) => message.op === 'studioPropPhysics').at(-1), {
    op: 'studioPropPhysics',
    ...WHIP_PHYSICS,
    gravity: 9.3,
  });
  assert.equal(editor.rope.physics.bend, 0);
  // Ignore characters: on at first (Collide: Floor only); off, it collides with them (On).
  const rows = () => editor.details().rows;
  const ignore = () => rows().findIndex((row) => row.label === 'Ignore characters');
  const collide = () => rows().find((row) => row.label === 'Collide').value;
  assert.equal(rows()[ignore()].toggle, true);
  assert.equal(collide(), 'Floor only');
  editor.pressDetail(ignore(), 0);
  assert.equal(rows()[ignore()].toggle, false);
  assert.equal(collide(), 'On');
  assert.equal(sent.filter((message) => message.op === 'studioPropPhysics').at(-1).collide, 1);
  editor.undo();
  assert.equal(editor.rope.physics.collide, 2);
  // Collide stays no higher than Floor only.
  editor.pressDetail(
    rows().findIndex((row) => row.label === 'Collide'),
    1,
  );
  assert.equal(editor.rope.physics.collide, 2);

  // The grip (Props mode): Grip angle a step down turns the handle 15 degrees toward the arm.
  editor.useMode('props');
  const grip = editor.details().rows.findIndex((row) => row.label === 'Grip angle');
  editor.pressDetail(grip, -1);
  assert.deepEqual(sent.filter((message) => message.op === 'studioPropGrip').at(-1), {
    op: 'studioPropGrip',
    pitch: 75,
    twist: 0,
  });
});

test('the character wears Henry’s clothes; outfits and pieces change, the project keeps them', () => {
  const { editor, sent, saves } = setup();
  editor.shell = { render: () => {} };
  editor.rebuildProject();
  const outfits = () => sent.filter((message) => message.op === 'studioOutfit');
  assert.equal(outfits().at(-1).pieces.length, 4, 'Henry’s four pieces');
  editor.selected = 'character';
  const rows = () => editor.details().rows;
  assert.equal(rows().find((row) => row.label === 'Outfit').value, 'Henry');
  const hood = rows().findIndex((row) => row.label === 'Hood');
  assert.equal(rows()[hood].toggle, true);
  editor.pressDetail(hood, 0);
  assert.equal(outfits().at(-1).pieces.length, 3);
  assert.equal(rows()[hood].toggle, false);
  assert.deepEqual(JSON.parse(saves().at(-1).data).clothing, { outfit: 'henry', off: ['Hood'] });
  const outfit = rows().findIndex((row) => row.label === 'Outfit');
  editor.pressDetail(outfit, 1);
  assert.equal(rows()[outfit].value, 'Henry travelling');
  assert.equal(outfits().at(-1).pieces.length, 8);
  editor.pressDetail(outfit, 1);
  assert.deepEqual(outfits().at(-1), { op: 'studioOutfit', pieces: [] });

  const again = setup();
  again.editor.shell = { render: () => {} };
  again.editor.projectKept(
    JSON.stringify({ version: 1, clothing: { outfit: 'henry', off: ['Hood', 'x'] } }),
  );
  assert.deepEqual(again.editor.clothing, { outfit: 'henry', off: ['Hood'] });
});

test('the scene’s sky: time of day round the clock, weather, rain; the project keeps it', () => {
  const { editor, sent, saves } = setup();
  editor.shell = { render: () => {} };
  editor.rebuildProject();
  const skies = () => sent.filter((message) => message.op === 'studioSky');
  assert.deepEqual(skies().at(-1), {
    op: 'studioSky',
    hour: 12,
    preset: 'cloudless_sunny',
    rain: 0,
  });
  editor.pressOutliner(0);
  assert.equal(editor.selected, 'scene');
  const rows = () => editor.details().rows;
  const hour = rows().findIndex((row) => row.label === 'Time of day');
  for (let step = 0; step < 13; step += 1) {
    editor.pressDetail(hour, 1);
  }
  assert.equal(rows()[hour].value, '01:00', 'past midnight');
  const weather = rows().findIndex((row) => row.label === 'Weather');
  editor.pressDetail(weather, 1);
  const rain = rows().findIndex((row) => row.label === 'Rain');
  editor.pressDetail(rain, 1);
  editor.pressDetail(rain, 1);
  assert.deepEqual(skies().at(-1), {
    op: 'studioSky',
    hour: 1,
    preset: 'overcast_rain',
    rain: 0.2,
  });
  assert.equal(rows()[rain].value, '20 %');
  assert.deepEqual(JSON.parse(saves().at(-1).data).sky, {
    hour: 1,
    preset: 'overcast_rain',
    rain: 0.2,
  });
});

test('Static props lists your meshes; a tile shows one, Details removes it, the project keeps it', async () => {
  const { editor, sent, saves } = setup();
  editor.shell = { render: () => {} };
  const meshes = () => sent.filter((message) => message.op === 'studioMesh');
  editor.pressFolder(1);
  assert.equal(editor.folder, 'static');
  assert.ok(sent.some((message) => message.op === 'studioMeshList'));
  // Without meshes: only the simple props to make here.
  assert.deepEqual(
    editor.items().map((item) => item.name),
    ['shape:box', 'shape:cylinder', 'shape:sphere', 'shape:plane'],
  );
  editor.meshesKept(['suzanne', 'whip']);
  assert.deepEqual(
    editor.view().dock.tiles.map((tile) => tile.name),
    ['suzanne', 'whip', 'New box', 'New cylinder', 'New sphere', 'New plane'],
  );
  editor.pressTile(0);
  assert.equal(editor.selected, 'mesh');
  assert.equal(meshes().at(-1).name, 'suzanne');
  assert.equal(JSON.parse(saves().at(-1).data).mesh, 'suzanne');
  assert.ok(editor.outlinerLines().some((line) => line.thing === 'mesh'));
  // The mesh shown named again (made anew by studio_make_mesh): its file loads again where it
  // stands, with no undo step of its own.
  const shownBefore = meshes().length;
  const undoBefore = editor.undoStack.length;
  await editor.script('mesh suzanne');
  assert.equal(meshes().length, shownBefore + 1);
  assert.equal(meshes().at(-1).name, 'suzanne');
  assert.equal(editor.undoStack.length, undoBefore);
  const remove = editor.details().rows.findIndex((row) => row.label === 'Remove');
  editor.pressDetail(remove, 0);
  assert.equal(meshes().at(-1).name, '');
  assert.equal(JSON.parse(saves().at(-1).data).mesh, undefined);
});

test('Game props and Game clips search the game’s own files by pages; a tile shows or plays one', () => {
  const { editor, sent, saves } = setup();
  editor.shell = { render: () => {} };
  const asked = () => sent.filter((message) => message.op === 'studioGameFiles');
  const PATHS = ['Objects/props/tools/whip_a.cgf', 'Objects/props/tools/whip_b.cgf'];
  editor.pressFolder(2);
  assert.equal(editor.folder, 'gameProps');
  assert.deepEqual(asked().at(-1), {
    op: 'studioGameFiles',
    kind: 'objects',
    query: '',
    page: 0,
    size: 10,
  });
  // While the game client still reads the packs.
  editor.gameFilesKept({ kind: 'objects', query: '', ready: false });
  assert.deepEqual(
    editor.items().map((item) => item.name),
    ['gameReading'],
  );
  // The search typed key by key asks again each time; an answer to an older search is left.
  editor.pressContentSearch();
  editor.type('w', false);
  editor.type('h', false);
  assert.equal(asked().at(-1).query, 'wh');
  editor.gameFilesKept({
    kind: 'objects',
    query: 'w',
    ready: true,
    page: 0,
    pages: 9,
    total: 90,
    paths: ['x'],
  });
  assert.deepEqual(
    editor.items().map((item) => item.name),
    ['gameReading'],
  );
  editor.gameFilesKept({
    kind: 'objects',
    query: 'wh',
    ready: true,
    page: 0,
    pages: 3,
    total: 22,
    paths: PATHS,
  });
  const view = editor.view();
  assert.deepEqual(
    view.dock.tiles.map((tile) => tile.name),
    ['whip_a', 'whip_b'],
  );
  assert.equal(view.dock.tiles[0].meta, 'props/tools');
  assert.deepEqual(view.dock.pager, ['<<', '1 / 3', '>>']);
  assert.equal(view.dock.search, 'wh|');
  editor.type('enter', false);
  assert.equal(editor.typing, false);
  // The pager asks the game client for the next page.
  editor.turnPage(1);
  assert.deepEqual(asked().at(-1), {
    op: 'studioGameFiles',
    kind: 'objects',
    query: 'wh',
    page: 1,
    size: 10,
  });
  // A tile shows the game's object by its path; the project keeps it.
  editor.pressTile(1);
  const shown = sent.filter((message) => message.op === 'studioMesh').at(-1);
  assert.equal(shown.path, PATHS[1]);
  assert.equal(shown.name, undefined);
  assert.equal(JSON.parse(saves().at(-1).data).mesh, PATHS[1]);
  assert.equal(editor.details().title, 'whip_b');
  // Game clips: a tile plays the clip on the character, pressed again it stops.
  editor.useMode('animation');
  editor.pressFolder(2);
  assert.equal(editor.folder, 'gameClips');
  assert.equal(asked().at(-1).kind, 'clips');
  const CLIP = 'Animations/humans/male/whipping.caf';
  editor.gameFilesKept({
    kind: 'clips',
    query: '',
    ready: true,
    page: 0,
    pages: 1,
    total: 1,
    paths: [CLIP],
  });
  editor.pressTile(0);
  const clips = () => sent.filter((message) => message.op === 'studioGameClip');
  assert.deepEqual(clips().at(-1), {
    op: 'studioGameClip',
    path: CLIP,
    loop: true,
    from: 0,
    to: 1,
    inPlace: true,
    topBody: false,
  });
  assert.equal(editor.view().dock.tile, 0);
  // Its Details: where it is (the game client's state) and the part that plays from here.
  editor.gameClipStateKept({
    path: CLIP,
    time: 0.5,
    seconds: 40,
    from: 0,
    to: 1,
    loop: true,
    inPlace: true,
  });
  const rows = () => editor.details().rows;
  assert.equal(rows().find((row) => row.label === 'Time').value, '20.00 s of 40.0 s');
  editor.pressDetail(
    rows().findIndex((row) => row.value === 'From here'),
    0,
  );
  assert.equal(clips().at(-1).from, 0.5);
  editor.pressDetail(
    rows().findIndex((row) => row.label === 'To'),
    1,
  );
  assert.equal(clips().at(-1).to, 1);
  editor.pressDetail(
    rows().findIndex((row) => row.label === 'To'),
    -1,
  );
  assert.equal(clips().at(-1).to, 0.9938);
  editor.pressDetail(
    rows().findIndex((row) => row.label === 'In place'),
    0,
  );
  assert.equal(clips().at(-1).inPlace, false);
  editor.pressTile(0);
  assert.equal(clips().at(-1).path, '');
});

test('a game clip’s part exports: every player plays it again and again with the prop', () => {
  const { editor, sent } = setup();
  editor.shell = { render: () => {} };
  editor.pressTile(1);
  const CLIP = 'Animations/humans/male/cinematics/x/executioner.caf';
  editor.playGameClip(CLIP, true, { from: 0.615, to: 0.88 });
  // Before the game client measured its length there is no part in seconds to export.
  assert.equal(editor.exportedGameClip(), null);
  editor.gameClipStateKept({
    path: CLIP,
    time: 0.7,
    seconds: 31.6,
    from: 0.615,
    to: 0.88,
    loop: true,
    inPlace: true,
  });
  assert.deepEqual(editor.exportedGameClip(), {
    clip: CLIP,
    from: 19.43,
    to: 27.81,
    topBody: false,
  });
  editor.exportResource();
  const files = sent.filter((message) => message.op === 'studioExport').at(-1).files;
  const server = files.find(([path]) => path === 'server.js')[1];
  assert.match(
    server,
    /const GAME_CLIP = \{"clip":"Animations\/humans\/male\/cinematics\/x\/executioner\.caf","from":19\.43,"to":27\.81,"topBody":false\};/,
  );
  assert.match(
    server,
    /player\.playAnimation\('', \{\n {4}clip: GAME_CLIP\.clip,\n {4}time: GAME_CLIP\.from,\n {4}lockMovement: true,\n {4}lockRotation: true,\n {2}\}\);/,
  );
  assert.match(
    server,
    /setInterval\(\(\) => playRound\(player\), \(GAME_CLIP\.to - GAME_CLIP\.from\) \* 1000\)/,
  );
  assert.match(server, /player\.setPhysicalProp\(PROP\);/);
  assert.match(
    server,
    /clearInterval\(rounds\.get\(player\.id\)\);\n {2}rounds\.delete\(player\.id\);/,
  );
  // The player sees themselves from behind while it plays (client.js, in the manifest).
  const manifest = JSON.parse(files.find(([path]) => path === 'package.json')[1]);
  assert.deepEqual(manifest.kcds.clientScripts, ['client.js']);
  const client = files.find(([path]) => path === 'client.js')[1];
  const calls = [];
  const binds = {};
  new Function('kcds', client)({
    ui: {
      Camera: { setThirdPerson: (view) => calls.push(view) },
      Key: { bind: (key, handler) => (binds[key] = handler) },
    },
  });
  assert.deepEqual(calls, [{ distance: 3.2, height: 1.5, shoulder: 0.5 }]);
  binds.v();
  assert.equal(calls.at(-1), null);
  binds.v();
  assert.deepEqual(calls.at(-1), { distance: 3.2, height: 1.5, shoulder: 0.5 });
});

test('top body only: a game clip plays on the upper body on players, who walk meanwhile', () => {
  const { editor, sent } = setup();
  editor.shell = { render: () => {} };
  const CLIP = 'Animations/humans/male/cinematics/x/executioner.caf';
  editor.playGameClip(CLIP, true, { from: 0.615, to: 0.88 });
  editor.useMode('animation');
  const rows = () => editor.details().rows;
  const top = () => rows().findIndex((row) => row.label === 'Top body only');
  assert.equal(rows()[top()].toggle, false);
  editor.pressDetail(top(), 0);
  assert.equal(sent.filter((message) => message.op === 'studioGameClip').at(-1).topBody, true);
  editor.gameClipStateKept({
    path: CLIP,
    time: 0.7,
    seconds: 31.6,
    from: 0.615,
    to: 0.88,
    loop: true,
    inPlace: true,
  });
  editor.exportResource();
  const files = sent.filter((message) => message.op === 'studioExport').at(-1).files;
  const server = files.find(([path]) => path === 'server.js')[1];
  assert.match(server, /upperBody: true/);
  assert.doesNotMatch(server, /lockMovement/);
});

test('a game clip baked into keys: the studio animation plays, exported held in place', () => {
  const { editor, sent } = setup();
  editor.shell = { render: () => {} };
  const CLIP = 'Animations/humans/male/cinematics/x/executioner.caf';
  editor.jointsKept(['Human_Male', 'Hips', 'Spine', 'RightArm'], [-1, 0, 1, 2]);
  editor.playGameClip(CLIP, true, { from: 0.615, to: 0.88 });
  editor.useMode('animation');
  const rows = () => editor.details().rows;
  editor.gameClipStateKept({
    path: CLIP,
    time: 0.7,
    seconds: 31.6,
    from: 0.615,
    to: 0.88,
    loop: true,
    inPlace: true,
  });
  editor.pressDetail(
    rows().findIndex((row) => row.value === 'Bake to keys'),
    0,
  );
  assert.equal(sent.at(-1).op, 'studioGameClipBake');
  // The game client's recording, in two parts: the arm swings round every 2 s.
  const { names, parents, idle, idleHips, frames } = recordedSwing(2);
  const half = (frames.length / (4 + 4 * names.length) / 2) * (4 + 4 * names.length);
  const common = { op: 'studioGameClipBaked', path: CLIP, parts: 2, seconds: 3 };
  editor.gameClipBakedKept({ ...common, part: 1, frames: frames.slice(half) });
  editor.gameClipBakedKept({
    ...common,
    part: 0,
    names,
    parents,
    idle,
    idleHips,
    frames: frames.slice(0, half),
  });
  // The game clip stopped; the animation is the baked loop, playing.
  assert.equal(sent.filter((message) => message.op === 'studioGameClip').at(-1).path, '');
  assert.equal(editor.animation.playing, true);
  const keys = editor.animation.toJSON().keys;
  assert.ok(keys.length >= 2 && keys.length <= 32);
  assert.ok(Math.abs(editor.animation.length() - 2) < 0.15, `loop ${editor.animation.length()}`);
  assert.ok(keys.every((key) => key.lift !== undefined && key.lift < 0));
  assert.match(editor.message, /^Baked: /);
  // Exported, the whole body plays it: the player is held and sees themselves from behind.
  editor.exportResource();
  const files = sent.filter((message) => message.op === 'studioExport').at(-1).files;
  const server = files.find(([path]) => path === 'server.js')[1];
  assert.match(
    server,
    /player\.playAnimation\('kcds_studio_animation', \{ lockMovement: true \}\);/,
  );
  assert.match(server, /"lift":-0\.05/);
  assert.ok(files.some(([path]) => path === 'client.js'));
});

// A recording as the game client makes one: the right arm swings round about x every `period`
// seconds over 4 s at 30 frames a second, the hips a little lower than before the clip.
function recordedSwing(period) {
  const names = ['Human_Male', 'Hips', 'Spine', 'RightArm'];
  const parents = [-1, 0, 1, 2];
  const idle = names.flatMap(() => [0, 0, 0, 1]);
  const frames = [];
  for (let frame = 0; frame <= 120; frame += 1) {
    const time = frame / 30;
    const half = (Math.sin((2 * Math.PI * time) / period) * 60 * Math.PI) / 360;
    // The time, the hips' place, then the root, hips and spine still, the arm swinging.
    const still = [0, 0, 0, 1];
    const arm = [Math.sin(half), 0, 0, Math.cos(half)];
    frames.push(time, 0, 0, 0.9, ...still, ...still, ...still, ...arm);
  }
  return { names, parents, idle, idleHips: [0, 0, 0.95], frames };
}

test('top body only: an animation’s hips and legs keys stay out of the export', () => {
  const { editor, sent } = setup();
  editor.shell = { render: () => {} };
  editor.jointsKept(['Human_Male', 'Hips', 'Spine', 'RightArm', 'LeftUpLeg'], [-1, 0, 1, 2, 1]);
  editor.animation.restore({
    keys: [
      { time: 0, pose: { 1: [10, 0, 0], 3: [20, 0, 0], 4: [30, 0, 0] } },
      { time: 1, pose: { 3: [40, 0, 0] } },
    ],
  });
  assert.deepEqual(editor.upperBodyJointNames(), ['Spine', 'RightArm']);
  editor.useMode('bones');
  const rows = () => editor.details().rows;
  const top = rows().findIndex((row) => row.label === 'Top body only');
  editor.pressDetail(top, 0);
  assert.equal(editor.animation.topBody, true);
  assert.equal(editor.animation.toJSON().topBody, true);
  const exported = editor.exportedAnimation();
  assert.deepEqual(exported.keys[0].pose, { RightArm: [20, 0, 0] });
  assert.deepEqual(exported.keys[1].pose, { RightArm: [40, 0, 0] });
  // The project keeps it.
  const saved = JSON.parse(sent.filter((one) => one.op === 'architectSave').at(-1).data);
  assert.equal(saved.animation.topBody, true);
});

test('the Timeline tab shows the animation’s timeline in every mode; Space plays there', () => {
  const { editor } = setup();
  editor.shell = { render: () => {} };
  assert.equal(editor.view().dock.custom, null);
  const timelineTab = editor.view().dock.tabs.indexOf('Timeline');
  assert.ok(timelineTab > 0);
  editor.pressTab(timelineTab);
  for (const mode of ['props', 'physics', 'animation', 'textures']) {
    editor.useMode(mode);
    const custom = editor.view().dock.custom;
    assert.ok(custom, `a timeline in ${mode} mode`);
    assert.equal(custom.playing, false);
  }
  // Its Play button: the timeline's own press (timeline.cjs hit) plays the animation.
  const pressed = [];
  editor.setPlaying = (on) => pressed.push(on);
  editor.pressTab(0);
  assert.equal(editor.view().dock.custom, null);
  editor.pressTab(timelineTab);
  editor.animation.playing = false;
  editor.keysDown.clear();
  editor.api.Key.isDown = (key) => key === 'space';
  editor.flying = false;
  editor.typing = false;
  editor.poll();
  assert.deepEqual(pressed, [true]);
});

// A stand-in for the NativeUI root: clips that make clips (the shell's panels sit in its stage
// clip), each kept in `clips`.
function standInRoot(clips, mouse = { x: 0, y: 0 }) {
  const make = () => {
    const clip = {
      at: { x: 0, y: 0 },
      mouse: { ...mouse },
      handlers: {},
      set(values) {
        Object.assign(this.at, values);
      },
      on(event, handler) {
        this.handlers[event] = handler;
      },
      get(name) {
        return name === '_xmouse' ? this.mouse.x : this.mouse.y;
      },
      draw() {},
      createClip: make,
    };
    clips.push(clip);
    return clip;
  };
  return { createClip: make };
}

test('a press the world view hears over a panel goes to that panel; one heard twice is one', () => {
  const { Shell } = require('../../../../Client/resources/architect/shell.cjs');
  const clips = [];
  const root = standInRoot(clips);
  const heard = [];
  const handlers = {
    view: (x, y) => heard.push(['view', x, y]),
    camSpeed: () => heard.push(['camSpeed']),
    detail: (row, side) => heard.push(['detail', row, side]),
    viewRelease: (x) => heard.push(['viewRelease', x]),
  };
  const shell = new Shell(root, handlers, {}, () => {});
  shell.build();
  const view = clips.find((clip) => clip.at.x === 326 && clip.at.y === 84);
  // The press is over the Details panel (stage 1880, 223), heard by the world view's clip.
  view.mouse = { x: 1880 - 326, y: 223 - 84 };
  let now = 1000;
  const realNow = Date.now;
  Date.now = () => now;
  try {
    view.handlers.press();
    // The same click heard by the Details clip too: one press.
    const details = clips.find((clip) => clip.at.x === 1536 && clip.at.y === 84);
    details.mouse = { x: 1880 - 1536, y: 223 - 84 };
    details.handlers.press?.();
    now += 500;
    view.mouse = { x: 400, y: 300 };
    view.handlers.press();
    view.handlers.release();
  } finally {
    Date.now = realNow;
  }
  assert.equal(heard.filter((one) => one[0] === 'view').length, 1, 'only the press in the view');
  assert.deepEqual(
    heard.find((one) => one[0] === 'view'),
    ['view', 726, 384],
  );
  assert.deepEqual(heard.at(-1), ['viewRelease', 726]);
  assert.ok(
    heard.every((one) => one[0] !== 'view' || one[1] < 1536),
    'the press over Details did not go to the view',
  );
});

test('a Details list longer than the panel scrolls with the wheel over it; presses follow', () => {
  const { Shell, detailRows } = require('../../../../Client/resources/architect/shell.cjs');
  const clips = [];
  const root = standInRoot(clips, { x: -1, y: -1 });
  const heard = [];
  const shell = new Shell(root, { detail: (row, side) => heard.push([row, side]) }, {}, () => {});
  shell.build();
  // Only the Details panel of the screen (the whole of it needs an editor's view).
  shell.render = function render(view) {
    this.lastView = view;
    this.runs = [];
    this.renderDetails(view.details);
  };
  const rows = Array.from({ length: 25 }, (_, index) => ({
    label: `Row ${index}`,
    value: '1',
    step: () => {},
  }));
  shell.render({ details: { icon: 'magnet', title: 'Chain', kind: 'Physical prop', rows } });
  const details = clips.find((clip) => clip.at.x === 1536 && clip.at.y === 84);
  // Not over the panel: nothing scrolls.
  assert.equal(shell.wheel(-1), false);
  details.mouse = { x: 100, y: 200 };
  assert.equal(shell.wheel(-1), true);
  assert.equal(shell.detailFrom, 2);
  // Never past the end: the last rows fill the panel above its footer.
  const last = rows.length - detailRows(rows.length);
  shell.wheel(-10);
  assert.equal(shell.detailFrom, last);
  shell.pressDetails(300, 106 + 32 * 0 + 5);
  assert.deepEqual(heard.at(-1), [last, 0]);
  shell.wheel(3);
  assert.equal(shell.detailFrom, last - 6);
  // Something else shown: back at the top.
  shell.render({ details: { icon: 'magnet', title: 'Whip', kind: 'Physical prop', rows } });
  assert.equal(shell.detailFrom, 0);
});

test('the number keys choose the modes, as in World Architect', () => {
  const { editor } = setup();
  const down = new Set();
  editor.api.Key = { isDown: (key) => down.has(key) };
  editor.api.NoClip = { setInputEnabled: () => {} };
  editor.shell = { render: () => {} };
  down.add('5');
  editor.poll();
  assert.equal(editor.mode, 'bones');
  down.delete('5');
  editor.poll();
  down.add('2');
  editor.poll();
  assert.equal(editor.mode, 'physics');
  down.delete('2');
  editor.poll();
  down.add('3');
  editor.poll();
  assert.equal(editor.mode, 'animation', 'Animation mode: the animation’s events');
});

test('Ctrl+Z and Ctrl+Y follow the letters the keyboard layout prints (QWERTZ)', () => {
  const { editor } = setup();
  const down = new Set();
  // A Czech or German layout: the key a US keyboard has Y on prints Z, and the other way round.
  editor.api.Key = {
    isDown: (key) => down.has(key),
    getLabel: (key) => ({ y: 'Z', z: 'Y' }[key] ?? key.toUpperCase()),
  };
  editor.api.NoClip = { setInputEnabled: () => {} };
  editor.shell = { render: () => {} };
  const done = [];
  editor.undo = () => done.push('undo');
  editor.redo = () => done.push('redo');
  down.add('ctrl');
  down.add('y');
  editor.poll();
  down.delete('y');
  editor.poll();
  down.add('z');
  editor.poll();
  assert.deepEqual(done, ['undo', 'redo']);
});

test('the rope stays within its limits', () => {
  const { editor } = setup();
  editor.pressTile(0);
  for (let step = 0; step < 40; step += 1) {
    editor.changeRope('segments', 1);
  }
  assert.equal(editor.rope.segments, 64);
  for (let step = 0; step < 300; step += 1) {
    editor.changeRope('length', -1);
  }
  assert.equal(editor.rope.length, 0.2);
});

test('the kept project comes back when the editor stands; a bad one is left out', () => {
  const { editor, ropes } = setup();
  editor.projectKept(
    JSON.stringify({
      version: 1,
      rope: { segments: 99, length: 2, sweep: true, physics: { gravity: 50, bend: 20 } },
    }),
  );
  editor.rebuildProject();
  assert.deepEqual(editor.rope, {
    kind: 'rope',
    segments: 64,
    length: 2,
    sweep: true,
    physics: { ...PHYSICS, gravity: 30, bend: 20 },
    hold: HOLD,
    end: END,
  });
  assert.deepEqual(ropes().slice(-2), [
    { op: 'studioRope', segments: 64, length: 2 },
    { op: 'studioRopeSweep', on: true },
  ]);
  assert.equal(editor.undoStack.length, 0);

  const other = setup();
  other.editor.projectKept('not json');
  other.editor.rebuildProject();
  assert.equal(other.editor.rope, null);
  other.editor.projectKept(JSON.stringify({ version: 7, rope: { segments: 4, length: 1 } }));
  other.editor.rebuildProject();
  assert.equal(other.editor.rope, null);
});

test('the whip tile makes a whip; a rope made after it takes its place', () => {
  const { editor, sent } = setup();
  // The prop's own messages: not the project's saves nor its physics settings.
  const own = () =>
    sent.filter(
      (message) =>
        !['architectSave', 'studioPropPhysics', 'studioPropGrip', ...PLACING].includes(message.op),
    );
  editor.pressTile(1);
  assert.deepEqual(own().slice(-2), [
    { op: 'studioWhip', segments: 20, length: 1.6 },
    { op: 'studioWhipSweep', on: false },
  ]);
  assert.deepEqual(editor.rope, {
    kind: 'whip',
    segments: 20,
    length: 1.6,
    sweep: false,
    physics: WHIP_PHYSICS,
    hold: HOLD,
    grip: { pitch: 90, twist: 0 },
  });
  assert.equal(editor.details().title, 'Whip');
  assert.equal(editor.outlinerLines().at(-1).name, 'Whip');
  editor.pressTile(0);
  const made = own().slice(-3);
  assert.deepEqual(made, [
    { op: 'studioWhipOff' },
    { op: 'studioRope', segments: 16, length: 1.2 },
    { op: 'studioRopeSweep', on: false },
  ]);
  // Back to the whip: the rope goes first (the whip has its own lash).
  editor.undo();
  assert.equal(editor.rope.kind, 'whip');
  assert.deepEqual(own().slice(-3), [
    { op: 'studioRopeOff' },
    { op: 'studioWhip', segments: 20, length: 1.6 },
    { op: 'studioWhipSweep', on: false },
  ]);
});

test('the AI tab shows how to connect an assistant and copies a row', () => {
  const { editor, sent } = setup();
  editor.api.aiBridge = () => ({
    port: 47821,
    listening: true,
    connected: false,
    server: 'C:\\Users\\someone\\kcds\\architect-mcp\\server.cjs',
    token: 'secret',
  });
  editor.pressTab(4);
  const ai = editor.view().dock.ai;
  assert.equal(ai.state, 'waiting');
  assert.equal(ai.rows.length, 3);
  assert.match(ai.rows[0].shown, /%USERPROFILE%/, 'the user folder is not shown');
  editor.copyAi(2);
  assert.deepEqual(sent.at(-1), { op: 'architectCopy', text: 'secret' });
});

test('Leave saves the scene with its whip, then takes the whip away and asks to leave', () => {
  const { editor, sent } = setup();
  editor.pressTile(1);
  editor.leave();
  const save = sent.findLastIndex((message) => message.op === 'architectSave');
  const off = sent.findIndex((message) => message.op === 'studioWhipOff');
  assert.ok(save >= 0 && save < off, 'saved before the whip goes');
  assert.equal(JSON.parse(sent[save].data).rope.kind, 'whip');
  assert.deepEqual(sent.at(-1), { op: 'architectLeave' });
  assert.equal(editor.rope, null);
});

test('the scene exports as a server resource that gives every player the prop', () => {
  const { editor, sent } = setup();
  assert.equal(editor.exportResource(), null);
  editor.pressTile(1);
  editor.changeRope('length', 1);
  const result = editor.exportResource();
  assert.deepEqual(result, { name: 'kcds_studio_whip', files: ['package.json', 'server.js'] });
  const message = sent.find((one) => one.op === 'studioExport');
  assert.equal(message.name, 'kcds_studio_whip');
  const files = Object.fromEntries(message.files);
  assert.deepEqual(JSON.parse(files['package.json']), {
    name: 'kcds_studio_whip',
    version: '1.0.0',
    kcds: { serverScripts: ['server.js'] },
  });
  // The script, run against a stand-in of the server SDK, gives a joining player the whip.
  const given = [];
  let ready = null;
  const kcds = {
    core: { Events: { on: (name, handler) => (ready = name === 'playerReady' ? handler : ready) } },
    // A player already in when the resource starts gets the prop at once.
    players: {
      Player: { all: () => [{ ready: true, setPhysicalProp: (prop) => given.push(prop) }] },
    },
  };
  new Function('kcds', files['server.js'])(kcds);
  assert.equal(given.length, 1);
  ready({ setPhysicalProp: (prop) => given.push(prop) });
  // With its hand, grip and physics, as the studio has them, for both.
  const whip = { kind: 'whip', segments: 20, length: 1.7, ...WHIP_SETTINGS };
  assert.deepEqual(given, [whip, whip]);
  assert.ok(editor.details().rows.some((row) => row.label === 'Export'));
});

test('an animation exports too: alone, or with the prop; joints by name', () => {
  const { editor, sent } = setup();
  editor.jointsKept(['root', 'Hips', 'RightArm'], [-1, 0, 1]);
  editor.setAnimation(
    JSON.stringify({
      keys: [
        { time: 0, pose: { RightArm: [40, 0, 0] } },
        { time: 0.5, pose: { RightArm: [120, 0, 0] } },
      ],
      play: false,
    }),
  );
  // The script defines the animation by name (Animations.define) and plays it by that name.
  const run = (files) => {
    const given = [];
    const defined = {};
    let ready = null;
    const kcds = {
      core: {
        Events: { on: (name, handler) => (ready = name === 'playerReady' ? handler : ready) },
      },
      characters: { Animations: { define: (name, animation) => (defined[name] = animation) } },
      players: { Player: { all: () => [] } },
    };
    new Function('kcds', files['server.js'])(kcds);
    ready({
      setPhysicalProp: (prop) => given.push(['prop', prop]),
      playAnimation: (name) => given.push(['animation', defined[name]]),
    });
    return given;
  };
  assert.deepEqual(editor.exportResource().name, 'kcds_studio_animation');
  const alone = Object.fromEntries(sent.filter((one) => one.op === 'studioExport').at(-1).files);
  const animation = {
    keys: [
      { time: 0, pose: { RightArm: [40, 0, 0] } },
      { time: 0.5, pose: { RightArm: [120, 0, 0] } },
    ],
    blend: 'spline',
    loop: true,
  };
  assert.deepEqual(run(alone), [['animation', animation]]);
  editor.pressTile(1);
  // A prop key: the second key sets the whip's grip; the export carries it.
  editor.setTime(0.5);
  editor.setPropKey(true);
  editor.stepPropKey('pitch', -1);
  assert.equal(editor.exportResource().name, 'kcds_studio_whip');
  const both = Object.fromEntries(sent.filter((one) => one.op === 'studioExport').at(-1).files);
  const keyed = structuredClone(animation);
  keyed.keys[1].prop = { ...PROP_KEY, pitch: 75 };
  assert.deepEqual(run(both), [
    ['prop', { kind: 'whip', segments: 20, length: 1.6, ...WHIP_SETTINGS }],
    ['animation', keyed],
  ]);
});

test('prop keys set the prop’s grip and physics along the animation, blended between keys', () => {
  const { editor, sent } = setup();
  editor.jointsKept(['root', 'Hips', 'RightArm'], [-1, 0, 1]);
  editor.setAnimation(
    JSON.stringify({
      keys: [
        { time: 0, pose: { RightArm: [40, 0, 0] } },
        { time: 1, pose: { RightArm: [120, 0, 0] } },
      ],
      blend: 'linear',
      play: false,
    }),
  );
  // Without a prop the rows are not there.
  assert.ok(!editor.jointDetails().rows.some((row) => row.label === 'PROP AT THIS KEY'));
  editor.pressTile(1);
  const rows = () => editor.jointDetails().rows;
  const row = (label) => rows().findIndex((one) => one.label === label);
  editor.setTime(0);
  assert.equal(rows()[row('Prop key')].toggle, false);
  editor.setPropKey(true);
  assert.deepEqual(editor.animation.keys[0].prop, PROP_KEY);
  editor.setTime(1);
  editor.setPropKey(true);
  for (let step = 0; step < 6; step += 1) {
    editor.stepPropKey('pitch', -1);
  }
  // A whip ignores characters at first (Floor only); this key makes it collide with them (On).
  editor.stepPropKey('collide', -1);
  assert.equal(editor.animation.keys[1].prop.pitch, 0);
  assert.equal(rows()[row('Collide')].value, 'On');
  // Halfway the grip is halfway (linear), collide still the first key's.
  editor.setTime(0.5);
  editor.sendPose();
  const last = (op) => sent.filter((one) => one.op === op).at(-1);
  assert.deepEqual(last('studioPropGrip'), { op: 'studioPropGrip', pitch: 45, twist: 0 });
  assert.equal(last('studioPropPhysics').collide, 2);
  editor.setTime(1);
  editor.sendPose();
  assert.equal(last('studioPropPhysics').collide, 1);
  // The hand: the key switches it, and the grip is mirrored in the left hand.
  editor.stepPropKey('hand', 1);
  assert.equal(last('studioPropHold').joint, 'LeftHand');
  assert.deepEqual(last('studioPropGrip'), { op: 'studioPropGrip', pitch: 0, twist: 0 });
  // Undo takes the prop key's changes back; the project keeps the prop keys.
  editor.undo();
  assert.equal(editor.animation.keys[1].prop.hand, 'RightHand');
  const saved = JSON.parse(sent.filter((one) => one.op === 'architectSave').at(-1).data);
  assert.equal(saved.animation.keys[1].prop.collide, 1);
  // Off again: the key no longer sets the prop.
  editor.setPropKey(false);
  assert.equal(editor.animation.keys[1].prop, undefined);
});
