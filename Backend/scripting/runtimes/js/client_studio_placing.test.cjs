'use strict';

// Where things stand in the Asset/Animation Studio: the prop held in a hand
// or free in the scene (Client/resources/studio/placing.cjs, studio/studio_hold.hpp), and the
// props' gizmo dragging the prop and your mesh (studio/studio_prop_gizmo.hpp).
const test = require('node:test');
const assert = require('node:assert/strict');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');
const placing = require('../../../../Client/resources/studio/placing.cjs');

function near(actual, expected, digits = 3) {
  assert.equal(actual.length, expected.length);
  actual.forEach((part, index) => {
    assert.ok(Math.abs(part - expected[index]) < 10 ** -digits, `${actual} is not ${expected}`);
  });
}

// An editor whose camera stands south of the prop's free place, looking north along +y; `aim`
// is the line a press on the stage goes along.
function setup() {
  const sent = [];
  const camera = { from: { x: 32.6, y: 28, z: 33.3 }, aim: { x: 0, y: 1, z: 0 } };
  const pointer = { x: 900, y: 500 };
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {
      getPosition: () => camera.from,
      screenLine: () => camera.aim,
    },
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
  });
  editor.shell = { render: () => {}, viewPointer: () => pointer };
  const last = (op) => sent.filter((message) => message.op === op).at(-1);
  return { editor, sent, camera, pointer, last };
}

test('turns in degrees go to quaternions and back; a turn about a world axis adds up', () => {
  near(placing.degreesOf(placing.quaternionOf([10, 20, 30])), [10, 20, 30]);
  near(placing.turnAbout([0, 0, 0], 2, 90), [0, 0, 90], 1);
  near(placing.turnAbout([0, 0, 90], 2, 45), [0, 0, 135], 1);
  // About x after z, the world's x: x goes to y (z), then y goes up (x).
  const [qx, qy, qz, qw] = placing.quaternionOf(placing.turnAbout([0, 0, 90], 0, 90));
  const v = [1, 0, 0];
  const t = [2 * (qy * v[2] - qz * v[1]), 2 * (qz * v[0] - qx * v[2]), 2 * (qx * v[1] - qy * v[0])];
  const turned = [
    v[0] + qw * t[0] + (qy * t[2] - qz * t[1]),
    v[1] + qw * t[1] + (qz * t[0] - qx * t[2]),
    v[2] + qw * t[2] + (qx * t[1] - qy * t[0]),
  ];
  near(turned, [0, 0, 1]);
});

test('a press line meets a world axis where it passes nearest', () => {
  // The x axis through the origin; a line from (1, -5, 0) going north meets it at x = 1.
  assert.equal(placing.alongAxis([0, 0, 0], 0, [1, -5, 0], [0, 1, 0]), 1);
  // Along the axis itself: no answer.
  assert.equal(placing.alongAxis([0, 0, 0], 1, [0, -5, 0], [0, 1, 0]), null);
});

test('Held in: right hand, left hand, free in the scene with its place and turn; undo', () => {
  const { editor, last } = setup();
  editor.pressTile(0);
  assert.deepEqual(last('studioPropHold'), {
    op: 'studioPropHold',
    joint: 'RightHand',
    at: [32.6, 31.4, 33.3],
    turn: [0, 0, 0],
  });
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(editor.details().rows[row('Held in')].value, 'Right hand');
  editor.pressDetail(row('Held in'), 1);
  assert.equal(last('studioPropHold').joint, 'LeftHand');
  assert.equal(editor.view().toolbar.snaps[0].label, 'Left hand');
  // The toolbar's chip goes round too.
  editor.pressSnap(0);
  assert.equal(last('studioPropHold').joint, '');
  assert.equal(editor.details().rows[row('Held in')].value, 'Free in the scene');
  editor.pressDetail(row('Place X'), 1);
  editor.pressDetail(row('Turn Z'), 1);
  assert.deepEqual(last('studioPropHold'), {
    op: 'studioPropHold',
    joint: '',
    at: [32.65, 31.4, 33.3],
    turn: [0, 0, 15],
  });
  editor.undo();
  editor.undo();
  editor.undo();
  assert.equal(last('studioPropHold').joint, 'LeftHand');
  assert.equal(row('Place X'), -1, 'no place rows in a hand');
});

