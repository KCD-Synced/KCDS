'use strict';

// instanceof across isolated server resource contexts
// (Backend/scripting/runtimes/js/realms.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  clientFixture,
  delay,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

test('values the host and other resources give a resource pass its own instanceof', async (t) => {
  const f = serverFixture(t);
  serverResource(
    f.root,
    'sender',
    `kcds.core.Events.on('realm:ask', () => {
      kcds.core.Events.emit('realm:values', [1, 2], { kind: 'plain' }, new RangeError('from sender'), new Map());
    });`,
  );
  serverResource(
    f.root,
    'checker',
    `
    const results = {};
    kcds.core.Events.on('realm:values', (list, plain, error, map) => {
      results.foreignArray = list instanceof Array;
      results.foreignObject = plain instanceof Object;
      results.foreignError = error instanceof RangeError && error instanceof Error;
      results.foreignMap = map instanceof Map;
    });
    kcds.core.Events.on('realm:fail', () => {
      throw new Error('fails');
    });
    kcds.core.Events.on('realm:run', async () => {
      const pending = kcds.core.Events.emit('realm:fail');
      results.promise = pending instanceof Promise;
      try {
        await pending;
      } catch (error) {
        results.aggregate = error instanceof AggregateError && error instanceof Error;
        results.errors = error.errors instanceof Array;
      }
      try {
        kcds.core.Chat.sendToAll(5);
      } catch (error) {
        results.sdkError = error instanceof TypeError && error instanceof Error;
      }
      results.vector = new kcds.math.Vector3(1, 2, 3) instanceof kcds.math.Vector3;
      results.subclass = new (class extends Error {})() instanceof RangeError;
      results.primitive = 'text' instanceof Object;
      await kcds.core.Events.emit('realm:ask');
      console.log(JSON.stringify(results));
    });
    `,
    { requires: ['sender'] },
  );
  await f.host.startAll();
  await f.host.services.emit('shared', 'realm:run', []);
  const results = JSON.parse(f.linesOf('checker').find((line) => line.startsWith('{')));
  assert.deepEqual(results, {
    promise: true,
    aggregate: true,
    errors: true,
    sdkError: true,
    vector: true,
    subclass: false,
    primitive: false,
    foreignArray: true,
    foreignObject: true,
    foreignError: true,
    foreignMap: true,
  });
});

test('a stopped resource is unlinked from the other contexts', async (t) => {
  const f = serverFixture(t);
  serverResource(f.root, 'short', '');
  await f.host.startAll();
  const before = f.host.realmLinks.realms.size;
  await f.host.stop('short');
  assert.equal(f.host.realmLinks.realms.size, before - 1);
});

test('client resources see host values pass instanceof with the production evaluator', async (t) => {
  const f = clientFixture(t);
  await f.runtime.apply(
    {
      resources: [
        f.resource(
          'checker',
          `
          kcds.core.Events.on('realm:fail', () => {
            throw new Error('fails');
          });
          setTimeout(async () => {
            const results = {};
            const pending = kcds.core.Events.emit('realm:fail');
            results.promise = pending instanceof Promise;
            try {
              await pending;
            } catch (error) {
              results.aggregate = error instanceof AggregateError;
            }
            try {
              kcds.core.Events.on(5, () => {});
            } catch (error) {
              results.sdkError = error instanceof TypeError;
            }
            results.vector = new kcds.math.Vector3(1, 2, 3) instanceof kcds.math.Vector3;
            console.log(JSON.stringify(results));
          }, 0);
          `,
        ),
      ],
    },
    f.read,
  );
  await delay(100);
  const line = f.linesOf('checker').find((entry) => entry.startsWith('{'));
  assert.deepEqual(JSON.parse(line), {
    promise: true,
    aggregate: true,
    sdkError: true,
    vector: true,
  });
});
