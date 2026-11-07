#!/usr/bin/env node
'use strict';

// The one MCP server of World Architect and the Asset/Animation Studio: lets an AI assistant (Claude Code, Claude Desktop, any MCP client) use the editors in
// a running game on this PC. It speaks MCP over stdio (JSON-RPC 2.0, one message a line).
//
// - World Architect's commands (tools.cjs) go to the game client's AI bridge
//   (Client/runtime/architect/ai_bridge.hpp): 127.0.0.1, port KCDS_ARCHITECT_AI_PORT or 47870,
//   after the token the game wrote to %LOCALAPPDATA%\KCDS\architect\ai-token. The editor answers
//   each call (Client/resources/architect/ai_commands.cjs).
// - The Studio's tools and the editor_* tools (editor_steps.cjs) are steps of the open editor,
//   through the game client's script-step pipe (Client/runtime/architect/editor_pipe.hpp).
//
// No packages needed.

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const readline = require('node:readline');

const { TOOLS: ARCHITECT_TOOLS } = require('./tools.cjs');
const steps = require('./editor_steps.cjs');
const { makeCustomMap } = require('./custom_map.cjs');
const { makeTexture } = require('./textures.cjs');
const { importModel } = require('./models/import_model.cjs');
const { openModel } = require('./models/open_model.cjs');
const { INSTRUCTIONS } = require('./instructions.cjs');
const { measure, verdict } = require('./look/picture_review.cjs');
const { DrawnHits, nearer, OWN_MODEL } = require('./look/drawn_hits.cjs');
const { slotShares } = require('./look/model_mesh.cjs');

// Every tool: World Architect's commands, then the editors' steps.
const TOOLS = [...ARCHITECT_TOOLS, ...steps.TOOLS];
const STEP_TOOLS = new Set(steps.TOOLS.map((tool) => tool.name));
let stepLink = null;

// The script-step pipe of the open editor, found when a step tool is first called (the game may
// start after this server); it connects again by itself when the game closed it.
function editorLink() {
  if (!stepLink) {
    const pipe = steps.findPipe();
    stepLink = pipe ? new steps.EditorLink(pipe) : null;
  }
  return stepLink;
}

const PROTOCOL = '2025-06-18';
const ANSWER_MS = 20000;
const PORT = Number(process.env.KCDS_ARCHITECT_AI_PORT) || 47870;

function tokenPath() {
  const local =
    process.env.LOCALAPPDATA ?? path.join(process.env.USERPROFILE ?? '', 'AppData', 'Local');
  return path.join(local, 'KCDS', 'architect', 'ai-token');
}

// The connection to the game, made when a tool is first called and again after it closes.
class Game {
  constructor() {
    this.socket = null;
    this.ready = null;
    this.buffer = '';
    this.waiting = new Map();
    this.nextId = 1;
  }

  connect() {
    if (this.ready) {
      return this.ready;
    }
    this.ready = new Promise((resolve, reject) => {
      let token = '';
      try {
        token = fs.readFileSync(tokenPath(), 'utf8').trim();
      } catch {
        reject(
          new Error(
            'World Architect is not running: start the game in local mode first (no token file).',
          ),
        );
        return;
      }
      const socket = net.connect({ host: '127.0.0.1', port: PORT });
      let trusted = false;
      socket.setEncoding('utf8');
      socket.on('connect', () => socket.write(`${token}\n`));
      socket.on('data', (chunk) => {
        this.buffer += chunk;
        let end = this.buffer.indexOf('\n');
        while (end >= 0) {
          const line = this.buffer.slice(0, end);
          this.buffer = this.buffer.slice(end + 1);
          if (!trusted) {
            trusted = line === 'ok';
            if (trusted) {
              this.socket = socket;
              resolve();
            } else {
              reject(new Error(`the game refused the connection (${line})`));
              socket.destroy();
            }
          } else {
            this.answered(line);
          }
          end = this.buffer.indexOf('\n');
        }
      });
      socket.on('error', (error) => {
        if (!trusted) {
          reject(
            new Error(`no World Architect on 127.0.0.1:${PORT} (${error.code ?? error.message})`),
          );
        }
      });
      socket.on('close', () => {
        this.socket = null;
        this.ready = null;
        this.buffer = '';
        for (const { reject: fail, timer } of this.waiting.values()) {
          clearTimeout(timer);
          fail(new Error('the game closed the connection'));
        }
        this.waiting.clear();
      });
    }).catch((error) => {
      this.ready = null;
      throw error;
    });
    return this.ready;
  }