test('a whip in the left hand gets the grip mirrored; a new prop stays in that hand', () => {
  const { editor, last } = setup();
  editor.pressTile(1);
  assert.deepEqual(last('studioPropGrip'), { op: 'studioPropGrip', pitch: 90, twist: 0 });
  editor.changeHold(1);
  assert.equal(last('studioPropHold').joint, 'LeftHand');
  assert.deepEqual(last('studioPropGrip'), { op: 'studioPropGrip', pitch: -90, twist: 0 });
  editor.pressTile(0);
  assert.equal(editor.rope.kind, 'rope');
  assert.equal(editor.rope.hold.joint, 'LeftHand');
});

test('a rope’s end pins to a point and moves in Details; undo lets it go; a whip has none', () => {
  const { editor, last } = setup();
  editor.pressTile(0);
  assert.deepEqual(last('studioRopeEnd'), {
    op: 'studioRopeEnd',
    on: false,
    at: [33.4, 31.4, 33.3],
  });
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(row('End X'), -1);
  editor.pressDetail(row('Pin end'), 0);
  assert.deepEqual(last('studioRopeEnd'), {
    op: 'studioRopeEnd',
    on: true,
    at: [33.4, 31.4, 33.3],
  });
  editor.pressDetail(row('End Z'), -1);
  assert.deepEqual(last('studioRopeEnd').at, [33.4, 31.4, 33.25]);
  editor.undo();
  editor.undo();
  assert.equal(last('studioRopeEnd').on, false);
  editor.pressTile(1);
  assert.equal(editor.rope.end, undefined);
  assert.equal(row('Pin end'), -1);
});

test('dragging the gizmo’s X arrow sets the prop free and moves it; one undo step', () => {
  const { editor, camera, pointer, last } = setup();
  editor.pressTile(0);
  assert.equal(last('studioPropGizmo').thing, 'prop');
  assert.equal(last('studioPropGizmo').prop, true);
  editor.pressView(900, 500);
  assert.deepEqual(last('studioPickProp'), {
    op: 'studioPickProp',
    from: [32.6, 28, 33.3],
    direction: [0, 1, 0],
  });
  // The game client: the X arrow of the prop in the right hand, which is at the free place.
  editor.propPicked('prop', 0, [32.6, 31.4, 33.3], [0, 0, 0]);
  assert.equal(last('studioPropGizmo').axis, 0);
  const steps = editor.undoStack.length;
  camera.aim = { x: 0.2, y: 3.4, z: 0 };
  pointer.x = 950;
  editor.followPropDrag();
  assert.deepEqual(last('studioPropHold'), {
    op: 'studioPropHold',
    joint: '',
    at: [32.8, 31.4, 33.3],
    turn: [0, 0, 0],
  });
  assert.equal(editor.undoStack.length, steps, 'no step while dragging');
  editor.releaseView(950);
  assert.equal(editor.undoStack.length, steps + 1);
  assert.deepEqual(editor.rope.hold, { joint: '', at: [32.8, 31.4, 33.3], turn: [0, 0, 0] });
  assert.equal(last('studioPropGizmo').axis, -1);
  editor.undo();
  assert.equal(editor.rope.hold.joint, 'RightHand');
});

test('the rotate tool turns the prop about the axis dragged across', () => {
  const { editor, pointer } = setup();
  editor.pressTile(1);
  editor.useTool('rotate');
  editor.propPicked('prop', 2, [32.6, 31.4, 33.3], [0, 0, 0]);
  pointer.x += 60;
  editor.followPropDrag();
  editor.releaseView(pointer.x);
  near(editor.rope.hold.turn, [0, 0, 30], 1);
});

