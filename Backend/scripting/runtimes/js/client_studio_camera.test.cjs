'use strict';

// The studio's camera as in Blender (Client/resources/studio/camera.cjs): orbit,
// pan, zoom, the front, side and top views, framing what is chosen.
const test = require('node:test');
const assert = require('node:assert/strict');
const camera = require('../../../../Client/resources/studio/camera.cjs');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');

function near(actual, expected, digits = 3) {
  actual.forEach((part, index) => {
    assert.ok(Math.abs(part - expected[index]) < 10 ** -digits, `${actual} is not ${expected}`);
  });
}

test('an orbit and the camera pose go both ways; the views stand where Blender’s do', () => {
  const target = [32, 32, 33];
  const orbit = camera.orbitOf([35, 32, 33], target);
  near([orbit.yaw, orbit.pitch, orbit.distance], [0, 0, 3]);
  const { place, forward } = camera.poseOf(orbit);
  near(place, [35, 32, 33]);
  near(forward, [-3, 0, 0]);
  // Front: from -y looking along +y; side: from +x; top: from above.
  near(camera.poseOf(camera.viewOf('front', target)).place, [32, 29, 33]);
  near(camera.poseOf(camera.viewOf('side', target)).place, [35, 32, 33]);
  assert.ok(camera.poseOf(camera.viewOf('top', target)).place[2] > 35.9);
  assert.equal(camera.viewOf('back', target), null);
});

test('dragging orbits round the target, pans it with the screen and zooms in and out', () => {
  const orbit = camera.viewOf('side', [0, 0, 0]);
  const turned = camera.dragged(orbit, 'orbit', 100, 0);
  near([turned.distance, turned.target[0]], [3, 0]);
  assert.ok(turned.yaw < orbit.yaw);
  const up = camera.dragged(orbit, 'orbit', 0, -10000);
  near([up.pitch], [1.5], 6);
  const down = camera.dragged(orbit, 'orbit', 0, 10000);
  near([down.pitch], [-0.1], 6);
  // From the side (+x) the screen's right is +y: dragging right moves the focus to -y.
  const panned = camera.dragged(orbit, 'pan', 100, 0);
  assert.ok(panned.target[1] < 0);
  near([panned.target[0], panned.target[2]], [0, 0]);
  near([camera.dragged(orbit, 'zoom', 0, 200).distance], [6]);
  near([camera.dragged(orbit, 'zoom', 0, -200).distance], [1.5]);
});

test('the editor: numpad views and F go to the chosen thing; a flight forgets the orbit', () => {
  const poses = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {
      setPose: (place, forward) => poses.push({ place, forward }),
      getPosition: () => ({ x: 34, y: 34, z: 33 }),
    },
    Vector3: class {
      constructor(x, y, z) {
        Object.assign(this, { x, y, z });
      }
    },
    Key: { isDown: () => false },
    send: () => {},
    log: () => {},
  });
  editor.shell = { render: () => {}, viewPointer: () => null };
  editor.showView('front');
  near([poses.at(-1).place.x, poses.at(-1).place.y, poses.at(-1).place.z], [32, 29, 33.1]);
  // A free prop chosen: F frames it.
  editor.pressTile(0);
  editor.changeHold(1);
  editor.changeHold(1);
  assert.equal(editor.rope.hold.joint, '');
  editor.frameChosen();
  assert.deepEqual(editor.orbit.target, editor.rope.hold.at);
  // Nothing chosen: F is the front of the character again.
  editor.selected = 'character';
  editor.frameChosen();
  assert.equal(editor.orbit, null);
});