  answered(line) {
    let answer = null;
    try {
      answer = JSON.parse(line);
    } catch {
      return;
    }
    const waiting = this.waiting.get(answer.id);
    if (waiting) {
      clearTimeout(waiting.timer);
      this.waiting.delete(answer.id);
      waiting.resolve(answer);
    }
  }

  async call(command, args) {
    await this.connect();
    const id = this.nextId;
    this.nextId += 1;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(id);
        reject(new Error(`the game did not answer "${command}" in ${ANSWER_MS / 1000} s`));
      }, ANSWER_MS);
      this.waiting.set(id, { resolve, reject, timer });
      this.socket.write(`${JSON.stringify({ id, command, args })}\n`);
    });
  }
}

const game = new Game();

// The drawn faces of the author's own models, met here (look/drawn_hits.cjs).
const drawnHits = new DrawnHits(game);
const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

// A ray against them, or null when it is not a ray or none is met (a failure is no hit).
async function drawnRay(from, to) {
  if (!from || !to || !isNumber(from.x) || !isNumber(to.x)) {
    return null;
  }
  try {
    return await drawnHits.ray({ ...from, z: from.z ?? 0 }, { ...to, z: to.z ?? 0 });
  } catch {
    return null;
  }
}

// Each slot's share of one of the author's own models' faces, or null (a game model, unread).
function drawnShares(model) {
  if (typeof model !== 'string' || !OWN_MODEL.test(model) || !drawnHits.folder) {
    return null;
  }
  const mesh = drawnHits.meshOf(model);
  return mesh ? slotShares(mesh) : null;
}

// A command's arguments with the drawn hits the editor cannot find itself: place's snap ray
// (drawnHit) and review's ceiling (drawnCeiling), the same rays as ai_commands.cjs casts.
async function withDrawnHits(name, args) {
  const at = args.at;
  if (!at || !isNumber(at.x) || !isNumber(at.y)) {
    return args;
  }
  if (name === 'place' && args.snap !== undefined) {
    let from = null;
    let to = null;
    if (args.snap === 'floor') {
      const top = isNumber(at.z) ? at.z + 1 : 2000;
      from = { x: at.x, y: at.y, z: top };
      to = { x: at.x, y: at.y, z: top - 4000 };
    } else if (args.snap === 'ceiling' && isNumber(at.z)) {
      from = at;
      to = { ...at, z: at.z + SNAP_REACH };
    } else if (args.snap === 'wall' && isNumber(at.z) && args.toward) {
      const dx = args.toward.x - at.x;
      const dy = args.toward.y - at.y;
      const length = Math.hypot(dx, dy);
      if (length >= 0.1) {
        const reach = Math.max(length, SNAP_REACH);
        from = at;
        to = { x: at.x + (dx / length) * reach, y: at.y + (dy / length) * reach, z: at.z };
      }
    }
    const drawn = from ? await drawnRay(from, to) : null;
    return drawn ? { ...args, drawnHit: { position: drawn.position, normal: drawn.normal } } : args;
  }
  if (name === 'review') {
    // The floor as review finds it (the higher of the game's and the drawn one under the
    // middle), then the drawn ceiling over a player's eye there.
    const top = isNumber(at.z) ? at.z + 1 : 2000;
    const down = { x: at.x, y: at.y, z: top - 4000 };
    const physical = await game.call('raycast', { from: { x: at.x, y: at.y, z: top }, to: down });
    const drawnFloor = await drawnRay({ x: at.x, y: at.y, z: top }, down);
    const floors = [physical.ok ? physical.result?.position?.z : null, drawnFloor?.position.z];
    const floor = Math.max(...floors.filter(isNumber));
    if (!Number.isFinite(floor)) {
      return args;
    }
    const eye = { x: at.x, y: at.y, z: floor + EYE_HEIGHT };
    const ceiling = await drawnRay(eye, { ...eye, z: eye.z + CEILING_REACH });
    return ceiling ? { ...args, drawnCeiling: ceiling.position.z } : args;
  }
  return args;
}

// The same reaches as ai_commands.cjs: a snap's ray, a player's eye, how high a ceiling is
// looked for (metres).
const SNAP_REACH = 30;
const EYE_HEIGHT = 1.7;
const CEILING_REACH = 60;

// A picture's measured light and colour and what they mean (look/picture_review.cjs).
function pictureNotes(png, options) {
  try {
    const measured = measure(png);
    const { width, height, ...numbers } = measured;
    return [`Measured: ${JSON.stringify(numbers)}`, ...verdict(measured, options)].join('\n');
  } catch (error) {
    return `Not measured: ${error.message}`;
  }
}