test('your mesh moves, turns and sizes with the gizmo and Details; the project keeps it', () => {
  const { editor, pointer, last, sent } = setup();
  editor.meshesKept(['suzanne']);
  editor.pressFolder(1);
  editor.pressTile(0);
  assert.equal(last('studioPropGizmo').thing, 'mesh');
  assert.deepEqual(last('studioMesh'), {
    op: 'studioMesh',
    name: 'suzanne',
    at: [32.64, 31.36, 33],
    turn: [0, 0, 0],
    scale: 1,
    joint: '',
  });
  editor.useTool('scale');
  assert.equal(last('studioPropGizmo').tool, 'scale');
  editor.propPicked('mesh', 0, [32.64, 31.36, 33], [0, 0, 0]);
  pointer.x += 200;
  editor.followPropDrag();
  assert.equal(last('studioMeshPlace').scale, 2);
  editor.releaseView(pointer.x);
  assert.equal(editor.meshPlace.scale, 2);
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  editor.pressDetail(row('Place Z'), -1);
  assert.deepEqual(last('studioMeshPlace').at, [32.64, 31.36, 32.95]);
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.deepEqual(saved.meshPlace, {
    at: [32.64, 31.36, 32.95],
    turn: [0, 0, 0],
    scale: 2,
    joint: '',
  });
  editor.undo();
  editor.undo();
  assert.equal(editor.meshPlace.scale, 1);
  // A rope is not sized: the scale tool moves it.
  editor.pressFolder(0);
  editor.pressTile(0);
  assert.equal(last('studioPropGizmo').tool, 'move');
});

test('your mesh held in a hand or a joint: Details and MCP, a drag sets it free, the export stands it', async () => {
  const { editor, pointer, last, sent } = setup();
  editor.meshesKept(['crate']);
  editor.pressFolder(1);
  editor.pressTile(0);
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(editor.details().rows[row('Held in')].value, 'Free in the scene');
  // Held in: the right hand first (HOLDS after "free"), in the palm, the size kept.
  editor.pressDetail(row('Place Z'), 1);
  editor.stepMesh('scale', 0, 1);
  const steps = editor.undoStack.length;
  editor.pressDetail(row('Held in'), 1);
  assert.equal(editor.undoStack.length, steps + 1);
  assert.deepEqual(last('studioMeshPlace'), {
    op: 'studioMeshPlace',
    at: [0, 0, 0],
    turn: [0, 0, 0],
    scale: 1.25,
    joint: 'RightHand',
  });
  assert.equal(editor.details().rows[row('Held in')].value, 'Right hand');
  assert.match(editor.message, /Place and Turn are now in the hand/);
  // Place steps are now in the hand.
  editor.pressDetail(row('Place Z'), 1);
  assert.deepEqual(last('studioMeshPlace').at, [0, 0, 0.05]);
  assert.equal(last('studioMeshPlace').joint, 'RightHand');
  // Over MCP: any joint by name, at its own place unless at is given; the project keeps it.
  await editor.script('place {"mesh":{"joint":"Head","turn":[0,0,90]}}');
  assert.deepEqual(editor.meshPlace, {
    at: [0, 0, 0],
    turn: [0, 0, 90],
    scale: 1.25,
    joint: 'Head',
  });
  assert.equal(editor.details().rows[row('Held in')].value, 'Head');
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.equal(saved.meshPlace.joint, 'Head');
  // A kept project holds it again.
  const fresh = setup();
  fresh.editor.projectKept(JSON.stringify(saved));
  assert.equal(fresh.last('studioMesh').joint, 'Head');
  // The export stands a held mesh where a free one stands at first.
  editor.exportResource();
  const files = sent.filter((message) => message.op === 'studioExport').at(-1).files;
  const server = files.find(([path]) => path === 'server.js')[1];
  assert.match(
    server,
    /const MESH_PLACE = \{"offset":\[0\.64,-0\.64,1\],"turn":\[0,0,0\],"scale":1\.25\};/,
  );
  // Dragging the gizmo sets it free where the game client found it.
  editor.useTool('move');
  editor.propPicked('mesh', 0, [32.9, 31.3, 33.4], [0, 0, 90]);
  pointer.x += 40;
  editor.followPropDrag();
  assert.equal(last('studioMeshPlace').joint, undefined);
  editor.releaseView(pointer.x);
  assert.equal(editor.meshPlace.joint, '');
  assert.equal(editor.meshPlace.at[2], 33.4);
  // Back in the hand, then free again: beside the character.
  editor.pressDetail(row('Held in'), -1);
  assert.equal(editor.meshPlace.joint, 'LeftHand');
  editor.pressDetail(row('Held in'), 1);
  assert.equal(editor.meshPlace.joint, '');
  assert.deepEqual(editor.meshPlace.at, [32.64, 31.36, 33]);
});

