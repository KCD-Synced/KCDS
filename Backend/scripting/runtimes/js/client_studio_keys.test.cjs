'use strict';

// Recording a prop's simulation into keys in the Asset/Animation Studio: Physics
// mode asks the game client to record (studio/studio_rope.hpp studioRopeRecord), keeps the keys
// it answers with the rope (undo, project, export) and plays them back (studioRopePlay).
const test = require('node:test');
const assert = require('node:assert/strict');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');

function setup() {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {
      getPosition: () => ({ x: 32, y: 28, z: 33 }),
      screenLine: () => ({ x: 0, y: 1, z: 0 }),
    },
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
  });
  editor.shell = { render: () => {}, viewPointer: () => ({ x: 0, y: 0 }) };
  const last = (op) => sent.filter((message) => message.op === op).at(-1);
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  return { editor, sent, last, row };
}

// Keys of a rope of `points` points: each frame its points a little lower.
function frames(count, points) {
  return Array.from({ length: count }, (_, frame) =>
    Array.from({ length: points * 3 }, (_, part) => (part % 3 === 2 ? 33 - frame * 0.01 : 32)),
  );
}

test('Physics mode records the simulation, keeps the keys and plays them back', () => {
  const { editor, last, row } = setup();
  editor.pressTile(0);
  editor.useMode('physics');
  assert.equal(row('Keys'), -1);
  editor.pressDetail(row('Seconds'), 1);
  editor.pressDetail(row('Record'), 0);
  assert.deepEqual(last('studioRopeRecord'), { op: 'studioRopeRecord', seconds: 4 });
  assert.equal(editor.recordingKeys, true);
  const segments = editor.rope.segments;
  // The game client sends a recording in parts of 30 keys; the last part makes the keys.
  const recorded = frames(90, segments + 1);
  editor.ropeRecorded(30, recorded.slice(0, 30), 0, 3);
  editor.ropeRecorded(30, recorded.slice(30, 60), 1, 3);
  assert.equal(editor.recordingKeys, true);
  editor.ropeRecorded(30, recorded.slice(60), 2, 3);
  assert.equal(editor.recordingKeys, false);
  assert.equal(editor.rope.keys.frames.length, 90);
  assert.deepEqual(editor.rope.keys.frames[45], recorded[45]);
  assert.equal(editor.details().rows[row('Keys')].value, '90 (3.0 s)');
  // Kept in the project.
  assert.equal(editor.project().rope.keys.frames.length, 90);
  // Played back in parts too: three messages of 30 keys, the last one plays them.
  editor.pressDetail(row('Play keys'), 0);
  const play = last('studioRopePlay');
  assert.equal(play.rate, 30);
  assert.equal(play.part, 2);
  assert.equal(play.parts, 3);
  assert.equal(play.frames.length, 30);
  assert.deepEqual(play.frames[0], recorded[60]);
  assert.equal(editor.keysPlaying, true);
  editor.pressDetail(row('Play keys'), 0);
  assert.deepEqual(last('studioRopePlay'), { op: 'studioRopePlay', frames: [] });
});

test('keys are one undo step, removed in Details, and dropped with another rope', () => {
  const { editor, last, row } = setup();
  editor.pressTile(0);
  editor.useMode('physics');
  editor.ropeRecorded(30, frames(10, editor.rope.segments + 1));
  editor.undo();
  assert.equal(editor.rope.keys, undefined);
  editor.redo();
  assert.equal(editor.rope.keys.frames.length, 10);
  editor.playKeys(true);
  editor.pressDetail(row('Remove keys'), 0);
  assert.equal(editor.rope.keys, undefined);
  assert.deepEqual(last('studioRopePlay'), { op: 'studioRopePlay', frames: [] });
  editor.ropeRecorded(30, frames(10, editor.rope.segments + 1));
  editor.useMode('props');
  editor.changeRope('segments', 1);
  assert.equal(editor.rope.keys, undefined);
});

test('a recording with nothing in it or uneven keys is not kept', () => {
  const { editor } = setup();
  editor.pressTile(0);
  editor.ropeRecorded(30, []);
  assert.equal(editor.rope.keys, undefined);
  editor.ropeRecorded(30, [
    [1, 2, 3],
    [1, 2],
  ]);
  assert.equal(editor.rope.keys, undefined);
  assert.match(editor.message, /Nothing was recorded/);
});

test('the export carries small keys as rope_keys.json; the script steps record and play', () => {
  const { editor, last } = setup();
  editor.pressTile(0);
  editor.ropeRecorded(30, frames(30, editor.rope.segments + 1));
  const exported = editor.exportResource();
  assert.ok(exported.files.includes('rope_keys.json'));
  const file = last('studioExport').files.find(([path]) => path === 'rope_keys.json')[1];
  assert.equal(JSON.parse(file).frames.length, 30);
  return Promise.resolve(editor.script('record 2')).then((answer) => {
    assert.match(answer, /"seconds":2/);
    assert.deepEqual(last('studioRopeRecord'), { op: 'studioRopeRecord', seconds: 2 });
  });
});
