'use strict';

const { BundleCache, DiskFile } = require('./bundle_cache.cjs');
const { BrowserResourceRuntime } = require('./runtime.cjs');
const { EntityTransfer, ResyncNeeded } = require('./entity_transfer.cjs');
const {
  setRuntimeLoader,
} = require('../../Backend/scripting/runtimes/js/languages/csharp_host.cjs');

// C# resources run on the .NET runtime the runtime program serves from its own folder
// (Client/resources/runtime_files.hpp); it loads when the first one starts.
const DOTNET_URL = 'https://runtime.kcds.invalid/dotnet/_framework/dotnet.js';
setRuntimeLoader(async () => {
  // In a worker, the .NET loader takes itself for one of its own threads' workers and never
  // starts, unless it is told it runs here on its own (its "sidecar" mode).
  self.dotnetSidecar = true;
  const { dotnet } = await import(DOTNET_URL);
  // The runtime's WebView2 profile is removed when it ends (runtime_profiles.hpp), so nothing is
  // kept in the browser's caches.
  return dotnet.withConfig({ startupMemoryCache: false, cacheBootResources: false }).create();
});

const send = (value) => self.postMessage(value);
const hash = async (bytes) => {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return [...digest].map((byte) => byte.toString(16).padStart(2, '0')).join('');
};
const decodeBytes = (text) => Uint8Array.from(atob(text), (character) => character.charCodeAt(0));
const encodeBytes = (bytes) => {
  let text = '';
  for (let offset = 0; offset < bytes.length; offset += 32768) {
    text += String.fromCharCode(...bytes.subarray(offset, offset + 32768));
  }
  return btoa(text);
};
// A kcds:// file goes to the game in parts: the runtime host drops any message over 64 kB, and a
// part's base64 must stay well under it.
const FILE_PART_BYTES = 32 * 1024;
// An AI answer's characters a message (a message holds 64 KiB; a screenshot's answer is far more).
const REPLY_PART_CHARS = 48 * 1024;
const pending = new Map();
let nextRequest = 1;
let generation = 0;
let chain = Promise.resolve();
let appliedCatalog = null;
// World Architect's test lines waiting, run one at a time (runTestLine).
const testLines = [];
const cache = new BundleCache(hash);
const runtime = new BrowserResourceRuntime({
  send,
  log: (resource, text) => send({ op: 'log', resource, text: String(text).slice(0, 4096) }),
});

const entityTransfer = new EntityTransfer((kind, value) => {
  if (kind === 'directory') {
    runtime.entities.updateDirectory(value);
    runtime.follow.check();
    const local = runtime.entities.localId;
    runtime.progression.receiveDirectory(local, runtime.entities.progressionOf(local));
  } else runtime.entities.applyState(value);
});

// The server's tree events (Trees) arrive as the reserved network event kcds:tree and
// reach client scripts as their own events; false for any other event.
// Runs the first waiting test line, then the next one on a later turn of the worker.
function runTestLine() {
  const { line, id } = testLines[0];
  let done = Promise.resolve();
  try {
    done = Promise.resolve(runtime.worldArchitect().script(line)).then((result) => {
      send({ op: 'log', resource: 'host', text: `ARCHITECT TEST ${result}` });
      // A step of the assistant's (architect/editor_pipe.hpp) gets its answer by its id.
      if (typeof id === 'number') {
        send({ op: 'editorReply', id, text: String(result) });
      }
    });
  } catch (error) {
    send({ op: 'log', resource: 'host', text: String(error) });
  }
  done
    .catch((error) => send({ op: 'log', resource: 'host', text: String(error) }))
    .finally(() => {
      testLines.shift();
      if (testLines.length > 0) {
        setTimeout(runTestLine, 0);
      }
    });
}