test('Textures mode puts your textures on your mesh; tiling, undo, the project and a reload keep it', () => {
  const { editor, last, sent } = setup();
  editor.useMode('textures');
  assert.ok(sent.some((message) => message.op === 'studioTextureList'));
  editor.texturesKept(['bricks', 'leather']);
  assert.deepEqual(
    editor.view().dock.tiles.map((tile) => tile.name),
    ['bricks', 'leather'],
  );
  // Without a mesh: nothing is sent, the message says why.
  editor.pressTile(0);
  assert.equal(last('studioTexture'), undefined);
  assert.match(editor.message, /Show a mesh first/);
  editor.meshesKept(['suzanne']);
  editor.useMode('props');
  editor.pressFolder(1);
  editor.pressTile(0);
  editor.useMode('textures');
  editor.pressTile(1);
  assert.deepEqual(last('studioTexture'), { op: 'studioTexture', name: 'leather', tiling: 1 });
  assert.equal(editor.view().dock.tile, 1);
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  editor.pressDetail(row('Tiling'), 1);
  assert.deepEqual(last('studioTexture'), { op: 'studioTexture', name: 'leather', tiling: 2 });
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.deepEqual(saved.meshTexture, { name: 'leather', tiling: 2 });
  // Reload: the mesh comes anew, so the texture goes on again.
  editor.reloadMesh();
  assert.equal(last('studioMesh').name, 'suzanne');
  assert.deepEqual(last('studioTexture'), { op: 'studioTexture', name: 'leather', tiling: 2 });
  editor.undo();
  editor.undo();
  assert.deepEqual(last('studioTexture'), { op: 'studioTexture', name: '', tiling: 1 });
  editor.texturedKept({
    name: 'leather',
    ok: false,
    width: 0,
    height: 0,
    error: 'no such texture file',
  });
  assert.match(editor.message, /no such texture file/);
});