// What review asks the assistant to look at in its pictures, beyond the numbers.
const REVIEW_CHECKLIST = [
  'Judge the views as a player standing there would, and fix before you call it done:',
  '- Does every thing stand on the floor, hang from the ceiling or sit against a wall? Nothing in the air, nothing through a wall.',
  '- Is the room lit like a room of the game: a room light (light preset room) for its ambient, then its lamps, candles and windows, warm and with shadows? Every lit-looking lamp needs a real light.',
  "- Are walls, floor and ceiling textured like the game's own rooms (stone, plaster, planks), with no black or stretched faces?",
  '- Are sizes right next to a person (door 2 m, table 0.75 m, bench 0.45 m, ceiling 3 m or more)?',
  '- Is the furniture laid out for use: paths to the doors free, things facing the way they are used, groups not grids?',
  '- Does it read as one place (a church, a tavern, a smithy) with enough detail, not an empty box with a few props?',
  'Then review again from the same at, and compare.',
].join('\n');

function reply(id, result) {
  process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', id, result })}\n`);
}

function fail(id, code, message) {
  process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', id, error: { code, message } })}\n`);
}

async function callTool(id, params) {
  if (STEP_TOOLS.has(params?.name)) {
    // A step of the open editor (editor_steps.cjs), with its own checks and answer.
    const answer = await steps.handle({ method: 'tools/call', params }, editorLink());
    if (answer.error) {
      fail(id, answer.error.code, answer.error.message);
    } else {
      reply(id, answer.result);
    }
    return;
  }
  const tool = TOOLS.find((entry) => entry.name === params?.name);
  if (!tool) {
    fail(id, -32602, `unknown tool ${params?.name}`);
    return;
  }
  try {
    if (tool.name === 'new_map') {
      // Made here, on this PC (custom_map.cjs), then listed by the game.
      const made = makeCustomMap(params.arguments ?? {});
      const listed = await game.call('map_added', { map: made.level });
      const note = listed.ok
        ? `the game lists it now: switch_map ${made.level} opens it`
        : 'the game lists it at its next start';
      const text = JSON.stringify({ result: made, message: note }, null, 1);
      reply(id, { content: [{ type: 'text', text }], isError: false });
      return;
    }
    if (tool.name === 'import_model') {
      // Made here, on this PC (models/), into the folder the game names, then loaded.
      const assets = await game.call('assets', {});
      if (!assets.ok) {
        throw new Error(assets.error ?? 'the editor is not open');
      }
      const made = importModel(params.arguments ?? {}, assets.result.folder);
      const loaded = await game.call('assets', {});
      const listed = loaded.ok && loaded.result.meshes.includes(made.mesh);
      const refused = loaded.ok ? loaded.result.refused : [];
      const note = listed
        ? `place each of meshes at the same spot and turn (one piece each); put a game door (catalogue "door") at each of doors`
        : `the game did not load it (refused: ${JSON.stringify(refused)})`;
      const text = JSON.stringify({ result: made, message: note }, null, 1);
      reply(id, { content: [{ type: 'text', text }], isError: !listed });
      return;
    }
    if (tool.name === 'open_model') {
      // A game building copied without the boxes that fill it (models/open_model.cjs), into the
      // folder the game names, then loaded.
      const assets = await game.call('assets', {});
      if (!assets.ok) {
        throw new Error(assets.error ?? 'the editor is not open');
      }
      if (!assets.result.folder) {
        throw new Error('the game named no assets folder (ask again in a moment)');
      }
      const made = openModel(params.arguments ?? {}, assets.result.folder);
      const loaded = await game.call('assets', {});
      const listed = loaded.ok && made.meshes.every((file) => loaded.result.meshes.includes(file));
      const note = listed
        ? "hide the level's building (hide_level_at) and place each of meshes at its spot and turn"
        : 'the game did not load the copy';
      const text = JSON.stringify({ result: made, message: note }, null, 1);
      reply(id, { content: [{ type: 'text', text }], isError: !listed });
      return;
    }
    if (tool.name === 'texture') {
      // Made here, on this PC (textures.cjs), into the folder the game names, then loaded.
      const assets = await game.call('assets', {});
      if (!assets.ok) {
        throw new Error(assets.error ?? 'the editor is not open');
      }
      const made = makeTexture(params.arguments ?? {}, assets.result.folder);
      const loaded = await game.call('assets', {});
      const listed = loaded.ok && loaded.result.materials.includes(made.material);
      const refused = loaded.ok ? loaded.result.refused : [];
      const note = listed
        ? `set_material with "${made.material}" draws a prop with it`
        : `the game did not load it (refused: ${JSON.stringify(refused)})`;
      const text = JSON.stringify({ result: made, message: note }, null, 1);
      reply(id, { content: [{ type: 'text', text }], isError: !listed });
      return;
    }
    const args = await withDrawnHits(tool.name, params.arguments ?? {});
    const answer = await game.call(tool.command ?? tool.name, args);
    if (answer.ok && tool.name === 'raycast') {
      // The nearer of the game's hit and one on the drawn faces of the author's own models.
      const drawn = await drawnRay(args.from, args.to);
      const from = { ...args.from, z: args.from?.z ?? 0 };
      if (drawn && nearer(from, answer.result, drawn) === drawn) {
        answer.result = {
          position: drawn.position,
          level: drawn.name,
          drawn: true,
          slot: drawn.slot,
        };
      }
    }
    if (answer.ok && tool.name === 'slots') {
      // How much of one of your own models each slot draws (an opened building's walls are
      // mostly one or two slots).
      if (!drawnHits.folder) {
        await drawnHits.placed().catch(() => []);
      }
      const shares = drawnShares(answer.result.model);
      if (shares) {
        const byIndex = new Map(shares.map((entry) => [entry.index, entry.share]));
        answer.result.slots = answer.result.slots.map((slot) => ({
          ...slot,
          share: byIndex.get(slot.index) ?? 0,
        }));
      }
    }
    if (answer.ok && typeof answer.result?.png === 'string') {
      // A picture: the image itself, then its size and how its light and colour measure.
      const { png, width, height } = answer.result;
      reply(id, {
        content: [
          { type: 'image', data: png, mimeType: 'image/png' },
          { type: 'text', text: `${width} x ${height}\n${pictureNotes(png)}` },
        ],
        isError: false,
      });
      return;
    }
    if (answer.ok && Array.isArray(answer.result?.pictures)) {
      // review: each view with its measures, then what the editor found in the area.
      const { pictures, ...found } = answer.result;
      const content = [];
      for (const picture of pictures) {
        content.push({ type: 'text', text: `View ${picture.label}:` });
        content.push({ type: 'image', data: picture.png, mimeType: 'image/png' });
        content.push({
          type: 'text',
          // Only an eye-level view's upper part is walls and ceiling (the view from above looks
          // down across the room).
          text: pictureNotes(picture.png, {
            indoors: found.indoors,
            walls: found.indoors && picture.eyeLevel !== false,
          }),
        });
      }
      content.push({ type: 'text', text: JSON.stringify(found, null, 1) });
      content.push({ type: 'text', text: REVIEW_CHECKLIST });
      reply(id, { content, isError: false });
      return;
    }
    const text = answer.ok
      ? JSON.stringify(
          { result: answer.result, message: answer.message || undefined, state: answer.state },
          null,
          1,
        )
      : `Error: ${answer.error}`;
    reply(id, { content: [{ type: 'text', text }], isError: !answer.ok });
  } catch (error) {
    reply(id, { content: [{ type: 'text', text: `Error: ${error.message}` }], isError: true });
  }
}

function handle(message) {
  const { id, method, params } = message;
  if (method === 'initialize') {
    reply(id, {
      protocolVersion: params?.protocolVersion ?? PROTOCOL,
      capabilities: { tools: {} },
      serverInfo: { name: 'kcds-world-architect', version: '0.1.0' },
      instructions: INSTRUCTIONS,
    });
    return;
  }
  if (method === 'tools/list') {
    reply(id, {
      tools: TOOLS.map(({ name, description, inputSchema }) => ({
        name,
        description,
        inputSchema,
      })),
    });
    return;
  }
  if (method === 'tools/call') {
    callTool(id, params);
    return;
  }
  if (method === 'ping') {
    reply(id, {});
    return;
  }
  if (id !== undefined && id !== null) {
    fail(id, -32601, `no method ${method}`);
  }
}

if (require.main === module) {
  const lines = readline.createInterface({ input: process.stdin });
  lines.on('line', (line) => {
    if (line.trim() === '') {
      return;
    }
    try {
      handle(JSON.parse(line));
    } catch {
      fail(null, -32700, 'parse error');
    }
  });
}

module.exports = { handle, Game, tokenPath };
