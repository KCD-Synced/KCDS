'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-client-roundtrip-'));
const folder = path.join(root, 'roundtrip');
fs.mkdirSync(folder);
fs.writeFileSync(
  path.join(folder, 'package.json'),
  JSON.stringify({
    name: 'roundtrip',
    kcds: {
      sharedScripts: ['shared.js', 'math-contract.cjs'],
      server: 'server-legacy.js',
      serverScripts: ['server.js'],
      client: 'client-legacy.js',
      clientScripts: ['client.js'],
      files: ['assets/*.{json,css}'],
      clientFiles: ['fonts/*.woff'],
    },
  }),
);
fs.mkdirSync(path.join(folder, 'assets'));
fs.mkdirSync(path.join(folder, 'fonts'));
fs.writeFileSync(path.join(folder, 'assets/data.json'), JSON.stringify({ marker: 'shipped' }));
fs.writeFileSync(path.join(folder, 'fonts/custom.woff'), 'fixture');
fs.writeFileSync(path.join(folder, 'shared.js'), `globalThis.manifestOrder = ['shared'];`);
for (const side of ['server', 'client']) {
  fs.writeFileSync(
    path.join(folder, side + '-legacy.js'),
    `
    if (globalThis.manifestOrder.join(',') !== 'shared') throw Error('shared order');
    globalThis.manifestOrder.push('legacy');
  `,
  );
}
fs.copyFileSync(
  path.resolve(__dirname, '../testkit/math_contract.cjs'),
  path.join(folder, 'math-contract.cjs'),
);
function writeVersion(version) {
  fs.writeFileSync(
    path.join(folder, 'client.js'),
    `
  if (globalThis.manifestOrder.join(',') !== 'shared,legacy') throw Error('client manifest order');
  if (require('./assets/data.json').marker !== 'shipped') throw Error('manifest asset missing');
  try { require('./server-legacy.js'); throw Error('server script leaked'); }
  catch (error) { if (error.message === 'server script leaked') throw error; }
  const mathChecks = require('./math-contract.cjs')({Vector2, Vector3, Vector4, Quaternion, Color});
  const version=${version};
  const token=Math.random().toString(36).slice(2);
  kcds.core.Events.on('answer', data => {
    if(typeof data !== 'object' || data === null) throw Error('server event payload not parsed');
    if(data.value===5 && data.version===version && data.token===token) kcds.core.Events.toServer('ack',{version});
  });
  kcds.core.Events.toServer('hello',{value:new kcds.math.Vector3(3,4,0).length,version,token,mathChecks});
  `,
  );
}
writeVersion(1);
fs.writeFileSync(
  path.join(folder, 'server.js'),
  `
if (globalThis.manifestOrder.join(',') !== 'shared,legacy') throw Error('server manifest order');
const mathChecks = require('./math-contract.cjs')({Vector2, Vector3, Vector4, Quaternion, Color});
console.log('MATH CONTRACT checks=' + mathChecks);
kcds.core.Events.fromClient('hello',(sender,data)=>{
  if(!(sender instanceof kcds.players.Player) || kcds.players.Player.getById(sender.id) !== sender) throw Error('client event sender is not a Player');
  if(data.value!==5 || data.mathChecks !== mathChecks || mathChecks < 100) throw Error('invalid client SDK math');
  kcds.core.Events.broadcast('answer',{value:data.value,version:data.version,token:data.token,from:sender.id});
});
kcds.core.Events.fromClient('ack',(sender,data)=>{
  kcds.core.Chat.sendToPlayer(sender,'resource-roundtrip-ok-'+data.version);
});
`,
);
if (!process.argv[2])
  throw Error('Usage: node test_client_resource_roundtrip.cjs <build-root> [port]');
const bin = path.resolve(process.argv[2]);
const port = process.argv[3] ?? '17783';
const repo = path.resolve(__dirname, '../..');
const server = spawn(
  bin + '/Backend/integration/Release/kcd2s-server.exe',
  [
    port,
    '127.0.0.1',
    'trosecko',
    process.execPath,
    repo + '/Backend/scripting/runtimes/js/main.cjs',
    root,
  ],
  { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] },
);
let probe;
let launched = false;
let reloaded = false;
let probeLog = '';
let log = '';
const timer = setTimeout(() => {
  console.error('Roundtrip timed out');
  probe?.kill();
  server.kill();
  process.exitCode = 1;
}, 30000);
server.stdout.on('data', (data) => {
  const text = data.toString();
  log += text;
  process.stdout.write('SERVER ' + text);
  if (!launched && log.includes('RESOURCE HOST ready')) {
    launched = true;
    probe = spawn(
      bin + '/Backend/integration/Release/kcd2s-client-resource-probe.exe',
      [port, bin + '/Client/runtime/Release/kcd2s-resource-runtime.exe', 'reload'],
      { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] },
    );
    probe.stdout.on('data', (data) => {
      const text = data.toString();
      process.stdout.write(text);
      probeLog += text;
      if (
        !reloaded &&
        probeLog.includes('A resource-roundtrip-ok-1') &&
        probeLog.includes('B resource-roundtrip-ok-1')
      ) {
        reloaded = true;
        writeVersion(2);
        server.stdin.write('ensure roundtrip\n');
      }
    });
    probe.stderr.pipe(process.stderr);
    probe.on('exit', (code) => {
      clearTimeout(timer);
      server.stdin.write('stop\n');
      console.log(JSON.stringify({ realClientRoundtrip: code === 0, exitCode: code }));
      process.exitCode = code === 0 ? 0 : 1;
    });
  }
});
server.stderr.pipe(process.stderr);
server.on('exit', (code) => {
  if (!launched) {
    clearTimeout(timer);
    process.exitCode = 1;
    console.error('Server did not become ready: ' + code);
  }
  fs.rmSync(root, { recursive: true, force: true });
});