test('your mesh in the animation: mesh keys place it, it moves between them, the export moves it', async () => {
  const { editor, last, sent } = setup();
  editor.meshesKept(['crate']);
  editor.pressFolder(1);
  editor.pressTile(0);
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
  // Key 1: where the mesh stands now; key 2: 1 m higher and turned.
  editor.setTime(0);
  assert.match(await editor.script('meshkey on'), /"key":0,"time":0,"at":\[32\.64,31\.36,33\]/);
  editor.useMode('props');
  editor.selected = 'mesh';
  await editor.script('place {"mesh":{"at":[32.64,31.36,34],"turn":[0,0,90]}}');
  editor.setTime(1);
  await editor.script('meshkey on');
  assert.equal(editor.animation.keys[1].mesh.at[2], 34);
  // Halfway between them (linear blend): half as high and half turned.
  editor.setTime(0.5);
  assert.deepEqual(last('studioMeshPlace'), {
    op: 'studioMeshPlace',
    at: [32.64, 31.36, 33.5],
    turn: [0, 0, 45],
    scale: 1,
    joint: '',
  });
  // Saved with the animation, and taken out again with undo.
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.ok(JSON.stringify(saved).includes('"mesh":{"at":[32.64,31.36,34],"turn":[0,0,90]}'));
  // The export: the animation without mesh keys, and the server moving each player's mesh.
  editor.exportResource();
  const files = Object.fromEntries(
    sent.filter((message) => message.op === 'studioExport').at(-1).files,
  );
  assert.ok(!files['server.js'].includes('"mesh":{'));
  const poses = [];
  const timers = [];
  let now = 1000;
  let ready = null;
  const kcds = {
    core: { Events: { on: (name, handler) => (ready = name === 'playerReady' ? handler : ready) } },
    characters: { Animations: { define: () => {} } },
    players: { Player: { all: () => [] } },
    math: {
      Vector3: class {
        constructor(x, y, z) {
          Object.assign(this, { x, y, z });
        }
      },
    },
    world: {
      Prop: {
        spawn: () => ({ setPose: (place, turn) => poses.push([place, turn]), destroy: () => {} }),
      },
    },
  };
  const clock = { now: () => now };
  new Function('kcds', 'setInterval', 'clearInterval', 'Date', files['server.js'])(
    kcds,
    (run) => timers.push(run),
    () => {},
    clock,
  );
  ready({ id: 7, position: { x: 100, y: 200, z: 30 }, playAnimation: () => {} });
  assert.equal(timers.length, 1);
  now += 500;
  timers[0]();
  const [place, turn] = poses.at(-1);
  // Half a second in: 0.5 m above where it stood at first (1 m from the feet), turned 45°.
  assert.deepEqual(
    [place.x, place.y, place.z].map((part) => Math.round(part * 1000) / 1000),
    [100.64, 199.36, 31.5],
  );
  assert.deepEqual([turn.x, turn.y, turn.z], [0, 0, 45]);
  // Taking the key out: the mesh stands where it was placed again once none is left.
  editor.setTime(1);
  await editor.script('meshkey off');
  editor.setTime(0);
  await editor.script('meshkey off');
  assert.equal(editor.animation.hasMeshKeys(), false);
  assert.deepEqual(last('studioMeshPlace').at, editor.meshPlace.at);
});

test('Physics mode gives your mesh a body: solid or falling, its weight, cloth, the project, MCP', async () => {
  const { editor, last, sent } = setup();
  editor.meshesKept(['crate']);
  editor.useMode('props');
  editor.pressFolder(1);
  editor.pressTile(0);
  // A mesh shown anew has no body: nothing is sent until one is chosen.
  assert.equal(last('studioMeshBody'), undefined);
  editor.useMode('physics');
  editor.selected = 'mesh';
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(editor.details().rows[row('Body')].value, 'None: stands as placed');
  editor.pressDetail(row('Body'), 1);
  assert.deepEqual(last('studioMeshBody'), { op: 'studioMeshBody', kind: 'solid', mass: 5 });
  editor.pressDetail(row('Body'), 1);
  assert.deepEqual(last('studioMeshBody'), { op: 'studioMeshBody', kind: 'falls', mass: 5 });
  assert.match(editor.message, /falls from where it is placed/);
  editor.pressDetail(row('Body weight'), 1);
  assert.deepEqual(last('studioMeshBody'), { op: 'studioMeshBody', kind: 'falls', mass: 6.25 });
  assert.equal(editor.details().rows[row('Body weight')].value, '6.25 kg');
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.deepEqual(saved.meshBody, { kind: 'falls', mass: 6.25 });
  // A kept project gives it the body again.
  const fresh = setup();
  fresh.editor.projectKept(JSON.stringify(saved));
  assert.deepEqual(fresh.last('studioMeshBody'), {
    op: 'studioMeshBody',
    kind: 'falls',
    mass: 6.25,
  });
  // The game client finds no mass in its collision: no body, and the editor says so.
  editor.bodiedKept({ ok: true, error: '' });
  assert.equal(editor.meshBody.kind, 'falls');
  editor.bodiedKept({ ok: false, error: 'it has no collision of its own, so it cannot fall' });
  assert.equal(editor.meshBody.kind, 'none');
  assert.match(editor.message, /^crate: it has no collision of its own, so it cannot fall\.$/);
  editor.pressDetail(row('Body'), -1);
  assert.equal(editor.meshBody.kind, 'falls');
  // Cloth takes the body away, a body makes the cloth solid.
  editor.pressDetail(row('Cloth'), 0);
  assert.equal(last('studioMeshBody').kind, 'none');
  assert.equal(last('studioMeshCloth').on, true);
  assert.match(await editor.script('body solid'), /"kind":"solid"/);
  assert.equal(last('studioMeshCloth').on, false);
  assert.match(await editor.script('body lava'), /body is one of none, solid, falls/);
  // Undo brings the cloth back; taking the mesh away tells the game client it has no body.
  editor.undo();
  assert.equal(editor.meshCloth.on, true);
  assert.equal(editor.meshBody.kind, 'none');
  await editor.script('body falls 2');
  assert.deepEqual(editor.meshBody, { kind: 'falls', mass: 2 });
  await editor.script('mesh');
  assert.deepEqual(last('studioMeshBody'), { op: 'studioMeshBody', kind: 'none', mass: 2 });
});

