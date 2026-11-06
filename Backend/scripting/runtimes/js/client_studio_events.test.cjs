'use strict';

// An animation's events in the Asset/Animation Studio: effects and sounds at times,
// kept with the animation, played as the playhead passes them (studio/studio_event.hpp).
const test = require('node:test');
const assert = require('node:assert/strict');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');
const { Animation, eventOf } = require('../../../../Client/resources/studio/animation.cjs');

function setup() {
  const sent = [];
  let clock = 0;
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
    now: () => clock,
  });
  editor.shell = { render: () => {}, viewPointer: () => null };
  const events = () => sent.filter((message) => message.op === 'studioEvent');
  return { editor, sent, events, at: (ms) => (clock = ms) };
}

test('an event is an effect (Library.group.name) or a sound at a time; others are left out', () => {
  assert.deepEqual(
    eventOf({
      time: 0.456,
      kind: 'effect',
      name: 'WH_Particels.dust.sweep',
      joint: 'RightHand',
      scale: 2,
    }),
    { time: 0.46, kind: 'effect', name: 'WH_Particels.dust.sweep', joint: 'RightHand', scale: 2 },
  );
  assert.deepEqual(eventOf({ time: 1, kind: 'sound', name: 'v_horse_whistle', joint: 'Head' }), {
    time: 1,
    kind: 'sound',
    name: 'v_horse_whistle',
  });
  assert.equal(eventOf({ time: 1, kind: 'effect', name: 'not an effect' }), null);
  assert.equal(eventOf({ time: 99, kind: 'sound', name: 'x' }), null);
  assert.equal(eventOf({ time: 1, kind: 'light', name: 'x' }), null);
  const animation = new Animation();
  animation.addEvent({ time: 0.5, kind: 'sound', name: 'b' });
  animation.addEvent({ time: 0.2, kind: 'sound', name: 'a' });
  assert.deepEqual(
    animation.eventsBetween(0, 0.3).map((one) => one.name),
    ['a'],
  );
  // Round the end of a loop.
  assert.deepEqual(
    animation.eventsBetween(0.4, 0.25).map((one) => one.name),
    ['b', 'a'],
  );
  assert.deepEqual(animation.toJSON().events.length, 2);
  assert.equal(new Animation().toJSON().events, undefined, 'no events, no field');
});

test('Animation mode: a tile adds the event at the playhead, it plays as playing passes it', () => {
  const { editor, events, at } = setup();
  editor.useMode('animation');
  assert.equal(editor.details().title, 'Animation events');
  const tiles = editor.view().dock.tiles.map((tile) => tile.name);
  assert.equal(tiles[0], 'blacksmith_hit');
  // Two keys so the animation can play, then an effect at 0.4 s at the left hand.
  editor.animation.addKeyAt(1);
  editor.setTime(0.4);
  const joint = editor.details().rows.findIndex((row) => row.label === 'At joint');
  editor.pressDetail(joint, 1);
  editor.pressTile(0);
  assert.deepEqual(events().at(-1), {
    op: 'studioEvent',
    kind: 'effect',
    name: 'WH_Particels.other.blacksmith_hit',
    joint: 'LeftHand',
    scale: 1,
  });
  assert.equal(editor.animation.events.length, 1);
  // Playing from 0: nothing until 0.4 s, then the effect once.
  const before = events().length;
  editor.setTime(0);
  at(0);
  editor.setPlaying(true);
  at(300);
  editor.playFrame();
  assert.equal(events().length, before);
  at(500);
  editor.playFrame();
  assert.equal(events().length, before + 1);
  editor.setPlaying(false);
  // The project keeps it; undo takes it away; a row's press removes it.
  assert.equal(
    JSON.parse(editor.project ? JSON.stringify(editor.project()) : '{}').animation.events.length,
    1,
  );
  editor.undo();
  assert.equal(editor.animation.events.length, 0);
  editor.redo();
  const row = editor.details().rows.findIndex((one) => one.label === '0.40 s');
  editor.pressDetail(row, 0);
  assert.equal(editor.animation.events.length, 0);
});

test('setAnimation (the MCP) takes events with the keys and says how many it left out', () => {
  const { editor } = setup();
  editor.jointsKept(['root', 'Hips', 'RightArm'], [-1, 0, 1]);
  const answer = editor.setAnimation(
    JSON.stringify({
      keys: [
        { time: 0, pose: { RightArm: [40, 0, 0] } },
        { time: 0.6, pose: { RightArm: [120, 0, 0] } },
      ],
      events: [
        { time: 0.5, kind: 'effect', name: 'WH_Particels.dust.sweep', joint: 'RightHand' },
        { time: 0.5, kind: 'sound', name: 'v_horse_whistle' },
        { time: 0.5, kind: 'effect', name: 'bad' },
      ],
      play: false,
    }),
  );
  assert.equal(answer.events, 2);
  assert.equal(answer.eventsLeftOut, 1);
  assert.equal(editor.animationByName().events.length, 2);
});
