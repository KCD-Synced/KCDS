'use strict';

// The Asset/Animation Studio's Details values dragged sideways (click and hold, as in World
// Architect), and the rope's pinned end picked and moved with the gizmo like its start.
const test = require('node:test');
const assert = require('node:assert/strict');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');

function setup() {
  const sent = [];
  const keys = { mouse1: false };
  const pointer = { x: 900, y: 500 };
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {
      getPosition: () => ({ x: 32.6, y: 28, z: 33.3 }),
      screenLine: () => ({ x: 0, y: 1, z: 0 }),
    },
    Key: { isDown: (key) => Boolean(keys[key]) },
    send: (message) => sent.push(message),
    log: () => {},
  });
  editor.shell = { render: () => {}, viewPointer: () => pointer };
  const last = (op) => sent.filter((message) => message.op === op).at(-1);
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  return { editor, sent, keys, pointer, last, row };
}

test('a value held and dragged sideways steps on; let go, it is one undo step', () => {
  const { editor, keys, row } = setup();
  editor.pressTile(0);
  const before = editor.rope.segments;
  keys.mouse1 = true;
  editor.testDetailsPointer = { x: 100, y: 10 };
  editor.pressDetail(row('Segments'), 0);
  editor.testDetailsPointer = { x: 137, y: 10 };
  editor.scrubOn();
  assert.equal(editor.rope.segments, before + 3 * 2);
  editor.testDetailsPointer = { x: 113, y: 10 };
  editor.scrubOn();
  assert.equal(editor.rope.segments, before + 2);
  const steps = editor.undoStack.length;
  keys.mouse1 = false;
  editor.scrubOn();
  assert.equal(editor.scrub, null);
  assert.ok(editor.undoStack.length < steps);
  editor.undo();
  assert.equal(editor.rope.segments, before);
});

test('a press on - steps once and the drag goes on from there', () => {
  const { editor, keys, row } = setup();
  editor.pressTile(0);
  const before = editor.rope.segments;
  keys.mouse1 = true;
  editor.testDetailsPointer = { x: 50, y: 10 };
  editor.pressDetail(row('Segments'), -1);
  assert.equal(editor.rope.segments, before - 2);
  editor.testDetailsPointer = { x: 26, y: 10 };
  editor.scrubOn();
  assert.equal(editor.rope.segments, before - 6);
  keys.mouse1 = false;
  editor.scrubOn();
  editor.undo();
  assert.equal(editor.rope.segments, before);
});

test('the pinned end is in the gizmo message, is picked and moves; one undo step', () => {
  const { editor, last, row, pointer } = setup();
  editor.pressTile(0);
  editor.pressDetail(row('Pin end'), 0);
  editor.sendGizmo();
  assert.deepEqual(last('studioPropGizmo').end, editor.rope.end.at);
  editor.propPicked('end', -1, editor.rope.end.at, [0, 0, 0]);
  assert.equal(editor.gizmoThing(), 'end');
  assert.equal(editor.gizmoTool(), 'move');
  editor.sendGizmo();
  assert.equal(last('studioPropGizmo').thing, 'end');
  const start = [...editor.rope.end.at];
  editor.propPicked('end', 0, start, [0, 0, 0]);
  assert.equal(editor.propDrag.thing, 'end');
  editor.propDrag.now = { at: [start[0] + 0.5, start[1], start[2]], turn: [0, 0, 0], scale: 1 };
  editor.releasePropDrag();
  assert.deepEqual(editor.rope.end.at, [start[0] + 0.5, start[1], start[2]]);
  assert.deepEqual(last('studioRopeEnd'), {
    op: 'studioRopeEnd',
    on: true,
    at: editor.rope.end.at,
  });
  editor.undo();
  assert.deepEqual(editor.rope.end.at, start);
  assert.ok(pointer);
});

test('choosing the prop in the outliner puts the gizmo back on its start', () => {
  const { editor } = setup();
  editor.pressTile(0);
  editor.setEndPinned(true);
  editor.propPicked('end', -1, editor.rope.end.at, [0, 0, 0]);
  assert.equal(editor.gizmoThing(), 'end');
  const line = editor.outlinerLines().findIndex((one) => one.thing === 'rope');
  editor.pressOutliner(line);
  assert.equal(editor.gizmoThing(), 'prop');
});