test('Physics mode makes your mesh cloth; its weight, undo, the project and removing it keep it right', () => {
  const { editor, last, sent } = setup();
  editor.meshesKept(['clothtest']);
  editor.useMode('props');
  editor.pressFolder(1);
  editor.pressTile(0);
  editor.useMode('physics');
  editor.selected = 'mesh';
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(editor.details().rows[row('Cloth')].toggle, false);
  editor.pressDetail(row('Cloth'), 0);
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: true, mass: 1 });
  assert.match(editor.message, /hangs from its highest vertices/);
  editor.pressDetail(row('Weight'), 1);
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: true, mass: 1.25 });
  const saved = JSON.parse(sent.filter((message) => message.op === 'architectSave').at(-1).data);
  assert.deepEqual(saved.meshCloth, { on: true, mass: 1.25 });
  // A kept project makes it cloth again after showing it.
  const fresh = setup();
  fresh.editor.projectKept(JSON.stringify(saved));
  const again = fresh.sent.map((message) => message.op);
  assert.ok(again.includes('studioMesh'));
  assert.ok(again.indexOf('studioMeshCloth') > again.indexOf('studioMesh'));
  assert.deepEqual(fresh.last('studioMeshCloth'), { op: 'studioMeshCloth', on: true, mass: 1.25 });
  editor.undo();
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: true, mass: 1 });
  editor.undo();
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: false, mass: 1 });
  // Cloth when taken away: the game client is told it is solid.
  editor.setCloth(true, 50);
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: true, mass: 10 });
  editor.showMesh('');
  assert.deepEqual(last('studioMeshCloth'), { op: 'studioMeshCloth', on: false, mass: 10 });
  assert.equal(editor.meshCloth.on, false);
});

