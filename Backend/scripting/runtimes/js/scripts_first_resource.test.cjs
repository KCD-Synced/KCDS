'use strict';

// The public guide KCDS-DOCS/first-resource.md, run as it is written: its code
// blocks are read from the page, so a change of the SDK that breaks the guide fails here. The
// server half is the production host process (main.cjs), the client half the browser
// BrowserResourceRuntime; a small relay stands in for the native transport.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  delay,
  entryContent,
  writeResource,
  clientFixture,
  hostProcess,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const { docsDir, SKIP } = require('./docs_dir.cjs');

const DOCS = docsDir();
if (!DOCS) {
  test('the first resource guide', { skip: SKIP }, () => {});
  return;
}
const GUIDE = path.join(DOCS, 'first-resource.md');

// The page's ```js (or ```lua) blocks in order: server, client, then the server's addition.
function guideBlocks(language = 'js') {
  const text = fs.readFileSync(GUIDE, 'utf8').replace(/\r\n/g, '\n');
  const blocks = [];
  const pattern = new RegExp('```' + language + '\\n([\\s\\S]*?)```', 'g');
  let match = pattern.exec(text);
  while (match) {
    blocks.push(match[1]);
    match = pattern.exec(text);
  }
  return blocks;
}

function guideManifest() {
  const text = fs.readFileSync(GUIDE, 'utf8').replace(/\r\n/g, '\n');
  return JSON.parse(/```json\n([\s\S]*?)```/.exec(text)[1]);
}

function chatTo(host, playerId) {
  const lines = [];
  for (const frame of host.frames) {
    if (frame.op === 'chat' && frame.target === playerId) {
      lines.push([frame.author, frame.text]);
    }
  }
  return lines;
}

// The guide in each language its tabs show: the Lua tab names the scripts .lua;
// the C# tab's code is the start of a script, built into server.dll and client.dll.
const LANGUAGES = [
  { fence: 'js', language: 'javascript', extension: 'js' },
  { fence: 'lua', language: 'lua', extension: 'lua' },
  { fence: 'cs', language: 'csharp', extension: 'dll' },
];

// A side's script file: the guide's code, or for C# the assembly of a Script that runs it.
function scriptFile(name, code, language) {
  if (language !== 'csharp') {
    return code;
  }
  const body = code.replace(/^(?=.)/gm, '        ');
  const script = `public class Main : Script\n{\n    public override void OnStart()\n    {\n${body}\n    }\n}\n`;
  return entryContent(name, { csharp: script }, 'csharp');
}

for (const { fence, language, extension } of LANGUAGES) {
  test(`first resource guide (${language}): the server greets, answers /hello and the F7 count`, async (t) => {
    const [server, client, addition] = guideBlocks(fence);
    const manifest = guideManifest();
    assert.equal(manifest.name, 'hello');
    assert.deepEqual(manifest.kcds.serverScripts, ['server.js']);
    manifest.kcds.serverScripts = [`server.${extension}`];
    manifest.kcds.clientScripts = [`client.${extension}`];

    const host = await hostProcess(t, (root) => {
      writeResource(root, 'hello', manifest, {
        [`server.${extension}`]: scriptFile('hello_server', server + '\n' + addition, language),
        [`client.${extension}`]: scriptFile('hello_client', client, language),
      });
    });
    await host.waitFor(
      (frame) => frame.op === 'log' && frame.resource === 'hello' && frame.text === 'started',
      'hello started',
    );
    // playerReady: in the world and playing, as the native server reports it.
    host.report(7, { nickname: 'Hans', gameplayReady: true });
    host.report(8, { nickname: 'Henry', gameplayReady: true });
    await host.waitFor((frame) => frame.op === 'chat' && frame.target === 7, 'the greeting');
    assert.deepEqual(chatTo(host, 7)[0], ['Server', 'Welcome, Hans!']);

    host.send({ op: 'chat', id: 7, text: '/hello Good morning' });
    host.send({ op: 'chat', id: 7, text: '/HELLO' });
    host.send({ op: 'chat', id: 7, text: '/other' });
    await host.waitFor(
      (frame) => frame.op === 'chat' && frame.target === 7 && frame.text === 'Hello!',
      'the answers',
    );
    assert.deepEqual(chatTo(host, 7), [
      ['Server', 'Welcome, Hans!'],
      ['Server', 'Good morning'],
      ['Server', 'Hello!'],
    ]);

    // The client's question, as Events.toServer sends it, answered to that player only.
    host.clientEvent(7, 'hello:count', 'null');
    await host.waitFor(
      (frame) => frame.op === 'event' && frame.name === 'hello:count',
      'the count',
    );
    const answer = host.frames.find(
      (frame) => frame.op === 'event' && frame.name === 'hello:count',
    );
    assert.equal(answer.player, 7);
    assert.deepEqual(JSON.parse(answer.payload), { players: 2 });
    assert.equal(await host.stop(), 0);
    assert.deepEqual(host.errors, []);
  });

  test(`first resource guide (${language}): the client notifies, sends on F7 and shows the answer`, async (t) => {
    const [, client] = guideBlocks(fence);
    const f = clientFixture(t);
    f.runtime.hud.receiveHost({ ready: true, dialogue: false });
    const code =
      language === 'csharp'
        ? `public class Main : Script\n{\n    public override void OnStart()\n    {\n${client}\n    }\n}\n`
        : client;
    const resource = f.resource('hello', { [language]: code }, {}, 'hello', language);
    await f.runtime.apply({ resources: [resource] }, f.read);
    await delay(20);
    await f.runtime.services.emit('shared', 'playerReady', [{ id: 7 }]);

    f.runtime.input.receiveEdges({ edges: [['f7', 'down']], down: ['f7'] });
    f.runtime.input.receiveEdges({ edges: [['f7', 'up']], down: [] });
    await delay(20);
    const asked = f.sent.filter((message) => message.op === 'event');
    assert.equal(asked.length, 1);
    assert.equal(asked[0].name, 'hello:count');

    await f.runtime.services.emit('shared', 'hello:count', [{ players: 2 }]);
    await delay(20);
    const shown = f.sent
      .filter((message) => message.op === 'hud' && message.call === 'showNotification')
      .map((message) => message.args[0]);
    assert.deepEqual(shown, ['The hello resource is running.', '2 players online']);
  });
}
