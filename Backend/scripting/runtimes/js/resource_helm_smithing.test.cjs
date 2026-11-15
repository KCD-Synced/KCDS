'use strict';

// The helm-smithing example resource (Dev/examples/resources/helm-smithing, not shipped with the
// server): its helmet workpiece and its kettle hat recipe register, and the recipe stands in the
// smithing book with the script's other recipes.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const RESOURCE = path.join(__dirname, '../../../../Dev/examples/resources/helm-smithing');

test('helm-smithing resource: the kettle hat recipe on its helmet workpiece', async (t) => {
  const f = serverFixture(t);
  fs.cpSync(RESOURCE, path.join(f.root, 'helm-smithing'), { recursive: true });
  await f.host.startAll();
  const lines = f.linesOf('helm-smithing');
  assert.ok(
    lines.some((line) => line.includes("'Kettle hat' at every smithy")),
    `registered: ${lines.join(' | ')}`,
  );
  // A joining player's game gets the workpiece with the helm model, and the recipe on it.
  f.report(7);
  const sent = JSON.stringify(f.sent);
  assert.match(sent, /semifinished_helm\.cgf/);
  assert.match(sent, /helm_kettle_hat/);
});