test('the Saves tab: the name button opens it, a typed name saves, Load brings a save back', () => {
  const { editor, last } = setup();
  assert.equal(editor.view().menu.saves, true);
  // The menu's name button (shell handlers.architect) opens the Saves tab.
  const saves = editor.view().dock.tabs.indexOf('Saves');
  editor.pressTab(saves);
  assert.equal(editor.view().dock.tabs[saves], 'Saves');
  assert.deepEqual(last('studioSaveList'), { op: 'studioSaveList' });
  assert.equal(editor.view().dock.saves.name, 'Name of the save');
  // Typing: letters, a space, shift+minus is _; Enter ends.
  editor.startTyping();
  for (const key of ['w', 'h', 'i', 'p', 'space']) {
    editor.type(key, false);
  }
  editor.type('minus', true);
  editor.type('1', false);
  editor.type('enter', false);
  assert.equal(editor.saveName, 'whip _1');
  assert.equal(editor.typing, false);
  editor.pressTile(0);
  editor.saveAs();
  const saved = last('studioSaveAs');
  assert.equal(saved.name, 'whip _1');
  assert.equal(JSON.parse(saved.data).rope.kind, 'rope');
  editor.savesKept([{ name: 'whip _1', bytes: 900, time: 1700000000 }]);
  assert.equal(editor.view().dock.saves.rows[0].name, 'whip _1');
  // A bad name is refused.
  editor.saveName = ' ';
  editor.saveAs();
  assert.match(editor.message, /letters, digits/);
  // Load: asked, then the save's scene replaces the one now (here: no prop).
  editor.pressSave(0, 'load');
  assert.deepEqual(last('studioSaveLoad'), { op: 'studioSaveLoad', name: 'whip _1' });
  editor.saveLoaded(
    'whip _1',
    JSON.stringify({ version: 1, rope: null, animation: { keys: [{ time: 0, pose: {} }] } }),
  );
  assert.equal(editor.rope, null);
  assert.equal(editor.undoStack.length, 0);
  assert.equal(editor.view().menu.map, 'whip _1');
  editor.saveLoaded('broken', 'not json');
  assert.match(editor.message, /could not be read/);
  editor.pressSave(0, 'delete');
  assert.deepEqual(last('studioSaveDelete'), { op: 'studioSaveDelete', name: 'whip _1' });
});

test('your mesh exports in the stream folder and stands beside each player who joins', () => {
  const { editor, last } = setup();
  editor.meshesKept(['suzanne']);
  editor.pressFolder(1);
  editor.pressTile(0);
  editor.texturesKept(['checker']);
  editor.setTexture('checker', 2);
  editor.stepMesh('scale', 0, 1);
  assert.equal(editor.exportResource().name, 'kcds_studio_mesh');
  const message = last('studioExport');
  assert.deepEqual(message.mesh, { name: 'suzanne', texture: 'checker', tiling: 2 });
  const files = Object.fromEntries(message.files);
  const spawned = [];
  const destroyed = [];
  const handlers = {};
  const kcds = {
    core: { Events: { on: (name, handler) => (handlers[name] = handler) } },
    math: {
      Vector3: class {
        constructor(x, y, z) {
          Object.assign(this, { x, y, z });
        }
      },
    },
    world: {
      Prop: {
        spawn: (model, at, turn, scale) => {
          spawned.push({ model, at, turn, scale });
          return { destroy: () => destroyed.push(model) };
        },
      },
    },
    players: { Player: { all: () => [] } },
  };
  new Function('kcds', files['server.js'])(kcds);
  handlers.playerReady({ id: 7, position: { x: 100, y: 200, z: 30 } });
  assert.equal(spawned.length, 1);
  assert.equal(spawned[0].model, 'objects/kcds/kcds_studio_mesh/suzanne.cgf');
  // The studio's place from the character's feet (32, 32, 32): 0.64, -0.64, 1.
  assert.deepEqual([spawned[0].at.x, spawned[0].at.y, spawned[0].at.z], [100.64, 199.36, 31]);
  assert.equal(spawned[0].scale, 1.25);
  handlers.playerDisconnect({ id: 7 });
  assert.deepEqual(destroyed, ['objects/kcds/kcds_studio_mesh/suzanne.cgf']);
});

test('a press on nothing says so and Bones mode has no props’ gizmo', () => {
  const { editor, last } = setup();
  editor.pressTile(0);
  editor.propPicked('', -1, [0, 0, 0], [0, 0, 0]);
  assert.match(editor.message, /Nothing there/);
  editor.useMode('bones');
  assert.equal(last('studioPropGizmo').thing, '');
  editor.pressView(900, 500);
  assert.equal(last('studioPickJoint').op, 'studioPickJoint');
});