function treeEvent(name, payload) {
  if (name !== 'kcds:tree' || !payload || typeof payload !== 'object') {
    return false;
  }
  const tree = payload.tree;
  const failed = (error) => send({ op: 'log', resource: 'host', text: String(error) });
  if (payload.event === 'treeChop') {
    // A counted axe blow on a standing tree in this world: (tree, blow).
    const blow = payload.extra;
    runtime.services.emit('shared', 'treeChop', [tree, blow]).catch(failed);
  } else if (payload.event === 'treeFalling') {
    // A tree starts to fall: (tree, fall), fall.direction and fall.seconds.
    const fall = payload.extra;
    runtime.services.emit('shared', 'treeFalling', [tree, fall]).catch(failed);
  } else if (payload.event === 'treeFelled') {
    // A felled tree's trunk came to rest: (tree).
    runtime.services.emit('shared', 'treeFelled', [tree]).catch(failed);
  } else if (payload.event === 'trunkCut') {
    // A felled trunk was cut into items: (tree, items).
    const items = payload.extra;
    runtime.services.emit('shared', 'trunkCut', [tree, items]).catch(failed);
  } else if (payload.event === 'treeRegrown') {
    // A felled tree stands again: (tree).
    runtime.services.emit('shared', 'treeRegrown', [tree]).catch(failed);
  }
  return true;
}

function request(message, timeoutMs = 10000) {
  const requestId = nextRequest++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(requestId);
      reject(new Error('Resource download request timed out'));
    }, timeoutMs);
    pending.set(requestId, { resolve, reject, timer });
    send({ ...message, requestId });
  });
}

// How long the game may take to bring one big file into its download cache.
const ENSURE_TIMEOUT_MS = 15 * 60 * 1000;

// Download progress for the connecting screen, at most every 100 ms and always at the end.
const PROGRESS_INTERVAL_MS = 100;
function progressReporter(session) {
  let last = 0;
  return (progress) => {
    if (session !== generation) return;
    const now = Date.now();
    runtime.loadingScreen.download(progress);
    const finished = progress.bytesDone === progress.bytesTotal;
    if (!finished && now - last < PROGRESS_INTERVAL_MS) return;
    last = now;
    send({ op: 'resourceProgress', ...progress });
  };
}

function checkSession(session) {
  if (session !== generation) throw new Error('Resource session changed');
}

async function loadCatalog(info, session) {
  checkSession(session);
  if (appliedCatalog?.hash === info.hash && appliedCatalog.revision === info.revision) {
    send({ op: 'resourcesReady', ...appliedCatalog });
    return;
  }
  // The server's catalogBytes (bundles.cjs): the listing of up to 81920 files.
  if (!Number.isSafeInteger(info.size) || info.size < 1 || info.size > 20 * 1024 * 1024)
    throw new Error('Invalid resource catalog size');
  const bytes = new Uint8Array(info.size);
  for (let offset = 0; offset < bytes.length; offset += 16384) {
    const length = Math.min(16384, bytes.length - offset);
    const chunk = await request({ op: 'catalogChunk', revision: info.revision, offset, length });
    checkSession(session);
    const decoded = decodeBytes(chunk.data);
    if (
      chunk.revision !== info.revision ||
      chunk.offset !== offset ||
      chunk.size !== info.size ||
      decoded.length !== length
    )
      throw new Error('Invalid resource catalog chunk');
    bytes.set(decoded, offset);
  }
  if ((await hash(bytes)) !== info.catalogHash)
    throw new Error('Resource catalog download hash mismatch');
  const catalog = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
  if (catalog.revision !== info.revision || catalog.hash !== info.hash)
    throw new Error('Resource catalog identity mismatch');
  checkSession(session);
  await cache.acquire(
    catalog,
    async ({ revision, hash, offset, length }) => {
      checkSession(session);
      const chunk = await request({ op: 'fileRequest', revision, hash, offset, length });
      return { ...chunk, bytes: decodeBytes(chunk.data) };
    },
    progressReporter(session),
    (page) => {
      checkSession(session);
      try {
        runtime.showLoadingPage(page);
      } catch (error) {
        send({ op: 'log', resource: 'host', text: String(error) });
      }
    },
    async ({ hash }) => {
      // A big file comes whole over HTTP into the game's cache: up to 512 MiB.
      checkSession(session);
      try {
        return await request({ op: 'fileEnsure', hash }, ENSURE_TIMEOUT_MS);
      } catch {
        return { ok: false };
      }
    },
  );
  checkSession(session);
  runtime.loadingScreen.resourcesStarting();
  appliedCatalog = null;
  await runtime.apply(catalog, cache.reader());
  checkSession(session);
  appliedCatalog = { revision: catalog.revision, hash: catalog.hash };
  send({ op: 'resourcesReady', ...appliedCatalog });
}

