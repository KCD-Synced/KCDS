'use strict';

const {
  ResourceServices,
  describeError,
} = require('../../Backend/scripting/runtimes/js/services.cjs');
const { withNamespace } = require('../../Backend/scripting/runtimes/js/sdk_namespace.cjs');
const { LuaResource } = require('../../Backend/scripting/runtimes/js/languages/lua_host.cjs');
const { CSharpResource } = require('../../Backend/scripting/runtimes/js/languages/csharp_host.cjs');
const {
  Vector2,
  Vector3,
  Vector4,
  Quaternion,
  Color,
} = require('../../Backend/scripting/runtimes/js/math.cjs');
const { validPath } = require('./bundle_cache.cjs');
const { ClientEntities } = require('./entities.cjs');
const {
  progressionCatalog,
} = require('../../Backend/scripting/runtimes/js/progression/catalog.cjs');
const { ResourceWebViews } = require('./web.cjs');
const { LoadingScreen } = require('./loading_screen.cjs');
const { ResourceInput } = require('./input.cjs');
const { ResourceKeybinds } = require('./keybinds.cjs');
const { ResourceHud, HUD_ELEMENTS } = require('./hud.cjs');
const { ResourceChat } = require('./chat.cjs');
const { ResourceActionHints } = require('./action_hints.cjs');
const { ResourceActionMenus } = require('./action_menu.cjs');
const { ResourceWorldMap, compassFacade, travelFacade } = require('./world_map.cjs');
const { ResourceCharacterCreator } = require('./character_creator.cjs');
const { createCreatorPanel } = require('./creator_panel.cjs');
const { ResourceTutorialHint } = require('./tutorial_hint.cjs');
const { ResourceNoClip } = require('./noclip.cjs');
const { ResourceLocalVoice } = require('./local_voice.cjs');
const { ResourceCamera } = require('./camera.cjs');
const { ResourceVfx } = require('./vfx.cjs');
const { ResourceLights } = require('./lights.cjs');
const { ResourcePrefabs } = require('./prefabs.cjs');
const { ResourceCollision } = require('./collision.cjs');
const { ResourceTerrain } = require('./terrain.cjs');
const { ResourceVegetation } = require('./vegetation.cjs');
const { ArchitectEditor, architectFacade } = require('./architect/editor.cjs');
const { StudioEditor } = require('./studio/editor.cjs');
const {
  prefabNames,
  resolvePrefab,
  resolveMesh,
  meshPaths,
} = require('../../Backend/scripting/runtimes/js/props/catalog.cjs');
const { ResourceRender } = require('./render.cjs');
const { ResourceWorld } = require('./world.cjs');
const { meshPathOf } = require('../../Backend/scripting/runtimes/js/world/mesh_categories.cjs');
const { ResourcePreviews } = require('./previews.cjs');
const { ResourceLocalNpcs } = require('./local_npcs.cjs');
const { ResourceGizmos } = require('./gizmos.cjs');
const { ResourceNameplates } = require('./nameplates.cjs');
const { ResourceDiscord } = require('./discord.cjs');
const { ResourceGame } = require('./game.cjs');
const { ResourceAudio } = require('./audio.cjs');
const { ResourceAssets } = require('./assets.cjs');
const { ResourceFollow } = require('./follow.cjs');
const { ResourceTabletop } = require('./tabletop.cjs');
const { EmoteWheelState } = require('./emotes.cjs');
const { ResourceBlips } = require('./blips.cjs');
const { ResourceInventory } = require('./inventory.cjs');
const { ResourceBook } = require('./book.cjs');
const { ResourceVendorScreen } = require('./vendor_screen.cjs');
const { SyncChannel } = require('./sync_channel.cjs');
const { ResourceNativeUI } = require('./native_ui.cjs');
const { ResourceProgression } = require('./progression.cjs');
const { ResourceBuffs } = require('./buffs.cjs');

function resolveModule(request, parent) {
  if (typeof request !== 'string' || !request.startsWith('.'))
    throw new Error('Client resources support relative CommonJS modules only');
  const parts = parent.split('/').slice(0, -1);
  for (const part of request.split('/')) {
    if (part === '.' || part === '') continue;
    if (part === '..') {
      if (!parts.length) throw new Error('Client module escapes resource');
      parts.pop();
    } else parts.push(part);
  }
  const name = parts.join('/');
  if (!validPath(name)) throw new Error('Invalid client module path');
  return [name, name + '.js', name + '.cjs', name + '.json', name + '/index.js'];
}

