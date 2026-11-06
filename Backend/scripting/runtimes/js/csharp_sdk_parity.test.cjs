'use strict';

// The generated C# SDK against the runtime: every static member C# calls,
// reads or writes (Sdk.Call / Sdk.Get / Sdk.Set with "domain", "Type", "member") is one a script
// has on the server or in the player's game. The generator's own check (build_csharp_sdk.py
// --check) keeps the C# files equal to the contract; this test keeps the contract's static
// paths true to the running SDK.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const GENERATED = path.resolve(__dirname, '../csharp/Kcds.Sdk/Generated');
const CALL = /Sdk\.(?:Call|Get|Set)(?:<[^>]*>)?\("([a-z]+)", "([A-Za-z]+)", "([A-Za-z_$]+)"/g;

// The `kcds` tree of each side, as a JavaScript script reaches it.
async function sideTrees(t) {
  const server = serverFixture(t);
  const folder = path.join(server.root, 'probe');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'probe', kcds: { server: 'main.js' } }),
  );
  fs.writeFileSync(path.join(folder, 'main.js'), '');
  await server.host.startAll();
  // The client keeps a resource's scope inside its Lua state, so the probe there is in Lua.
  const client = clientFixture(t);
  await client.runtime.apply(
    { resources: [client.resource('probe', { lua: '' }, {}, 'probe', 'lua')] },
    client.read,
  );
  return [
    server.host.resources.get('probe').vm.kcds,
    client.runtime.resources.get('probe').lua.scope.kcds,
  ];
}

function has(tree, domain, name, member) {
  const owner = tree?.[domain]?.[name];
  if (owner === undefined || owner === null) {
    return false;
  }
  let object = owner;
  while (object && object !== Object.prototype && object !== Function.prototype) {
    if (Object.getOwnPropertyNames(object).includes(member)) {
      return true;
    }
    object = Object.getPrototypeOf(object);
  }
  return false;
}

test('C# SDK: every static member it uses is on the server or the client', async (t) => {
  const trees = await sideTrees(t);
  const missing = [];
  let count = 0;
  for (const file of fs.readdirSync(GENERATED).filter((name) => name.endsWith('.cs'))) {
    const text = fs.readFileSync(path.join(GENERATED, file), 'utf8');
    for (const [, domain, name, member] of text.matchAll(CALL)) {
      count += 1;
      if (!trees.some((tree) => has(tree, domain, name, member))) {
        missing.push(`${file}: kcds.${domain}.${name}.${member}`);
      }
    }
  }
  assert.deepEqual(missing, []);
  assert.ok(count > 500, `only ${count} members found`);
});