function disconnect() {
  generation++;
  appliedCatalog = null;
  entityTransfer.reset(null);
  cache.disconnect();
  for (const entry of pending.values()) {
    clearTimeout(entry.timer);
    entry.reject(new Error('Resource session disconnected'));
  }
  pending.clear();
  chain = chain
    .catch(() => {})
    .then(() => runtime.disconnect())
    .then(() => send({ op: 'runtimeStopped' }));
}

self.onmessage = ({ data }) => {
  if (!data || typeof data !== 'object') return;
  if (Number.isSafeInteger(data.requestId) && pending.has(data.requestId)) {
    const entry = pending.get(data.requestId);
    pending.delete(data.requestId);
    clearTimeout(entry.timer);
    if (data.op === 'resourceError') entry.reject(new Error(data.error));
    else entry.resolve(data);
    return;
  }
  if (data.op === 'syncChannel') {
    runtime.sync.attach(data.buffer);
    return;
  }
  if (data.op === 'disconnect') {
    disconnect();
    return;
  }
  // World Architect's local mode only (Client/runtime/architect/local_mode.hpp): the game client
  // opens the editor itself; no server and no resource can send this.
  if (data.op === 'architectProject') {
    // The project the game client keeps for this map (architect/project_store.hpp).
    // A large project comes in parts (part, parts), joined by the editor.
    runtime
      .worldArchitect()
      .projectKept(String(data.map ?? ''), String(data.data ?? ''), data.part, data.parts);
    return;
  }
  if (data.op === 'architectCursorAt') {
    // The cursor and what it points at, every frame it moved (architect/cursor_pick.hpp).
    runtime.worldArchitect().cursorAt(data);
    return;
  }
  if (data.op === 'architectPicturesKept') {
    // The pictures the game client keeps on disk for the editor (editor_overlay.hpp).
    runtime.worldArchitect().picturesKept(Array.isArray(data.keys) ? data.keys : []);
    return;
  }
  if (data.op === 'editorWheel') {
    // The mouse wheel in World Architect's local mode (architect/local_mode.hpp): the open
    // editor's shell scrolls the panel under the cursor (a long Details list).
    const notches = Number(data.notches) || 0;
    // Only an editor already made (asking for one would make it).
    for (const editor of [runtime.studio, runtime.architect]) {
      if (editor?.isOpen?.()) {
        editor.shell?.wheel?.(notches);
      }
    }
    return;
  }
  if (data.op === 'architectCommand') {
    // An AI assistant's request (architect/ai_bridge.hpp): one line in, one line back.
    Promise.resolve()
      .then(() => runtime.worldArchitect().command(String(data.text ?? '')))
      .catch((error) => JSON.stringify({ id: null, ok: false, error: String(error) }))
      .then((text) => {
        // In order, in parts the game client joins into the one answer line (ai_bridge.cpp).
        const parts = Math.max(1, Math.ceil(text.length / REPLY_PART_CHARS));
        for (let part = 0; part < parts; part += 1) {
          const start = part * REPLY_PART_CHARS;
          send({
            op: 'architectReply',
            text: text.slice(start, start + REPLY_PART_CHARS),
            part,
            parts,
          });
        }
      });
    return;
  }
  if (data.op === 'architectOpen' || data.op === 'architectTest') {
    try {
      const editor = runtime.worldArchitect();
      if (data.op === 'architectOpen') {
        editor.open();
      } else {
        // A step for the editor (editor.cjs script()): the kit's test line, or the assistant's
        // through the game client's script-step pipe (architect/editor_pipe.hpp), which gets the
        // answer by its id. One at a time with a turn of the worker between them: hundreds sent
        // at once, each waiting a frame for the game, held the worker past its 10 s heartbeat
        // and it was ended (441 picks).
        testLines.push({ line: String(data.line ?? ''), id: data.id });
        if (testLines.length === 1) {
          setTimeout(runTestLine, 0);
        }
      }
    } catch (error) {
      send({ op: 'log', resource: 'host', text: String(error) });
    }
    return;
  }
  // The Asset/Animation Studio in the local mode only (Client/runtime/studio/studio_scene.hpp):
  // its own editor instead of World Architect's.
  if (data.op === 'studioJoints') {
    // The character's joints for the studio's Bones mode (studio/studio_skeleton.hpp).
    runtime
      .assetStudio()
      .jointsKept(
        Array.isArray(data.names) ? data.names : [],
        Array.isArray(data.parents) ? data.parents : [],
      );
    return;
  }
  if (data.op === 'studioJointInfo') {
    // The joints' geometry for the studio's assistant (studio/studio_skeleton.hpp).
    runtime.assetStudio().jointInfoKept({
      frame: String(data.frame ?? ''),
      place: Array.isArray(data.place) ? data.place : null,
      forward: Array.isArray(data.forward) ? data.forward : null,
      joints: Array.isArray(data.joints) ? data.joints : [],
    });
    return;
  }
  if (data.op === 'studioCVarValue') {
    // An engine console variable's value for the studio's assistant (studio/studio_floor.hpp).
    runtime
      .assetStudio()
      .cvarKept(String(data.name ?? ''), Number.isFinite(data.value) ? data.value : null);
    return;
  }
  if (data.op === 'studioMeshes') {
    // The meshes converted from Blender the game client found (studio/studio_mesh.hpp).
    runtime.assetStudio().meshesKept(Array.isArray(data.names) ? data.names : []);
    return;
  }
  if (data.op === 'studioGameClipState') {
    // Where the game's clip on the studio's character is (studio/studio_game_clip.hpp).
    runtime.assetStudio().gameClipStateKept({
      path: String(data.path ?? ''),
      time: Number.isFinite(data.time) ? data.time : -1,
      seconds: Number.isFinite(data.seconds) ? data.seconds : 0,
      from: Number.isFinite(data.from) ? data.from : 0,
      to: Number.isFinite(data.to) ? data.to : 1,
      loop: data.loop !== false,
      inPlace: data.inPlace !== false,
      topBody: data.topBody === true,
    });
    return;
  }
  if (data.op === 'studioGameClipBaked') {
    // A part of a bake of the game's clip (studio/studio_game_clip.hpp): the editor makes a
    // studio animation of it once every part came.
    runtime.assetStudio().gameClipBakedKept(data);
    return;
  }
  if (data.op === 'studioGameFilesPage') {
    // A page of the game's own objects or clips that match a search (studio/game_files.hpp).
    runtime.assetStudio().gameFilesKept({
      kind: String(data.kind ?? ''),
      query: String(data.query ?? ''),
      ready: data.ready === true,
      page: Number.isInteger(data.page) ? data.page : 0,
      size: Number.isInteger(data.size) ? data.size : 0,
      pages: Number.isInteger(data.pages) ? data.pages : 1,
      total: Number.isInteger(data.total) ? data.total : 0,
      paths: Array.isArray(data.paths) ? data.paths : [],
    });
    return;
  }
  if (data.op === 'studioJointPicked') {
    // The joint a press in the studio's view picked (studio/studio_skeleton.hpp); -1 for none.
    runtime
      .assetStudio()
      .jointPicked(
        Number.isInteger(data.id) ? data.id : -1,
        Number.isInteger(data.ring) ? data.ring : -1,
      );
    return;
  }
  if (data.op === 'studioSaves') {
    // The studio's saves on this PC (studio/studio_saves.hpp), newest first.
    runtime.assetStudio().savesKept(Array.isArray(data.saves) ? data.saves : []);
    return;
  }
  if (data.op === 'studioSaveLoaded') {
    runtime.assetStudio().saveLoaded(String(data.name ?? ''), String(data.data ?? ''));
    return;
  }
  if (data.op === 'studioTextures') {
    // Your textures the game client found (studio/studio_texture.hpp).
    runtime.assetStudio().texturesKept(Array.isArray(data.names) ? data.names : []);
    return;
  }
  if (data.op === 'studioTextured') {
    // The game client's answer to a texture put on the mesh.
    runtime.assetStudio().texturedKept({
      name: String(data.name ?? ''),
      ok: data.ok === true,
      width: Number(data.width) || 0,
      height: Number(data.height) || 0,
      error: String(data.error ?? ''),
    });
    return;
  }
  if (data.op === 'studioMeshWritten') {
    // A simple prop the prop menu made, written by the game client.
    runtime.assetStudio().meshWrittenKept({
      name: String(data.name ?? ''),
      ok: data.ok === true,
      error: String(data.error ?? ''),
    });
    return;
  }
  if (data.op === 'studioMeshBodied') {
    // The game client's check of a falling body (studio/studio_mesh.hpp).
    runtime.assetStudio().bodiedKept({ ok: data.ok === true, error: String(data.error ?? '') });
    return;
  }
  if (data.op === 'studioRopeRecorded') {
    // The rope's simulation recorded into keys (studio/studio_rope.hpp).
    runtime
      .assetStudio()
      .ropeRecorded(
        Number(data.rate) || 0,
        Array.isArray(data.frames) ? data.frames : [],
        Number.isInteger(data.part) ? data.part : 0,
        Number.isInteger(data.parts) ? data.parts : 1,
        Array.isArray(data.handles) ? data.handles : null,
        Array.isArray(data.body) && data.body.length === 6 ? data.body.map(Number) : null,
      );
    return;
  }
  if (data.op === 'studioPropPicked') {
    // The prop, mesh or gizmo handle a press in the studio's view took
    // (studio/studio_prop_gizmo.hpp), and where the thing is now.
    const numbers = (value) =>
      Array.isArray(value) && value.length === 3
        ? value.map((part) => Number(part) || 0)
        : [0, 0, 0];
    runtime
      .assetStudio()
      .propPicked(
        typeof data.thing === 'string' ? data.thing : '',
        Number.isInteger(data.axis) ? data.axis : -1,
        numbers(data.at),
        numbers(data.turn),
      );
    return;
  }
  if (data.op === 'studioProject') {
    // The studio's project the game client keeps (architect/project_store.hpp).
    runtime.assetStudio().projectKept(String(data.data ?? ''));
    return;
  }
  if (data.op === 'studioOpen' || data.op === 'studioTest') {
    try {
      const studio = runtime.assetStudio();
      if (data.op === 'studioOpen') {
        studio.open();
      } else {
        // A step for the studio (studio/editor.cjs script()): the kit's test line, or the
        // assistant's through the script-step pipe (architect/editor_pipe.hpp), which gets the
        // answer.
        Promise.resolve(studio.script(String(data.line ?? ''))).then((result) => {
          send({ op: 'log', resource: 'host', text: `STUDIO TEST ${result}` });
          if (typeof data.id === 'number') {
            send({ op: 'editorReply', id: data.id, text: String(result) });
          }
        });
      }
    } catch (error) {
      send({ op: 'log', resource: 'host', text: String(error) });
    }
    return;
  }
  const entityOps = ['entitySession', 'entityFrame', 'entityBegin', 'entityChunk', 'entityCommit'];
  if (entityOps.includes(data.op)) {
    const session = generation;
    chain = chain
      .then(() => {
        checkSession(session);
        if (data.op === 'entitySession') {
          if (data.session !== entityTransfer.session) {
            entityTransfer.reset(data.session);
            runtime.entities.disconnect();
            runtime.progression.reset();
            runtime.buffs.reset();
          }
        } else {
          try {
            entityTransfer.receive(data);
          } catch (error) {
            if (!(error instanceof ResyncNeeded)) throw error;
            // A directory delta against a directory this client does not hold: the sender
            // sends the whole directory instead.
            send({ op: 'entityAck', session: data.session, delivery: data.delivery, resync: true });
            return;
          }
        }
        if (
          data.session === entityTransfer.session &&
          Number.isSafeInteger(data.delivery) &&
          data.delivery > 0
        ) {
          send({ op: 'entityAck', session: data.session, delivery: data.delivery });
        }
      })
      .catch((error) => {
        send({ op: 'log', resource: 'host', text: String(error) });
      });
    return;
  }
  if (data.op === 'catalogInfo') {
    const session = generation;
    chain = chain
      .then(() => loadCatalog(data, session))
      .catch((error) => {
        send({ op: 'log', resource: 'host', text: String(error) });
      });
  } else if (data.op === 'event' && data.name === 'kcds:playerCount') {
    // The server's player count and places for the Discord presence ("5 of 32"); scripts never
    // see the event.
    try {
      const count = JSON.parse(data.payload);
      send({ op: 'discordPlayers', players: count.players, max: count.max });
    } catch (error) {
      send({ op: 'log', resource: 'host', text: `player count: ${error.message}` });
    }
  } else if (data.op === 'event' && data.name === 'kcds:tabletop') {
    // The server's tabletops: the tables of this dimension and this player's seat;
    // scripts never see the event.
    try {
      runtime.tabletop.receive(JSON.parse(data.payload));
    } catch (error) {
      send({ op: 'log', resource: 'host', text: `tabletop: ${error.message}` });
    }
  } else if (data.op === 'event' && data.name === 'kcds:poseAnimation') {
    // An animation defined by name on the server, longer than the player field takes (issue):
    // the game keeps it by its name (pose_animation/player_animations.hpp); scripts never
    // see the event.
    try {
      const defined = JSON.parse(data.payload);
      send({ op: 'poseAnimationDefine', name: String(defined.name), field: String(defined.field) });
    } catch (error) {
      send({ op: 'log', resource: 'host', text: `pose animation: ${error.message}` });
    }
  } else if (data.op === 'event' && data.name === 'kcds:overheadText') {
    // Player.showOverheadText on the server: a label above a player's tag, drawn by
    // this game; scripts never see the event.
    try {
      const line = JSON.parse(data.payload);
      runtime.nameplates.setLabel('kcds', line.entity, line.text, line.durationMs, line.color);
    } catch (error) {
      send({ op: 'log', resource: 'host', text: `overhead text: ${error.message}` });
    }
  } else if (data.op === 'event') {
    if (typeof data.name === 'string' && typeof data.payload === 'string') {
      // Server events reach the shared Events.on table with the payload parsed from JSON; a
      // payload that is not JSON text (an older server's plain string) arrives as that string.
      let payload;
      try {
        payload = JSON.parse(data.payload);
      } catch {
        payload = data.payload;
      }
      if (!treeEvent(data.name, payload)) {
        runtime.services.emit('shared', data.name, [payload]).catch((error) => {
          send({ op: 'log', resource: 'host', text: String(error) });
        });
      }
    }
  } else if (data.op === 'keyEdges') {
    runtime.input.receiveEdges(data);
    runtime.nativeUI.receiveEdges(data);
  } else if (data.op === 'keybindChoices') {
    runtime.keybinds.receiveChoices(data);
  } else if (data.op === 'keyLabels') {
    runtime.input.receiveLabels(data);
  } else if (data.op === 'inputHost') {
    runtime.input.receiveHost(data);
  } else if (data.op === 'cursorAt') {
    runtime.input.receiveCursor(data);
  } else if (data.op === 'hudMonologueSuppressed') {
    runtime.hud.receiveMonologueSuppressed(data);
  } else if (data.op === 'hudStoryCutsceneSuppressed') {
    runtime.hud.receiveStoryCutsceneSuppressed(data);
  } else if (data.op === 'hudHost') {
    runtime.hud.receiveHost(data);
  } else if (data.op === 'camera') {
    runtime.hud.receiveCamera(data);
  } else if (data.op === 'chatTyped') {
    runtime.chat.receiveTyped(data);
  } else if (data.op === 'chatMessage') {
    runtime.chat.receiveMessage(data);
  } else if (data.op === 'engineIds') {
    runtime.entities.updateEngineIds(data);
  } else if (data.op === 'npcBodies') {
    runtime.entities.updateNpcBodies(data);
    runtime.follow.check();
  } else if (data.op === 'bodyTags') {
    runtime.entities.updateBodyTags(data);
  } else if (data.op === 'localBuffs') {
    runtime.buffs.receiveLocal(data);
  } else if (data.op === 'localKnockout') {
    // The local player was knocked out or revived (LocalPlayer.knockedOut); only changes raise.
    const event = runtime.entities.receiveLocalKnockout(data);
    const raised =
      event === 'localPlayerKnockedOut'
        ? runtime.services.emit('shared', 'localPlayerKnockedOut', [])
        : event === 'localPlayerRevived'
        ? runtime.services.emit('shared', 'localPlayerRevived', [])
        : null;
    raised?.catch((error) => {
      send({ op: 'log', resource: 'host', text: String(error) });
    });
  } else if (data.op === 'progressionPerkRefused') {
    runtime.progression.receiveRefusal(data);
  } else if (data.op === 'questTrackingChanged') {
    // The player started or stopped following a server quest; the native client sends
    // only changes, before the server hears of it.
    if (typeof data.key === 'string' && typeof data.tracked === 'boolean') {
      runtime.services
        .emit('shared', 'questTrackingChanged', [data.key, data.tracked])
        .catch((error) => {
          send({ op: 'log', resource: 'host', text: String(error) });
        });
    }
  } else if (data.op === 'chatHost') {
    runtime.chat.receiveHost(data);
  } else if (data.op === 'chatKey') {
    runtime.chat.receiveKey();
  } else if (data.op === 'actionHintFired') {
    runtime.actionHints.receiveFired(data);
  } else if (data.op === 'actionHintsShown') {
    runtime.actionHints.receiveShown(data);
  } else if (data.op === 'lightsGone') {
    runtime.lights.receiveGone(data);
  } else if (data.op === 'worldMap') {
    runtime.worldMap.receiveState(data);
  } else if (data.op === 'worldMapEvent') {
    runtime.worldMap.receiveEvent(data);
  } else if (data.op === 'worldMapDiscovered') {
    runtime.worldMap.receiveDiscovered(data);
  } else if (data.op === 'weaponHit') {
    runtime.world.receiveWeaponHit(data);
  } else if (data.op === 'creatorStage') {
    runtime.characterCreator.receiveStage(data);
  } else if (data.op === 'voiceState') {
    runtime.localVoice.update(data);
  } else if (data.op === 'noclipState') {
    runtime.noclip.receiveState(data);
  } else if (data.op === 'noclipEnded') {
    runtime.noclip.receiveEnded(data);
  } else if (data.op === 'noclipAllowed') {
    runtime.noclip.receiveAllowed(data);
  } else if (data.op === 'cameraShotLocalEnded') {
    runtime.camera.receiveEnded(data);
  } else if (data.op === 'cameraPathLocalFinished') {
    runtime.camera.receivePathFinished();
  } else if (data.op === 'renderViewChunk') {
    runtime.render.receiveChunk(data);
  } else if (data.op === 'renderViewDone') {
    runtime.render.receiveDone(data);
  } else if (data.op === 'renderViewFailed') {
    runtime.render.receiveFailed(data);
  } else if (data.op === 'gizmosCleared') {
    runtime.gizmos.levelChanged();
    // The level that went took the client scripts' previews too.
    runtime.previews.levelChanged();
  } else if (data.op === 'localPlacerReport') {
    runtime.previews.receiveReport(data);
  } else if (data.op === 'localGhostBounds') {
    runtime.previews.receiveBounds(data);
  } else if (data.op === 'blipLive') {
    runtime.blips.receiveLive(data);
  } else if (data.op === 'mapIconWanted') {
    // A server blip's own image: its resource's file goes to the game as a map icon.
    try {
      runtime.nativeUI.shipTexture(data.resource, data.path, { mapIcon: true });
    } catch (error) {
      runtime.log('host', `Blip image ${data.resource}/${data.path}: ${error}`);
    }
  } else if (data.op === 'inventoryUse') {
    runtime.inventory.receiveUse(data);
  } else if (data.op === 'itemTypes') {
    runtime.inventory.receiveItemTypes(data);
  } else if (data.op === 'followEvent') {
    runtime.follow.receiveEvent(data);
  } else if (data.op === 'assetsMounted') {
    runtime.assets.receiveMounted(data);
  } else if (data.op === 'discordStatus') {
    runtime.discord.receiveStatus(data);
  } else if (data.op === 'tutorialHintGame') {
    runtime.tutorialHint.receiveGame(data);
  } else if (data.op === 'creatorKey') {
    runtime.characterCreator.receiveKey(data);
  } else if (data.op === 'bookHost') {
    runtime.book.receiveHost(data);
  } else if (data.op === 'bookEvent') {
    runtime.book.receiveEvent(data);
  } else if (data.op === 'vendorEvent') {
    runtime.vendorScreen.receiveEvent(data);
  } else if (data.op === 'nativeElementEvent') {
    runtime.nativeUI.receiveElementEvent(data);
  } else if (data.op === 'nativeMenuButton') {
    runtime.nativeUI.receiveMenuButton(data);
  } else if (data.op === 'nativeMenu') {
    runtime.nativeUI.receiveMenu(data);
  } else if (data.op === 'loadingProgress') {
    // The game's own percentage of the level's load (LoadingScreen).
    runtime.loadingScreen.progress(data.percent);
  } else if (data.op === 'webHost') {
    // The game's browser state: ready, and the frame size for Web.screenSize.
    runtime.web.updateHost(data);
  } else if (data.op === 'webEvent') {
    // browser* events and page sendToGame calls, as JSON from Client/ui/web.
    let message;
    try {
      message = JSON.parse(data.json);
    } catch {
      return;
    }
    runtime.web.receive(message);
  } else if (data.op === 'webFileRequest') {
    // A view loads kcds://resources/<resource>/<path>: only files the resource ships are served.
    const bytes = runtime.shippedFile(data.resource, data.path);
    if (bytes === null) {
      send({ op: 'webFile', fileRequest: data.fileRequest, found: false, data: '' });
      return;
    }
    if (bytes instanceof DiskFile) {
      // The game reads a big file from its download cache itself.
      send({ op: 'webFile', fileRequest: data.fileRequest, found: true, cached: bytes.hash });
      return;
    }
    // Every part but the last says more: true; the game joins them before it answers the page.
    let offset = 0;
    do {
      const end = Math.min(offset + FILE_PART_BYTES, bytes.length);
      send({
        op: 'webFile',
        fileRequest: data.fileRequest,
        found: true,
        more: end < bytes.length,
        data: encodeBytes(bytes.subarray(offset, end)),
      });
      offset = end;
    } while (offset < bytes.length);
  } else if (data.op === 'lobby') {
    // The server holds the player out of the world (Player.setLobby) or lets them in: a server's
    // loading page stays up while they are held.
    runtime.loadingScreen.setLobby(data.held === true);
  } else if (data.op === 'playerReady') {
    // The game took its loading screen down; the argument is the local player's handle.
    runtime.loadingScreen.ready();
    const player = runtime.entities.facade().LocalPlayer;
    runtime.services.emit('shared', 'playerReady', [player]).catch((error) => {
      send({ op: 'log', resource: 'host', text: String(error) });
    });
  }
};

setInterval(() => send({ op: 'heartbeat' }), 1000);
send({ op: 'runtimeReady' });