class BrowserResourceRuntime {
  constructor({
    send = () => {},
    log = () => {},
    nativeAPI = () => ({}),
    evaluate = (code, globals, module, localRequire, file) => {
      // A live lexical scope preserves getters (for example a player handle
      // becoming available after startup). CommonJS locals stay local to the
      // module; the resource's globalThis points to its own SDK facade.
      const factory = new Function(
        'scope',
        `with(scope) { return function(exports,require,module,__filename,__dirname) {
          ${code}
        }; }
//# sourceURL=kcds-resource/${file}`,
      );
      const wrapper = factory(globals);
      return wrapper.call(
        module.exports,
        module.exports,
        localRequire,
        module,
        file,
        file.split('/').slice(0, -1).join('/'),
      );
    },
  } = {}) {
    this.send = send;
    this.log = log;
    this.nativeAPI = nativeAPI;
    this.evaluate = evaluate;
    this.resources = new Map();
    this.definitions = new Map();
    this.services = new ResourceServices({
      side: 'client',
      send,
      log,
      eventArgs: (name, args, owner) =>
        name === 'entityStateChange'
          ? [
              new (this.entities.facade(this.resources.get(owner)).Entity)(args[0].id),
              ...args.slice(1),
            ]
          : args,
    });
    this.buffs = new ResourceBuffs();
    this.entities = new ClientEntities({
      localBuffs: () => this.buffs.local(),
      send,
      pose: (entity, joint) => this.sync.call({ op: 'localWorldPose', entity, joint }),
      ownerAlive: (owner) => !owner || (owner.active && this.resources.get(owner.name) === owner),
      onChange: (id, key, value, previous) =>
        this.services.emitSync('entityStateChange', [
          new (this.entities.facade().Entity)(id),
          key,
          value,
          previous,
        ]),
      onError: (error, owner) => this.log(owner?.name ?? 'host', String(error)),
      // The server stays authoritative: assigning the local player's position or rotation asks
      // the server, which validates it and moves the player like a server-side assignment.
      force: ({ id, field, value }) => {
        if (id !== this.entities.localId)
          throw new Error('A client script can only move the local player');
        const request = { op: 'entityTransform', id, field, x: value.x, y: value.y, z: value.z };
        if (field === 'rotation') request.w = value.w;
        this.send(request);
      },
    });
    this.decoder = new TextDecoder('utf-8', { fatal: true });
    this.web = new ResourceWebViews({
      send,
      log,
      emit: (owner, name, args) => this.services.emit('shared', name, args, owner),
      raise: (name, args) => this.services.emit('shared', name, args),
    });
    this.loadingScreen = new LoadingScreen({
      web: this.web,
      send,
      emit: (stage, percent) => {
        this.services.emit('shared', 'loadingProgress', [stage, percent]).catch((error) => {
          log('host', String(error));
        });
      },
    });
    this.input = new ResourceInput({
      send,
      log,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    // Resource actions in the game's key binding settings.
    this.keybinds = new ResourceKeybinds({ send, input: this.input, log });
    this.hud = new ResourceHud({
      send,
      screenSize: () => this.web.screen,
      ask: (request) => this.sync.call(request) ?? null,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.actionHints = new ResourceActionHints({
      send,
      log,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.actionMenus = new ResourceActionMenus({ actionHints: this.actionHints, send });
    this.sync = new SyncChannel(send);
    this.worldMap = new ResourceWorldMap({
      send,
      sync: this.sync,
      Vector3,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.nativeUI = new ResourceNativeUI({
      sync: this.sync,
      send,
      log,
      shippedFile: (name, file) => this.shippedFile(name, file),
      screenSize: () => this.web.screen,
      input: this.input,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.book = new ResourceBook({
      send,
      shipImage: (owner, path) => this.nativeUI.shipTexture(owner, path),
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    // vendorOpened and vendorClosed of a server vendor's trade screen.
    this.vendorScreen = new ResourceVendorScreen({
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.characterCreator = new ResourceCharacterCreator({
      send,
      log,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
      ownLook: () => this.entities.records.get(this.entities.localId)?.appearance ?? null,
      createPanel: (creator, keys) =>
        createCreatorPanel(this.nativeUI.facade('@creator').NativeUI, creator, keys),
    });
    this.tutorialHint = new ResourceTutorialHint({
      hud: () => this.nativeUI.facade('@hint').NativeUI.element('HUD'),
      log,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.inventory = new ResourceInventory({
      send,
      shipIcon: (resource, path) => this.nativeUI.shipTexture(resource, path, { itemIcon: true }),
    });
    this.localVoice = new ResourceLocalVoice({
      send,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.noclip = new ResourceNoClip({
      send,
      Vector3,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.camera = new ResourceCamera({
      send,
      sync: this.sync,
      screenSize: () => this.web.screen,
      Vector3,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    // Particle effects this game's scripts play.
    this.vfx = new ResourceVfx({ send });
    // Point lights a client script shows in this game only (Light).
    this.lights = new ResourceLights({ send });
    // The game's prefabs a client script builds in this game only (Prefab, World Architect).
    this.prefabs = new ResourcePrefabs({ send });
    // Invisible solid boxes a client script adds in this game only (Collision, World Architect).
    this.collision = new ResourceCollision({ send });
    // The ground a client script shapes in this game only (Terrain, World Architect).
    this.terrain = new ResourceTerrain({ send });
    // Vegetation a client script plants in this game only (Vegetation, World Architect).
    this.vegetation = new ResourceVegetation({ sync: this.sync });
    // Pictures of the world from any camera (Render.view).
    this.render = new ResourceRender({ send });
    // World's rays, ground and spheres on this machine's own game.
    this.world = new ResourceWorld({
      sync: this.sync,
      Vector3,
      Quaternion,
      playerOf: (engineId, owner) => this.playerOfEngine(engineId, owner),
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.localNpcs = new ResourceLocalNpcs({ send, Vector3 });
    this.gizmos = new ResourceGizmos({ send });
    // PropGhost and PropPlacer of client scripts.
    this.previews = new ResourcePreviews({ send, Vector3, Quaternion, log });
    this.nameplates = new ResourceNameplates({ send });
    this.discord = new ResourceDiscord({ send });
    this.game = new ResourceGame({ send });
    this.audio = new ResourceAudio({ sync: this.sync });
    this.emoteWheel = new EmoteWheelState();
    // Tabletop minigames: the sit hints and the pointing at a board.
    this.tabletop = new ResourceTabletop({
      send,
      camera: this.camera,
      gizmos: this.gizmos,
      input: this.input,
      actionHints: this.actionHints,
      entities: this.entities,
    });
    this.follow = new ResourceFollow({
      entities: this.entities,
      sync: this.sync,
      Vector3,
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.assets = new ResourceAssets({
      send,
      sendFile: (resource, file, bytes) => this.nativeUI.sendFile(resource, file, bytes),
      log,
    });
    this.blips = new ResourceBlips({
      send,
      Vector3,
      shipImage: (resource, path) => this.nativeUI.shipTexture(resource, path, { mapIcon: true }),
    });
    this.progression = new ResourceProgression({
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.chat = new ResourceChat({
      send,
      emitSync: (name, args) => this.services.emitSync(name, args),
      emit: (name, args) =>
        this.services.emit('shared', name, args).catch((error) => log('host', String(error))),
    });
    this.read = null;
  }

  // The Player handle of the body with this engine id on this machine (a World hit's player), or
  // null for anything else.
  playerOfEngine(engineId, owner) {
    for (const [id, engine] of this.entities.engineIds) {
      if (engine === engineId && this.entities.records.get(id)?.kind === 'player') {
        return new (this.entities.facade(owner).Player)(id);
      }
    }
    return null;
  }

  // The animation clips the running resources ship (their .caf client files) go to the
  // game client, once per content, as NativeUI's files do: a resource's playAnimation({ clip })
  // plays one by its path on the owner's body and on every replica, so every client has the file
  // before an order or a replica names it.
  shipClips() {
    for (const [name, definition] of this.definitions) {
      if (!this.resources.has(name)) {
        continue;
      }
      // Its streamed game files too.
      this.assets.ship(
        name,
        definition.files.map((entry) => entry.name),
        (file) => this.shippedFile(name, file),
      );
      // The custom maps it brings (World Architect).
      this.assets.shipMaps(
        name,
        definition.files.map((entry) => entry.name),
        (file) => this.shippedFile(name, file),
      );
      for (const entry of definition.files) {
        if (!/\.caf$/i.test(entry.name)) {
          continue;
        }
        const bytes = this.shippedFile(name, entry.name);
        if (!bytes) {
          continue;
        }
        try {
          this.nativeUI.sendFile(name, entry.name, bytes);
        } catch (error) {
          this.log(name, String(error));
        }
      }
    }
  }

  // World Architect's editor, one for this game, made on first use with its own
  // facades of the pieces it drives.
  worldArchitect() {
    if (!this.architect) {
      const owner = '@architect';
      this.architect = new ArchitectEditor({
        NativeUI: this.nativeUI.facade(owner).NativeUI,
        NoClip: this.noclip.facade(),
        World: this.world.facade(owner),
        Prefab: this.prefabs.facade(owner),
        Collision: this.collision.facade(owner),
        Gizmos: this.gizmos.facade(owner),
        Terrain: this.terrain.facade(owner),
        Vegetation: this.vegetation.facade(owner),
        Vfx: this.vfx.facade(owner),
        Light: this.lights.facade(owner),
        Key: this.input.keyFacade(owner),
        Controls: this.input.controlsFacade(owner),
        PropGhost: this.previews.facade(owner).PropGhost,
        // A game prefab's brushes for its preview (prefabs/local_prefabs.hpp), and whether the
        // prop catalog carries a mesh (a preview shows only those).
        prefabPieces: (name) => {
          const found = resolvePrefab(name);
          if (!found || found.blocked) {
            return null;
          }
          return this.sync.call({ op: 'localPrefabPieces', template: found.guid }, 2000);
        },
        meshKnown: (path) => resolveMesh(path) !== null,
        // A hit as the game client sends it, read as World.raycast reads it.
        hitOf: (raw) => this.world.hitOf(raw, owner),
        // World Architect's saves on this PC (architect/saves.hpp).
        savesList: () => this.sync.call({ op: 'localArchitectSaves' }, 2000),
        // The custom maps on this PC the game lists (maps/custom_maps.hpp), by name.
        customMaps: () => this.sync.call({ op: 'localArchitectMaps' }, 2000),
        // The ground's height under many points in one question (architect/level_plants.hpp).
        groundHeights: (points, reach) =>
          this.sync.call({ op: 'localGroundHeights', points, reach }, 2000),
        // A save's whole text, read in pieces (a message holds 64 KiB); null for none.
        saveRead: (name) => {
          let text = '';
          let offset = 0;
          while (offset >= 0) {
            const piece = this.sync.call({ op: 'localArchitectSaveRead', name, offset }, 2000);
            if (!piece || typeof piece.text !== 'string') {
              return null;
            }
            text += piece.text;
            offset = piece.next;
          }
          return text;
        },
        // The AI bridge's port, state, MCP server path and token (architect/ai_bridge.hpp).
        aiBridge: () => this.sync.call({ op: 'localArchitectAi' }, 2000),
        // The level's own plants within a radius, for the nature brush
        // (architect/level_plants.hpp): [{ mesh, at }], the mesh in the form a hit's model has.
        // The level's loose brushes within a radius (architect/level_plants.hpp): [{ mesh, at,
        // turn, scale, hidden }], the turn in degrees about z; with all, buildings and walls too;
        // with hidden, also those their layer hides (the level's other story states).
        levelBrushes: (center, radius, most = 256, all = false, hidden = false) => {
          const reply = this.sync.call(
            {
              op: 'localLevelBrushes',
              center: { x: center.x, y: center.y, z: center.z },
              radius,
              most,
              all: all === true,
              hidden: hidden === true,
            },
            2000,
          );
          if (!Array.isArray(reply)) {
            return [];
          }
          return reply
            .filter((brush) => typeof brush?.mesh === 'string' && Array.isArray(brush.p))
            .map((brush) => ({
              mesh: brush.mesh,
              at: { x: brush.p[0], y: brush.p[1], z: brush.p[2] },
              turn: typeof brush.turn === 'number' ? brush.turn : 0,
              scale: typeof brush.scale === 'number' ? brush.scale : 1,
              hidden: brush.hidden === true,
            }));
        },
        levelPlants: (center, radius, most) => {
          const reply = this.sync.call(
            {
              op: 'localLevelPlants',
              center: { x: center.x, y: center.y, z: center.z },
              radius,
              most,
            },
            2000,
          );
          if (!Array.isArray(reply)) {
            return [];
          }
          return reply
            .filter((plant) => Array.isArray(plant?.p) && plant.p.length === 3)
            .map((plant) => ({
              mesh: meshPathOf(String(plant.mesh ?? '')) ?? String(plant.mesh ?? ''),
              at: { x: plant.p[0], y: plant.p[1], z: plant.p[2] },
            }));
        },
        // The kinds of the level's grass carpet around a point, the most common first
        // (architect/level_plants.hpp): [{ kind, mesh, count }].
        grassKinds: (center, radius) => {
          const reply = this.sync.call(
            { op: 'localGrassKinds', center: { x: center.x, y: center.y, z: center.z }, radius },
            2000,
          );
          return Array.isArray(reply) ? reply : [];
        },
        // Many plants, or many taken away, in one question each (vegetation.cjs).
        plantMany: (list) => this.vegetation.plantMany(owner, list),
        removeMany: (handles) => this.vegetation.removeMany(handles),
        Vector3,
        prefabNames,
        // The game's prop meshes for World Architect's Props (props/catalog.cjs), and a placed
        // prop's box in its own frame (props/local_props.hpp): { min, max } or null.
        propMeshes: meshPaths,
        propBounds: (mesh) => this.sync.call({ op: 'localPropBounds', mesh }, 2000),
        // The author's own assets folder, mounted (architect/user_assets.hpp).
        userAssets: () => this.sync.call({ op: 'localUserAssets' }, 10000),
        // A model's material and its slots, and a material's text, of the author's or the
        // game's (architect/asset_files.hpp): { material, slots } or null; text or null.
        modelSlots: (mesh) => this.sync.call({ op: 'localModelSlots', mesh }, 5000),
        assetText: (path) => this.sync.call({ op: 'localAssetText', path }, 5000),
        // A model's (.cgf) bytes, read a part a question: a Uint8Array, or null.
        assetBytes: (path) => {
          const parts = [];
          let size = Infinity;
          let read = 0;
          while (read < size) {
            const part = this.sync.call({ op: 'localAssetBytes', path, offset: read }, 5000);
            if (!part || typeof part.data !== 'string' || typeof part.size !== 'number') {
              return null;
            }
            const text = atob(part.data);
            if (text.length === 0 && read < part.size) {
              return null;
            }
            const bytes = new Uint8Array(text.length);
            for (let at = 0; at < text.length; at += 1) {
              bytes[at] = text.charCodeAt(at);
            }
            parts.push(bytes);
            size = part.size;
            read += bytes.length;
          }
          const whole = new Uint8Array(read);
          let at = 0;
          for (const part of parts) {
            whole.set(part, at);
            at += part.length;
          }
          return whole;
        },
        // A building's streamed meshes (.cgfm) split into models of yours at <to>_<n>.cgf drawn
        // with material: { meshes } or null.
        splitModel: (from, to, material) =>
          this.sync.call({ op: 'architectSplitModel', from, to, material }, 30000),
        // A level entity's Properties (architect/level_properties.hpp).
        entityProperties: (guid) => this.sync.call({ op: 'localEntityProperties', guid }, 2000),
        // Writes an export into exports\\<folder>\\ (architect/saves.hpp): its path, or null.
        exportFiles: (folder, files, map) =>
          this.sync.call(
            { op: 'localArchitectExport', folder, files, ...(map ? { map } : {}) },
            20000,
          ),
        // The game's world sounds that loop (architect/level_sounds.hpp).
        worldSounds: () => this.sync.call({ op: 'localSoundList' }, 2000),
        // Bake navmesh on a custom map (architect/navmesh_bake.hpp); a large map takes a while.
        bakeNavmesh: (blocked) => this.sync.call({ op: 'localNavmeshBake', blocked }, 30000),
        // A preview NPC's walk (npcs/local_npcs.hpp): walking, reached, no path ...
        npcWalkState: (id) => this.sync.call({ op: 'localNpcWalkState', id }, 2000),
        doorState: (guid) => this.sync.call({ op: 'localDoorState', guid }, 2000),
        gatesNear: (at, radius) => this.sync.call({ op: 'localGatesNear', at, radius }, 2000),
        log: (text) => this.log(owner, text),
        gameUi: (shown) => this.showGameUi(owner, shown),
        send: (message) => this.send(message),
        Render: this.render.facade(),
      });
    }
    return architectFacade(this.architect);
  }

  // The Asset/Animation Studio's editor, one for this game, made on first use. It
  // opens instead of World Architect's when the local mode runs the studio.
  assetStudio() {
    if (!this.studio) {
      const owner = '@studio';
      this.studio = new StudioEditor({
        NativeUI: this.nativeUI.facade(owner).NativeUI,
        NoClip: this.noclip.facade(),
        Key: this.input.keyFacade(owner),
        Controls: this.input.controlsFacade(owner),
        Vector3,
        log: (text) => this.log(owner, text),
        gameUi: (shown) => this.showGameUi(owner, shown),
        send: (message) => this.send(message),
        // The AI bridge's port, state, MCP server path and token, for the AI tab (one MCP server
        // for both editors, architect/ai_bridge.hpp).
        aiBridge: () => this.sync.call({ op: 'localArchitectAi' }, 2000),
      });
    }
    return this.studio;
  }

  // The game's own interface put away or back for the editor: every HUD element and nameplate,
  // and the client draws the watermark in the editor's menu bar ({ op: 'editorChrome' }).
  showGameUi(owner, shown) {
    for (const element of HUD_ELEMENTS) {
      this.hud.setHidden(owner, element, !shown);
    }
    this.nameplates.suppress(!shown);
    this.send({ op: 'editorChrome', open: !shown });
  }

  // Bytes of a file the resource ships to clients, for kcds://resources/<resource>/<path>; null when
  // the resource is not running or does not ship that file.
  // Before the resources start: the page of a resource's kcds.loadingScreen, with its files.
  showLoadingPage({ resource, page, read }) {
    this.earlyPage = { resource, read };
    this.loadingScreen.showManifestPage(resource, page);
  }

  shippedFile(name, file) {
    if (this.earlyPage?.resource === name) {
      const early = this.earlyPage.read(file);
      if (early) {
        return early;
      }
    }
    const definition = this.definitions.get(name);
    if (!this.resources.has(name) || !definition || !this.read) {
      return null;
    }
    if (!definition.files.some((entry) => entry.name === file)) {
      return null;
    }
    return this.read(name, file);
  }

  order(definitions = this.definitions) {
    const visiting = new Set();
    const visited = new Set();
    const result = [];
    const visit = (name) => {
      if (visited.has(name)) return;
      if (visiting.has(name)) throw new Error('Client resource dependency cycle');
      const resource = definitions.get(name);
      if (!resource) throw new Error('Required client resource missing');
      visiting.add(name);
      for (const dependency of resource.manifest.kcds.requires) {
        if (!definitions.has(dependency.name) && dependency.optional) continue;
        visit(dependency.name);
      }
      visiting.delete(name);
      visited.add(name);
      result.push(name);
    };
    for (const name of definitions.keys()) visit(name);
    return result;
  }

  async timeout(promise, milliseconds) {
    let timer;
    try {
      return await Promise.race([
        promise,
        new Promise((_, reject) => {
          timer = setTimeout(
            () => reject(new Error('Client resource lifecycle timeout')),
            milliseconds,
          );
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  }

  async apply(catalog, read) {
    this.read = read;
    const next = new Map(catalog.resources.map((resource) => [resource.manifest.name, resource]));
    const order = this.order(next);
    const changed = new Set();
    for (const [name, definition] of this.definitions) {
      if (next.get(name)?.hash !== definition.hash) changed.add(name);
    }
    // Dependents restart with their changed dependency, even if their own file
    // hash is unchanged: imported values still point to the previous module.
    for (const name of this.order()) {
      if (this.definitions.get(name).manifest.kcds.requires.some((dep) => changed.has(dep.name)))
        changed.add(name);
    }
    for (const name of [...this.order()].reverse()) {
      if (changed.has(name)) await this.stop(name);
    }
    this.definitions = next;
    // A resource that fails to start must not prevent independent resources from starting.
    // Its required dependents still refuse to start. The catalog is reported as failed after
    // every resource had its chance, so a partly failed catalog is never acknowledged as ready.
    const failures = [];
    for (const name of order) {
      if (this.resources.has(name)) {
        continue;
      }
      try {
        await this.start(name, read);
      } catch (error) {
        this.log(name, error?.stack ?? String(error));
        failures.push(error);
      }
    }
    // Registered item types' icons whose resources' files came only now.
    this.inventory.shipIcons();
    this.shipClips();
    if (failures.length === 1) {
      throw failures[0];
    }
    if (failures.length > 1) {
      throw new AggregateError(failures, 'Client resources failed to start');
    }
  }

  async start(name, read) {
    const definition = this.definitions.get(name);
    if (!definition) throw new Error('Unknown client resource');
    for (const dependency of definition.manifest.kcds.requires) {
      if (!dependency.optional && !this.resources.has(dependency.name))
        throw new Error('Required client dependency failed to start');
    }
    const state = { name, cache: new Map(), timers: new Set(), active: true };
    this.resources.set(name, state);
    const facade = this.services.add(name, definition.manifest);
    const invoke = (fn, args) => {
      if (!state.active) return;
      try {
        const result = fn(...args);
        if (result?.then) result.catch((error) => this.log(name, String(error)));
      } catch (error) {
        this.log(name, String(error));
      }
    };
    const globals = {
      Vector2,
      Vector3,
      Vector4,
      Quaternion,
      Color,
      Progression: progressionCatalog,
      ...facade,
      Web: this.web.facade(name),
      LoadingScreen: this.loadingScreen.facade(name),
      Key: this.input.keyFacade(name),
      Controls: this.input.controlsFacade(name),
      Keybinds: this.keybinds.facade(name),
      Hud: Object.freeze({ ...this.hud.facade(name), ...this.tutorialHint.facade(name) }),
      Chat: this.chat.facade(),
      ActionHint: this.actionHints.facade(name),
      ActionMenu: this.actionMenus.facade(name),
      WorldMap: this.worldMap.facade(),
      Compass: compassFacade(this.worldMap, this.hud.facade('@compass')),
      Travel: travelFacade(this.worldMap),
      Book: this.book.facade(name),
      CharacterCreator: this.characterCreator.facade(name),
      NoClip: this.noclip.facade(),
      LocalVoice: this.localVoice.facade(),
      Camera: this.camera.facade(),
      Vfx: this.vfx.facade(name),
      Light: this.lights.facade(name),
      Prefab: this.prefabs.facade(name),
      Collision: this.collision.facade(name),
      Terrain: this.terrain.facade(name),
      Vegetation: this.vegetation.facade(name),
      Render: this.render.facade(),
      World: this.world.facade(state),
      LocalNpc: this.localNpcs.facade(name),
      Gizmos: this.gizmos.facade(name),
      ...this.previews.facade(name),
      Nameplates: this.nameplates.facade(name),
      Discord: this.discord.facade(),
      Game: this.game.facade(),
      Audio: this.audio.facade(),
      Assets: this.assets.facade(),
      Follow: this.follow.facade(name),
      EmoteWheel: this.emoteWheel.facade(),
      Blip: this.blips.facade(name),
      Inventory: this.inventory.facade(name, invoke),
      ...this.nativeUI.facade(name),
      console: Object.fromEntries(
        ['log', 'info', 'warn', 'error', 'debug'].map((level) => [
          level,
          (...values) => this.log(name, values.map(String).join(' ')),
        ]),
      ),
      setTimeout: (fn, delay, ...args) => {
        const timer = setTimeout(() => {
          state.timers.delete(timer);
          invoke(fn, args);
        }, delay);
        state.timers.add(timer);
        return timer;
      },
      setInterval: (fn, delay, ...args) => {
        const timer = setInterval(() => invoke(fn, args), delay);
        state.timers.add(timer);
        return timer;
      },
      clearTimeout: (timer) => {
        clearTimeout(timer);
        state.timers.delete(timer);
      },
      clearInterval: (timer) => {
        clearInterval(timer);
        state.timers.delete(timer);
      },
      queueMicrotask: (fn) => queueMicrotask(() => invoke(fn, [])),
      Runtime: this.services.environment,
    };
    Object.defineProperties(globals, {
      ...Object.getOwnPropertyDescriptors(this.entities.facade(state)),
      ...Object.getOwnPropertyDescriptors(this.nativeAPI(name)),
    });
    const scope = withNamespace(globals);
    scope.globalThis = scope;
    const available = new Set(definition.files.map((file) => file.name));
    const load = (file) => {
      if (!available.has(file)) throw new Error('Client module is not a distributed resource file');
      if (state.cache.has(file)) return state.cache.get(file).exports;
      const module = { exports: {} };
      state.cache.set(file, module);
      try {
        const code = this.decoder.decode(read(name, file));
        if (file.endsWith('.json')) module.exports = JSON.parse(code);
        else
          this.evaluate(
            code,
            scope,
            module,
            (request) => {
              const resolved = resolveModule(request, file).find((candidate) =>
                available.has(candidate),
              );
              if (!resolved) throw new Error('Client resource module not found');
              return load(resolved);
            },
            file,
          );
        return module.exports;
      } catch (error) {
        state.cache.delete(file);
        throw error;
      }
    };
    const scripts = [
      ...definition.manifest.kcds.sharedScripts,
      ...definition.manifest.kcds.clientScripts,
    ];
    try {
      const language = definition.manifest.kcds.language?.client ?? 'javascript';
      if (language === 'lua') {
        // A Lua resource: its own Lua state over this same scope (lua_host.cjs).
        const readFile = (file) => {
          if (!available.has(file)) {
            throw new Error(`${file} is not a distributed resource file`);
          }
          return this.decoder.decode(read(name, file));
        };
        state.lua = await new LuaResource({ name, scope, readFile }).start();
        for (const file of scripts) {
          await state.lua.run(file);
        }
      } else if (language === 'javascript') {
        for (const file of scripts) {
          load(file);
        }
      } else if (language === 'csharp') {
        // A C# resource: its assemblies in their own context on this game's .NET
        // runtime, over this same scope (csharp_host.cjs).
        const readBytes = (file) => {
          if (!available.has(file)) {
            throw new Error(`${file} is not a distributed resource file`);
          }
          return read(name, file);
        };
        state.csharp = new CSharpResource({ name, scope, readBytes });
        await state.csharp.start(scripts);
      } else {
        throw new Error(`Client scripts in ${language} are not supported by this game yet`);
      }
      await this.timeout(this.services.emit('shared', 'resourceStart', [name]), 30000);
      this.log(name, 'client resource started');
    } catch (error) {
      await this.stop(name);
      throw error;
    }
  }

  async stop(name) {
    const state = this.resources.get(name);
    if (!state) return;
    for (const [other, definition] of this.definitions) {
      if (
        other !== name &&
        this.resources.has(other) &&
        definition.manifest.kcds.requires.some((dep) => dep.name === name)
      )
        await this.stop(other);
    }
    try {
      await this.timeout(this.services.emit('shared', 'resourceStop', [name]), 10000);
    } catch (error) {
      this.log(name, describeError(error));
    }
    state.active = false;
    this.web.removeOwner(name);
    this.input.removeOwner(name);
    this.keybinds.removeOwner(name);
    this.hud.removeOwner(name);
    this.actionMenus.removeOwner(name);
    this.actionHints.removeOwner(name);
    this.nativeUI.removeOwner(name);
    this.characterCreator.removeOwner(name);
    this.tutorialHint.removeOwner(name);
    this.inventory.removeOwner(name);
    this.localNpcs.removeOwner(name);
    this.vfx.removeOwner(name);
    this.lights.removeOwner(name);
    this.prefabs.removeOwner(name);
    this.collision.removeOwner(name);
    this.terrain.removeOwner(name);
    this.vegetation.removeOwner(name);
    this.gizmos.removeOwner(name);
    this.previews.removeOwner(name);
    this.nameplates.removeOwner(name);
    this.follow.removeOwner(name);
    this.blips.removeOwner(name);
    for (const timer of state.timers) {
      clearTimeout(timer);
      clearInterval(timer);
    }
    state.timers.clear();
    state.cache.clear();
    state.lua?.close();
    state.lua = null;
    state.csharp?.stop();
    state.csharp = null;
    this.entities.removeResource(state);
    this.services.remove(name);
    this.resources.delete(name);
    this.log(name, 'client resource stopped');
  }

  async disconnect() {
    this.earlyPage = null;
    this.loadingScreen.reset();
    for (const name of [...this.order()].reverse()) await this.stop(name);
    this.definitions.clear();
    this.tabletop.disconnect();
    this.entities.disconnect();
    this.progression.reset();
    this.buffs.reset();
  }
}

module.exports = { BrowserResourceRuntime, resolveModule };
