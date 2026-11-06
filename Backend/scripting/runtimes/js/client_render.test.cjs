'use strict';

// Render.view (Client/resources/render.cjs): a picture from any camera, asked of the
// game client as renderView with the shot's checks; the PNG comes back in pieces.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceRender } = require('../../../../Client/resources/render.cjs');

function setup() {
  const sent = [];
  const timers = [];
  const render = new ResourceRender({
    send: (message) => sent.push(message),
    timers: {
      setTimeout: (callback, ms) => {
        timers.push({ callback, ms, cleared: false });
        return timers.length - 1;
      },
      clearTimeout: (handle) => {
        timers[handle].cleared = true;
      },
    },
  });
  return { Render: render.facade(), render, sent, timers };
}

test('view asks the game client with the checked pose and resolves with the PNG pieces joined', async () => {
  const { Render, render, sent, timers } = setup();
  const answer = Render.view({
    position: { x: 1, y: 2, z: 3 },
    lookAt: { x: 1, y: 5, z: 3 },
    fov: 50,
    width: 640,
  });
  assert.deepEqual(sent, [
    {
      op: 'renderView',
      request: 1,
      position: { x: 1, y: 2, z: 3 },
      lookAt: { x: 1, y: 5, z: 3 },
      roll: 0,
      fov: 50,
      width: 640,
    },
  ]);
  render.receiveChunk({ request: 1, data: 'iVBO' });
  render.receiveChunk({ request: 1, data: 'Rw==' });
  render.receiveDone({ request: 1, chunks: 2, width: 640, height: 360 });
  assert.deepEqual(await answer, {
    png: 'data:image/png;base64,iVBORw==',
    width: 640,
    height: 360,
  });
  assert.equal(timers[0].cleared, true);
});

test('a failure, a missing piece and a timeout reject; a bad pose or width throws', async () => {
  const { Render, render, timers } = setup();
  const failed = Render.view({
    position: { x: 0, y: 0, z: 0 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  render.receiveFailed({ request: 1, reason: 'the world changed' });
  await assert.rejects(failed, /Render\.view failed: the world changed/);

  const partial = Render.view({
    position: { x: 0, y: 0, z: 0 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  render.receiveChunk({ request: 2, data: 'AAAA' });
  render.receiveDone({ request: 2, chunks: 2, width: 1, height: 1 });
  await assert.rejects(partial, /did not arrive whole/);

  const slow = Render.view({
    position: { x: 0, y: 0, z: 0 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  timers[2].callback();
  await assert.rejects(slow, /timed out/);
  // A late answer of a render given up is dropped.
  render.receiveDone({ request: 3, chunks: 0, width: 1, height: 1 });

  assert.throws(() => Render.view(null), TypeError);
  assert.throws(() => Render.view({ position: { x: 0, y: 0, z: 0 } }), /rotation or lookAt/);
  assert.throws(
    () =>
      Render.view({
        position: { x: 0, y: 0, z: 0 },
        rotation: { x: 0, y: 0, z: 0, w: 1 },
        width: 8,
      }),
    RangeError,
  );
});
