"""TypeScript declarations of the KCDS SDK, generated from the extracted API.

    python Dev/tools/web/build_typescript_declarations.py <api.json> <out dir> [--version X]

<api.json> is what Dev/tools/web/extract_sdk_api.cjs writes (the website's API, every member with
its side, signature, parameters, return and summary). Writes three files:

    shared.d.ts       the types server and client scripts have alike (Vector3, Quaternion, ...)
    server-api.d.ts   every other type as server scripts have it, referencing shared.d.ts
    client-api.d.ts   every other type as client scripts have it, referencing shared.d.ts

A project includes shared.d.ts and the side it writes for (a resource's server and client
scripts are checked as two projects).

A type is a class: a member written "Type.name(...)" is static, any other an instance member;
the global functions of Timers are declared as functions. Where the API gives no type, "any"
stands in; a type the API names but does not declare (Timeout, ...) is declared as "any" in
shared.d.ts. Every summary becomes the member's comment. The declarations describe the API; they
do not promise more than the website says.
"""

import argparse
import json
import re
from pathlib import Path

BUILTIN = {
    "any",
    "unknown",
    "void",
    "null",
    "undefined",
    "never",
    "string",
    "number",
    "boolean",
    "bigint",
    "object",
    "true",
    "false",
    "Promise",
    "Record",
    "Array",
    "ReadonlyArray",
    "Partial",
    "Pick",
    "Readonly",
    "Map",
    "Set",
    "Function",
    "Uint8Array",
    "ArrayBuffer",
    "Date",
    "Error",
    "keyof",
    "typeof",
}
GLOBAL_FUNCTION_TYPES = {"Timers"}
# Globals that are a value of another type rather than a class of their own: their type there.
VALUE_GLOBALS = {"LocalPlayer": "Player | null"}
# Members the API cannot describe as a TypeScript method: a function with members of its own.
MEMBER_DECLARATIONS = {
    ("process", "hrtime"): (
        "static hrtime: ((previous?: [number, number]) => [number, number]) & { bigint(): bigint };"
    ),
}
# An event argument the API gives no type: its type by its name, when the file declares that type
# (handlers of the game's events get typed parameters instead of implicit any).
EVENT_PARAMETER_TYPES = {
    "player": "Player",
    "sender": "Player",
    "target": "Player",
    "killer": "Player",
    "attacker": "Player",
    "thief": "Player",
    "victim": "Player",
    "stack": "GroundItem",
    "horse": "Horse",
    "npc": "Npc",
    "area": "Area",
    "entity": "Entity",
    "cart": "Cart",
    "door": "Door",
    "gate": "Gate",
    "grave": "Grave",
    "stash": "Stash",
    "prop": "Prop",
    "viewId": "number",
    "name": "string",
    "text": "string",
    "command": "string",
    "fragment": "string",
    "position": "Vector3",
    "connection": "JoinRequest",
    "rotation": "Quaternion",
}
# The handler types scripts name, declared in shared.d.ts, and the
# parameters that take them: (type, member, parameter) -> its type.
HANDLER_TYPES = [
    "/** Key.bind and Key.unbind: the key's name and the edge that happened. */",
    "type KeyHandler = (key: string, state: 'down' | 'up') => void;",
    "/** Keybinds.on and Keybinds.off: the action's id and the edge that happened. */",
    "type KeybindHandler = (id: string, state: 'down' | 'up') => void;",
    "/** Web.on and Web.off: the payload the page sent with sendToGame. */",
    "type WebEventHandler = (payload: any) => void;",
    "/** ActionHint.create: the id of the hint that was pressed or held to its end. */",
    "type ActionHintHandler = (id: number) => void | Promise<void>;",
    "/** NativeScreen.on: the arguments of the screen's notification. */",
    "type NativeScreenHandler = (...args: any[]) => void;",
    "/** NativeElement.on: the element's call arguments; with '*' also the event's name. */",
    "type NativeElementHandler = (args: any[], event?: string) => void;",
    "/** NativeClip.on: the clip whose event happened. */",
    "type NativeClipHandler = (clip: any) => void;",
    "/** Messages.handle: the request's payload and the function that answers it. */",
    "type MessageHandler = (payload: any, reply: (value?: any) => void) => any;",
    "/** Events.on with a resource's own event name: the arguments Events.emit gave it. */",
    "type EventHandler = (...args: any[]) => any;",
    "/** PropPlacer.on: the aimed pose (null for cancel and end) and whether the rules accept it. */",
    "type PlacementHandler = (pose: PlacementPose | null, valid: boolean) => void;",
]
HANDLER_PARAMETERS = {
    ("Key", "bind", "stateOrHandler"): "'down' | 'up' | 'both' | KeyHandler",
    ("Key", "bind", "handler"): "KeyHandler",
    ("Key", "unbind", "handler"): "KeyHandler",
    ("Keybinds", "on", "stateOrHandler"): "'down' | 'up' | 'both' | KeybindHandler",
    ("Keybinds", "on", "handler"): "KeybindHandler",
    ("Keybinds", "off", "state"): "'down' | 'up' | 'both'",
    ("Keybinds", "off", "handler"): "KeybindHandler",
    ("Keybinds", "add", "options"): (
        "{ label: string; category?: string; key?: string; controller?: string }"
    ),
    (
        "Keybinds",
        "set",
        "binding",
    ): "{ key?: string | null; controller?: string | null }",
    ("Keybinds", "addCategory", "id"): "string",
    ("Keybinds", "addCategory", "label"): "string",
    ("Keybinds", "add", "id"): "string",
    ("Keybinds", "remove", "id"): "string",
    ("Keybinds", "get", "id"): "string",
    ("Keybinds", "set", "id"): "string",
    ("Keybinds", "reset", "id"): "string",
    ("Keybinds", "on", "id"): "string",
    ("Keybinds", "off", "id"): "string",
    ("Web", "on", "handler"): "WebEventHandler",
    ("Web", "off", "handler"): "WebEventHandler",
    ("ActionHint", "create", "handler"): "ActionHintHandler",
    ("NativeScreen", "on", "handler"): "NativeScreenHandler",
    ("NativeElement", "on", "handler"): "NativeElementHandler",
    ("NativeClip", "on", "handler"): "NativeClipHandler",
    ("Messages", "handle", "handler"): "MessageHandler",
    ("PropPlacer", "on", "event"): "'move' | 'confirm' | 'rejected' | 'cancel' | 'end'",
    ("PropPlacer", "on", "handler"): "PlacementHandler",
    ("LevelEdit", "apply", "definition"): "LevelEditDefinition",
    ("Dialogue", "open", "page"): "DialoguePage",
    ("Dialogue", "update", "page"): "DialoguePage",
    ("Inventory", "remove", "request"): (
        "{ units: InventoryUnit[]; revision?: number; notify?: boolean }"
    ),
    (
        "Inventory",
        "transfer",
        "request",
    ): "{ units: InventoryUnit[]; notify?: boolean }",
    ("Player", "disconnect", "text"): "string",
    ("Player", "disconnect", "options"): "{ quit?: boolean }",
    ("Player", "ban", "reason"): "string",
    ("NativeUI", "addMenuButton", "label"): "string",
    ("NativeUI", "addMenuButton", "handler"): "(id: number) => void",
    ("NativeUI", "removeMenuButton", "id"): "number",
    ("Player", "jointPosition", "joint"): "string",
    ("Player", "jointRotation", "joint"): "string",
    ("Player", "setCollision", "collides"): "boolean",
    ("Animations", "list", "options"): "{ gender?: 'male' | 'female' }",
    ("Bans", "add", "identifiers"): "string | string[]",
    ("Bans", "add", "reason"): "string",
    ("Bans", "remove", "idOrIdentifier"): "string",
    ("Bans", "find", "identifierOrPlayer"): "string | Player",
    ("Inventory", "add", "request"): (
        "{ item: string; amount: number; metadata?: InventoryMetadata; revision?: number; "
        "notify?: boolean }"
    ),
    ("CookPot", "setFood", "food"): "CookPotFood",
    ("Trees", "configure", "settings"): "Partial<TreeSettings>",
    ("Trees", "setSpecies", "prefix"): "string",
    ("Trees", "setSpecies", "settings"): (
        "Partial<Pick<TreeSettings, 'hits' | 'trunkHits' | 'regrowMinutes' | 'height' | "
        "'trunkRadius' | 'drops' | 'stump'>>"
    ),
    ("Trees", "isTree", "mesh"): "string",
    ("Trees", "get", "id"): "string",
    ("Trees", "get", "dimension"): "number",
    ("Trees", "of", "levelObject"): "{ name: string; at: Vector3 }",
    ("Trees", "of", "dimension"): "number",
    ("Trees", "at", "position"): "Vector3",
    ("Trees", "at", "radius"): "number",
    ("Trees", "at", "dimension"): "number",
    ("Trees", "felled", "dimension"): "number",
    ("Trees", "regrowAll", "dimension"): "number",
    ("Trees", "state", "dimension"): "number",
    ("Trees", "restore", "list"): "TreeState[]",
}
# The data types scripts name, declared in shared.d.ts with the fields the runtime
# gives, and the members whose value is one: (type, member) -> its type.
DATA_TYPES = [
    "/** A body's look: its gender and the names of its parts ('' for the gender's default). */",
    "interface Appearance {",
    "  gender: 'male' | 'female';",
    "  body: string;",
    "  head: string;",
    "  hair: string;",
    "  beard: string;",
    "}",
    "/** A place of the game's map, a location or a point of interest: its GUID's fields. */",
    "interface LocationId {",
    "  data1: number;",
    "  data2: number;",
    "  data3: number;",
    "  data4: Uint8Array;",
    "}",
    "/** One row of an inventory: the item class GUID, its name and the stack's state. */",
    "interface InventoryRow {",
    "  id: string;",
    "  item: string;",
    "  name: string;",
    "  amount: number;",
    "  quality: number;",
    "  health: number;",
    "  condition: number;",
    "  equipped: number;",
    "  metadata: Record<string, any>;",
    "}",
    "/** A ban of this server (Bans, Player.ban): until is 0 for a ban without an end. */",
    "interface Ban {",
    "  id: string;",
    "  identifiers: string[];",
    "  reason: string;",
    "  by: string;",
    "  at: number;",
    "  until: number;",
    "}",
    "/** The properties of an inventory row a change gives: every one optional. */",
    "interface InventoryMetadata {",
    "  quality?: number;",
    "  health?: number;",
    "  condition?: number;",
    "  /** A poison's buff GUID, on ammunition. */",
    "  poison?: string;",
    "  poisonCharges?: number;",
    "  /** Buff GUIDs worn with the piece, on gear. */",
    "  onEquipBuffs?: string[];",
    "  /** The script's own fields: data is a plain object (JSON, up to 16 KiB), not text. */",
    "  custom?: { name?: string; description?: string; data?: Record<string, unknown> };",
    "  /** Not kept: the row leaves as its player disconnects, before playerDisconnect. */",
    "  transient?: boolean;",
    "}",
    "/** A player's quickslots: row ids of InventoryRow, null for an empty slot. */",
    "interface InventoryQuickslots {",
    "  weapons: { primary: string | null; secondary: string | null }[];",
    "  items: (string | null)[];",
    "  activeWeapon: number;",
    "  activeItem: number;",
    "}",
    "/** A player's inventory as the server holds it. */",
    "interface InventorySnapshot {",
    "  revision: number;",
    "  items: InventoryRow[];",
    "  quickslots: InventoryQuickslots | null;",
    "  ready: boolean;",
    "}",
    "/** An amount of one row, by its id from Inventory.get. */",
    "interface InventoryUnit {",
    "  id: string;",
    "  amount: number;",
    "}",
    "/** What an Inventory change answers: ok false with a code such as staleRevision when refused. */",
    "interface InventoryResult {",
    "  ok: boolean;",
    "  code: string;",
    "  revision: number;",
    "  items: InventoryUnit[];",
    "}",
    "/** playerInventoryChanged: each row before and after (after null for a removed row). */",
    "interface InventoryChange {",
    "  revision: number;",
    "  reason: string;",
    "  items: { id: string; before: InventoryRow | null; after: InventoryRow | null }[];",
    "}",
    "/** One row a thief took (playerPickpocketed). */",
    "interface PickpocketedItem {",
    "  item: string;",
    "  name: string;",
    "  amount: number;",
    "}",
    "/** LevelEdit.apply: a brush (by mesh path) or an entity (by GUID) to hide or move. */",
    "interface LevelEditDefinition {",
    "  kind?: 'brush' | 'entity';",
    "  name?: string;",
    "  guid?: string;",
    "  at?: Vector3 | [number, number, number];",
    "  hidden?: boolean;",
    "  position?: Vector3 | [number, number, number];",
    "  rotation?: Quaternion | [number, number, number, number] | { x: number; y: number; z: number };",
    "  scale?: number | Vector3;",
    "  model?: string;",
    "}",
    "/** A level object a ray hit (WorldRayHit.levelObject): what LevelEdit.apply needs to find it. */",
    "interface LevelObject {",
    "  kind: 'brush' | 'entity';",
    "  /** A brush's mesh path (lower case, '/'), or an entity's name. */",
    "  name: string;",
    "  /** An entity's GUID (undefined for a brush). */",
    "  guid?: string;",
    "  /** An entity's class (undefined for a brush). */",
    "  class?: string;",
    "  at: Vector3;",
    "  rotation: Quaternion;",
    "  scale: Vector3;",
    "  /** True for a brush its layer hides: an invisible collision of the level's (client hits). */",
    "  unseen?: boolean;",
    "}",
    "/** What a ray met: World.raycast, World.traceAll, World.snapToGround, weaponHit and",
    " * playerWeaponHit. */",
    "interface WorldRayHit {",
    "  position: Vector3;",
    "  normal: Vector3;",
    "  /** Metres from the ray's start. */",
    "  distance: number;",
    "  surface: string;",
    "  terrain: boolean;",
    "  kind: 'terrain' | 'entity' | 'vegetation' | 'brush' | 'static';",
    "  /** The mesh of a brush or a vegetation instance (lower case, '/'), or null. */",
    "  model: string | null;",
    "  category: 'tree' | 'bush' | 'plant' | 'rock' | null;",
    "  material: null;",
    "  entityGuid: string | null;",
    "  entityName: string | null;",
    "  entityClass: string | null;",
    "  levelObject: LevelObject | null;",
    "  /** Server scripts: the Player, Npc or Horse hit, or null. */",
    "  entity?: any;",
    "  /** Client scripts: the engine id of the entity hit, or null. */",
    "  entityId?: number | null;",
    "  /** Client scripts: the player hit, or null. */",
    "  player?: any;",
    "}",
    "/** Animations.list: one of the game's animations (a fragment with its tags). */",
    "interface AnimationInfo {",
    "  fragment: string;",
    "  tags: string;",
    "  /** Seconds. */",
    "  duration: number;",
    "  oneShot: boolean;",
    "  fullBody: boolean;",
    "  /** The prop kind each hand holds ('' for none), a name of Animations.props(). */",
    "  rightHand: string;",
    "  leftHand: string;",
    "  male: boolean;",
    "  female: boolean;",
    "  /** The sequence the fragment belongs to and its part of it ('' for none). */",
    "  sequence: string;",
    "  phase: string;",
    "  /** The fragment the partner plays with it ('' for none). */",
    "  partner: string;",
    "  slaveBody: boolean;",
    "  /** A player's body has it: what a woman's playAnimation plays (a man's: male). */",
    "  player: boolean;",
    "  /** Its first clip by name ('' for none): what playAnimation's upperBody plays. */",
    "  clip: string;",
    "}",
    "/** propPickedUp and propPlaced: the prop of a player's animation that moved. */",
    "interface AnimationPropMoment {",
    "  /** Its place in playAnimation's props. */",
    "  index: number;",
    "  fragment: string;",
    "  hand: 'right' | 'left';",
    "  /** The item class id, or null for a mesh. */",
    "  item: string | null;",
    "  /** The mesh, or null for an item class. */",
    "  model: string | null;",
    "  /** The Prop it rests as in the world, or null. */",
    "  rest: any;",
    "}",
    "/** Animations.props: a prop the game puts in people's hands. */",
    "interface AnimationPropInfo {",
    "  name: string;",
    "  model: string;",
    "  hand: string;",
    "}",
    "/** One row of a dialogue page. */",
    "interface DialogueOption {",
    "  id: string;",
    "  text: string;",
    "  enabled?: boolean;",
    "}",
    "/** Dialogue.open and update: an optional line above one to eight rows. */",
    "interface DialoguePage {",
    "  line?: string;",
    "  onRight?: boolean;",
    "  options: DialogueOption[];",
    "}",
    "/** PropPlacer: where the placement is aimed. surface is '' for now. */",
    "interface PlacementPose {",
    "  position: Vector3;",
    "  rotation: Quaternion;",
    "  normal: Vector3;",
    "  surface: string;",
    "  distance: number;",
    "  onSurface: boolean;",
    "}",
    "/** One parameter of a screen effect: its range, default and value that changes nothing. */",
    "interface ScreenEffectParam {",
    "  name: string;",
    "  description: string;",
    "  min: number;",
    "  max: number;",
    "  default: number;",
    "  neutral: number;",
    "}",
    "/** A tree as tree events name it (Trees): the level's tree and how far it was felled. */",
    "interface TreeInfo {",
    "  id: string;",
    "  species: string;",
    "  position: Vector3;",
    "  dimension: number;",
    "  state: 'standing' | 'falling' | 'felled' | 'cut';",
    "}",
    "/** A tree being chopped, felled or cut (Trees.get, the server's tree events). */",
    "interface Tree extends TreeInfo {",
    "  readonly hits: number;",
    "  readonly health: number;",
    "  readonly felledAt: number | null;",
    "  readonly regrowAt: number | null;",
    "  fell(direction?: Vector3 | null, player?: Player | null): boolean;",
    "  regrow(): void;",
    "  setHits(hits: number): boolean;",
    "}",
    "/** A counted axe blow on a tree or a felled trunk. */",
    "interface TreeBlow {",
    "  number: number;",
    "  of: number;",
    "  position: Vector3;",
    "}",
    "/** How a tree falls: its horizontal direction and how long it takes. */",
    "interface TreeFall {",
    "  direction: Vector3;",
    "  seconds: number;",
    "}",
    "/** Items a felled trunk gives. */",
    "interface TreeDrop {",
    "  item: string;",
    "  amount: number;",
    "}",
    "/** A touched tree as Trees.state keeps it and Trees.restore takes it. */",
    "interface TreeState {",
    "  id: string;",
    "  dimension: number;",
    "  species: string;",
    "  at: { x: number; y: number; z: number };",
    "  rotation: { w: number; x: number; y: number; z: number };",
    "  scale: { x: number; y: number; z: number };",
    "  state: 'standing' | 'felled' | 'cut';",
    "  hits: number;",
    "  trunkHits: number;",
    "  felledAt: number | null;",
    "  regrowAt: number | null;",
    "  pose: { w: number; x: number; y: number; z: number } | null;",
    "}",
    "/** The settings every tree uses (Trees.configure). */",
    "interface TreeSettings {",
    "  enabled: boolean;",
    "  hits: number;",
    "  trunkHits: number;",
    "  regrowMinutes: number;",
    "  regrowClearance: number;",
    "  reach: number;",
    "  swingSeconds: number;",
    "  height: number;",
    "  trunkRadius: number;",
    "  drops: TreeDrop[];",
    "  dropTo: 'inventory' | 'ground';",
    "  stump: string | null;",
    "  axes: string[] | null;",
    "}",
    "/** playerPreparing and playerPrepared: a cooking, smoking or drying at the game's own station. */",
    "interface Preparation {",
    "  kind: 'cooking' | 'smoking' | 'drying';",
    "  position: Vector3;",
    "  dimension: number;",
    "  station: string | null;",
    "  items?: { id: string; item: string; amount: number; product: string | null }[];",
    "}",
    "/** Keybinds.get and list: one of the resource's actions as it is bound now. */",
    "interface KeybindInfo {",
    "  id: string;",
    "  label: string;",
    "  category: string;",
    "  key: string | null;",
    "  keys: string[];",
    "  controller: string | null;",
    "  defaultKey: string | null;",
    "  defaultController: string | null;",
    "}",
    "/** Hud.effectList: a screen effect and its parameters. */",
    "interface ScreenEffectInfo {",
    "  name: string;",
    "  description: string;",
    "  params: ScreenEffectParam[];",
    "}",
    "/** chatMessage: a chat line the server sent this player (the author's name, '' for none). */",
    "interface ChatMessage {",
    "  author: string;",
    "  text: string;",
    "  /** 0xRRGGBB, 0 for the default colour. */",
    "  color: number;",
    "}",
    "/** The object every browser event of a resource's web view carries (Web). */",
    "interface BrowserEvent {",
    "  viewId: number;",
    "  url?: string;",
    "  isMainFrame?: boolean;",
    "  [field: string]: any;",
    "}",
    "/** webViewFailed: the page or frame that did not load, and why. */",
    "interface BrowserLoadingFailedEvent extends BrowserEvent {",
    "  url: string;",
    "  description: string;",
    "  errorCode: number;",
    "  isMainFrame: boolean;",
    "}",
    "/** A player's skills and perks as their game last reported them. */",
    "interface ProgressionSnapshot {",
    "  tracks: Record<string, { level: number; xp: number }>;",
    "  perks: string[];",
    "  perkPoints: Record<string, number>;",
    "}",
    "/** One cook pot's stock and state. */",
    "interface CookPotSnapshot {",
    "  guid: string;",
    "  position: Vector3;",
    "  food: CookPotFood | null;",
    "  nutrition: number | null;",
    "  poison: string | null;",
    "  portions: number;",
    "  capacity: number;",
    "  enabled: boolean;",
    "  state: 'empty' | 'half' | 'full';",
    "  fireLit: boolean | null;",
    "  dimension: number;",
    "}",
    "/** The meals a cook pot cooks. */",
    "type CookPotFood = 'goulash' | 'lentil' | 'soup';",
    "/** One of the level's own herbs: its pickable kind, the herb it gives, where it grows. */",
    "interface GatheringHerb {",
    "  kind: number;",
    "  item: string;",
    "  position: Vector3;",
    "}",
    "/** A join request in playerConnecting, before the player exists. */",
    "interface JoinRequest {",
    "  nickname: string;",
    "  steamId: string;",
    "  store: string;",
    "  storeAccount: string;",
    "  discordId: string;",
    "  discordVerified: boolean;",
    "  deviceId: string;",
    "  ip: string;",
    "  identifiers: { kind: string; id: string; verified: boolean }[];",
    "  ticket: string;",
    "  joinToken: string;",
    "  getIdentifier(kind: string): string;",
    "  reject(reason?: string): void;",
    "  update(message: string): void;",
    "  isPending(): boolean;",
    "}",
]
DATA_MEMBERS = {
    ("Player", "appearance"): "Appearance",
    ("Npc", "appearance"): "Appearance",
    ("LocalNpc", "appearance"): "Appearance",
    ("Player", "progression"): "ProgressionSnapshot | null",
    ("Player", "getInventory"): "InventorySnapshot | null",
    ("Inventory", "get"): "InventorySnapshot | null",
    ("Inventory", "add"): "InventoryResult",
    ("Inventory", "remove"): "InventoryResult",
    ("Inventory", "editCustom"): "InventoryResult",
    ("Inventory", "editRow"): "InventoryResult",
    ("Inventory", "set"): "InventoryResult",
    ("Inventory", "transfer"): "InventoryResult",
    ("Hud", "effectList"): "ScreenEffectInfo[]",
    ("Trees", "configure"): "TreeSettings",
    ("Trees", "settings"): "TreeSettings",
    ("Trees", "get"): "Tree | null",
    ("Trees", "of"): "Tree | null",
    ("Trees", "at"): "Tree[]",
    ("Trees", "felled"): "Tree[]",
    ("Trees", "state"): "TreeState[]",
    ("Keybinds", "get"): "KeybindInfo | null",
    ("Keybinds", "list"): "KeybindInfo[]",
    ("CookPot", "all"): "CookPotSnapshot[]",
    ("CookPot", "get"): "CookPotSnapshot | null",
    ("Gathering", "herbsWithin"): "GatheringHerb[]",
    ("Area", "herbsWithin"): "GatheringHerb[]",
}
# An event argument whose type its name does not tell: (event, parameter) -> its type.
EVENT_ARGUMENT_TYPES = {
    ("chatMessage", "message"): "ChatMessage",
    ("playerPickpocketed", "items"): "PickpocketedItem[]",
    ("playerInventoryChanged", "change"): "InventoryChange",
    ("propPickedUp", "prop"): "AnimationPropMoment",
    ("propPlaced", "prop"): "AnimationPropMoment",
    ("playerCarryingItem", "stack"): "GroundItem | null",
    ("playerPutDownItem", "stack"): "GroundItem | null",
    ("locationDiscovered", "locationId"): "LocationId",
    ("playerPreparing", "preparing"): "Preparation",
    ("playerPrepared", "preparation"): "Preparation",
    ("poiDiscovered", "poiId"): "LocationId",
    ("weaponHit", "hit"): "WorldRayHit",
    ("playerWeaponHit", "hit"): "WorldRayHit",
    ("playerTreeChop", "tree"): "Tree",
    ("playerTreeChop", "blow"): "TreeBlow",
    ("playerTreeFell", "tree"): "Tree",
    ("playerTreeFell", "fall"): "TreeFall",
    ("playerTrunkChop", "tree"): "Tree",
    ("playerTrunkChop", "blow"): "TreeBlow",
    ("playerTrunkCut", "tree"): "Tree",
    ("playerTrunkCut", "items"): "TreeDrop[]",
    ("treeFelled", "tree"): "TreeInfo",
    ("treeRegrow", "tree"): "Tree",
    ("treeRegrown", "tree"): "TreeInfo",
    ("treeChop", "tree"): "TreeInfo",
    ("treeChop", "blow"): "TreeBlow",
    ("treeFalling", "tree"): "TreeInfo",
    ("treeFalling", "fall"): "TreeFall",
    ("trunkCut", "tree"): "TreeInfo",
    ("trunkCut", "items"): "TreeDrop[]",
}
# The events of a resource's web views (Web): each handler gets one object with the view's id.
BROWSER_EVENTS = {
    "webViewOpened": "BrowserEvent",
    "webViewOrigin": "BrowserEvent",
    "webViewPopup": "BrowserEvent",
    "webViewNavigated": "BrowserEvent",
    "webViewBlocked": "BrowserEvent",
    "webViewLoading": "BrowserEvent",
    "webViewFailed": "BrowserLoadingFailedEvent",
    "webViewReady": "BrowserEvent",
    "webViewConsole": "BrowserEvent",
    "webViewCursor": "BrowserEvent",
    "webViewTooltip": "BrowserEvent",
    "webViewFocus": "BrowserEvent",
}
IDENTIFIER = re.compile(r"[A-Za-z_$][\w$]*")
SIMPLE_TYPE = re.compile(r"^[\w$\s\[\]|<>,.()'\"{}:;?\-]*$")


def comment(text, indent):
    """A JSDoc comment of a summary, or nothing."""
    text = (text or "").strip()
    if not text:
        return []
    lines = []
    words = text.replace("*/", "* /").split()
    line = ""
    for word in words:
        if len(line) + len(word) + 1 > 96:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}" if line else word
    if line:
        lines.append(line)
    out = [f"{indent}/**"]
    out += [f"{indent} * {item}" for item in lines]
    out.append(f"{indent} */")
    return out


def object_shorthand(text):
    """'{ name, kind: 'a' | 'b', done? }' as '{ name: any; kind: 'a' | 'b'; done?: any }'."""

    def expand(match):
        fields = []
        for part in match.group(1).split(","):
            part = part.strip()
            if not part:
                continue
            bare = re.fullmatch(r"([A-Za-z_$][\w$]*)(\?)?", part)
            if bare:
                fields.append(f"{bare.group(1)}{bare.group(2) or ''}: any")
            elif re.match(r"[A-Za-z_$][\w$]*\??\s*:", part):
                fields.append(part)
            else:
                return match.group(0)
        return "{ " + "; ".join(fields) + " }"

    return re.sub(r"\{([^{}]*)\}", expand, text)


def ts_type(text, fallback="any"):
    """A type as the API writes it, as TypeScript; the fallback where there is none or it is not
    one."""
    text = (text or "").strip()
    if not text:
        return fallback
    text = object_shorthand(text)
    if not SIMPLE_TYPE.match(text) or text.count("(") != text.count(")"):
        return fallback
    if text.count("{") != text.count("}") or text.count("<") != text.count(">"):
        return fallback
    # Words side by side are prose ("table of things"), not a type (keyof and typeof aside).
    unquoted = re.sub(r"'[^']*'|\"[^\"]*\"", "''", text)
    for first, _second in re.findall(r"([\w$]+)\s+([\w$]+)", unquoted):
        if first not in ("keyof", "typeof"):
            return fallback
    # A lowercase word that is no TypeScript type (array, table, ...) stands for any; a property
    # name before its colon stays. The API's "object" is a plain object of fields its summary
    # names, which TypeScript's object would not let a script read: any.
    return re.sub(
        r"(?<![\w$.'\"])([a-z][\w$]*)(?!\s*\??:)(?![\w$'\"])",
        lambda m: (m.group(1) if m.group(1) in BUILTIN and m.group(1) != "object" else "any"),
        text,
    )


def referenced(text):
    """The type names a TypeScript type text refers to (quoted strings left out)."""
    text = re.sub(r"'[^']*'|\"[^\"]*\"", "", text)
    text = re.sub(r"\{[^{}]*\}", "", text)
    return {name for name in IDENTIFIER.findall(text) if not name[0].islower() or name in BUILTIN}


def parameter(param):
    name = param.get("name") or "value"
    rest = name.startswith("...")
    clean = name[3:] if rest else name
    clean = re.sub(r"[^\w$]", "_", clean) or "value"
    kind = ts_type(param.get("type"))
    if rest:
        return f"...{clean}: {kind if kind.endswith('[]') else 'any[]'}"
    described = (param.get("description") or "").lower()
    # An options object is always optional in the SDK.
    optional = (
        clean == "options"
        or param.get("optional")
        or param.get("default") is not None
        or described.startswith("optional")
        or "(optional" in described
        or "default" in described
    )
    return f"{clean}{'?' if optional else ''}: {kind}"


def parameters(params):
    """A parameter list; once one is optional the later ones are too (TypeScript wants them so)."""
    out = []
    optional_seen = False
    for param in params:
        text = parameter(param)
        if text.startswith("..."):
            out.append(text)
            continue
        if "?:" in text.split(":", 1)[0] + ":":
            optional_seen = True
        elif optional_seen:
            name, kind = text.split(":", 1)
            text = f"{name}?:{kind}"
        out.append(text)
    return ", ".join(out)


def method_lines(member, indent, static, as_function=False):
    params = parameters(member.get("params", []))
    returns = ts_type(member.get("returns"), "any")
    summary = member.get("summary", "")
    described = [
        f"@param {p['name'].lstrip('.')} {p['description']}"
        for p in member.get("params", [])
        if p.get("description")
    ]
    text = summary + (" " if summary and described else "") + " ".join(described)
    lines = comment(text, indent)
    name = member["name"]
    if as_function:
        lines.append(f"{indent}declare function {name}({params}): {returns};")
    else:
        prefix = "static " if static else ""
        lines.append(f"{indent}{prefix}{name}({params}): {returns};")
    return lines, {returns} | {ts_type(p.get("type")) for p in member.get("params", [])}


def property_lines(member, indent, static):
    kind = ts_type(member.get("type"))
    lines = comment(member.get("summary", ""), indent)
    prefix = "static " if static else ""
    readonly = "readonly " if member.get("readonly", True) else ""
    lines.append(f"{indent}{prefix}{readonly}{member['name']}: {kind};")
    return lines, {kind}


def is_static(type_name, member):
    """A namespace's entries and a class's statics (the extractor marks them; older APIs without
    the mark name a static method Type.name(...))."""
    if member.get("static") is not None:
        return bool(member["static"])
    signature = member.get("signature", "")
    return signature.startswith(f"{type_name}.")


def all_shared(api_type):
    """Whether every member of a type is the same on both sides (the type goes in shared.d.ts)."""
    return all(
        member.get("side") == "shared" and not member.get("variants")
        for member in api_type["members"]
    )


def as_side(member, side):
    """The member as that side calls it: a side with its own variant (World.raycast on the
    client) takes the variant's signature, parameters, return and summary."""
    variant = (member.get("variants") or {}).get(side)
    if not variant:
        return member
    own = dict(member)
    for field in ("signature", "returns", "summary"):
        if variant.get(field):
            own[field] = variant[field]
    # A variant that takes nothing (Camera.endShot() on the client) has an empty list.
    if variant.get("params") is not None:
        own["params"] = variant["params"]
    return own


def members_for(api_type, side):
    """The members a file declares of a type: all of a shared type in shared.d.ts, the shared and
    that side's own of any other type in that side's file. A type whose members differ by side
    goes in the sides' files."""
    if side == "shared":
        return list(api_type["members"]) if all_shared(api_type) else []
    if all_shared(api_type):
        return []
    return [as_side(m, side) for m in api_type["members"] if m.get("side") in (side, "shared")]


INTERNAL_NAMESPACE = "KcdsTypes"


def namespace_block(types, known, classes):
    """The `kcds` object a script gets on this side, each domain with its types, and the same paths
    as type names (kcds.world.Door). The types themselves stay out of the global scope.
    """
    domains = {}
    for api_type in types:
        domain = api_type.get("domain")
        if domain and api_type["name"] in known:
            domains.setdefault(domain, []).append(api_type["name"])
    lines = ["/** The KCDS SDK, by domain. */", "declare const kcds: {"]
    for domain in sorted(domains):
        lines.append(f"  readonly {domain}: {{")
        for name in sorted(domains[domain]):
            lines.append(f"    readonly {name}: typeof {INTERNAL_NAMESPACE}.{name};")
        lines.append("  };")
    lines += ["};", "", "declare namespace kcds {"]
    for domain in sorted(domains):
        names = [name for name in sorted(domains[domain]) if name in classes]
        if names:
            lines.append(f"  namespace {domain} {{")
            for name in names:
                lines.append(f"    type {name} = {INTERNAL_NAMESPACE}.{name};")
            lines.append("  }")
    lines += ["}", ""]
    return lines


def internal_block(lines):
    """Lines of the SDK's own declarations, inside the namespace that keeps them out of the global
    scope. The namespace is ambient, so nothing in it says `declare` again."""
    if not lines:
        return []
    wrapped = [f"declare namespace {INTERNAL_NAMESPACE} {{"]
    for line in lines:
        if not line:
            wrapped.append("")
        elif line.startswith("declare "):
            wrapped.append("  " + line[len("declare ") :])
        else:
            wrapped.append("  " + line)
    wrapped += ["}", ""]
    return wrapped


def type_block(api_type, side, events):
    """The declaration of one type in a side's file ("shared" for shared.d.ts); the type texts it
    uses."""
    name = api_type["name"]
    used = set()
    if name in VALUE_GLOBALS:
        own_side = api_type.get("side", "shared")
        if side == own_side or (side != "shared" and own_side not in ("server", "client")):
            lines = comment(api_type.get("summary", ""), "")
            lines.append(f"declare const {name}: {VALUE_GLOBALS[name]};")
            return lines, {VALUE_GLOBALS[name]}
        return [], used
    members = members_for(api_type, side)
    if not members:
        return [], used
    lines = []
    if name in GLOBAL_FUNCTION_TYPES:
        lines += comment(api_type.get("summary", ""), "")
        for member in members:
            if member["kind"] == "method":
                block, types = method_lines(member, "", False, as_function=True)
                lines += block
                used |= types
        return lines, used
    lines += comment(api_type.get("summary", ""), "")
    base = api_type.get("base") or ""
    lines.append(f"declare class {name}{f' extends {base}' if base else ''} {{")
    # The SDK's value types are made with new (new Vector3(x, y, z)); the API lists no
    # constructors, so every class takes what its own docs give it.
    lines.append("  constructor(...args: any[]);")
    if name == "Events" and side in ("server", "client"):
        lines += events_overloads(side)
    seen = set()
    nested = set()
    for member in members:
        static = is_static(name, member)
        key = (member["kind"], member["name"], static)
        # A nested namespace's entry (Locales.defaults.items.maxTooltip): its first part is
        # declared once, as any.
        if "." in member["name"] and member["kind"] != "event":
            first = member["name"].split(".")[0]
            if IDENTIFIER.fullmatch(first) and first not in nested:
                nested.add(first)
                prefix = "static " if static else ""
                lines.append(f"  {prefix}readonly {first}: any;")
            continue
        if key in seen or not IDENTIFIER.fullmatch(member["name"]):
            continue
        # A deprecated member keeps working but is not declared: its old name may clash with the
        # base type's (Blip.state).
        if member.get("stability") == "deprecated":
            continue
        seen.add(key)
        if member["kind"] == "event":
            events.append(member)
            continue
        if (name, member["name"]) in MEMBER_DECLARATIONS:
            lines += comment(member.get("summary", ""), "  ")
            lines.append(f"  {MEMBER_DECLARATIONS[(name, member['name'])]}")
            continue
        if member["kind"] == "method":
            block, types = method_lines(member, "  ", static)
        else:
            block, types = property_lines(member, "  ", static)
        lines += block
        used |= types
    lines.append("}")
    return lines, used


def events_overloads(side):
    """Events.on, once and off for an event the side's scripts hear: the handler's parameters are
    those of the event (the untyped signatures after them take a resource's own events).
    """
    interface = "ServerEvents" if side == "server" else "ClientEvents"
    key = f"K extends keyof {interface}"
    lines = comment(
        "An event of the game or the SDK: the handler's parameters are typed (ServerEvents, "
        "ClientEvents). A resource's own event names take the signatures below.",
        "  ",
    )
    lines.append(f"  static on<{key}>(name: K, fn: {interface}[K]): any;")
    lines.append(f"  static once<{key}>(name: K, fn: {interface}[K]): void;")
    lines.append(f"  static off<{key}>(name: K, fn: {interface}[K]): void;")
    # A resource's own events: the handler's arguments are any, not implicitly so.
    lines.append("  static on(name: string, fn: EventHandler): any;")
    lines.append("  static once(name: string, fn: EventHandler): void;")
    lines.append("  static off(name: string, fn: EventHandler): void;")
    if side == "server":
        handler = "(player: Player, payload: any) => any"
        lines.append(f"  static fromClient(name: string, fn: {handler}): any;")
        lines.append(f"  static onceFromClient(name: string, fn: {handler}): void;")
    return lines


def typed_event_parameters(event, params, known):
    """An event's parameters with a type by name where the API gives none."""
    typed = []
    for param in params:
        own = dict(param)
        named = EVENT_ARGUMENT_TYPES.get((event, own.get("name", "")))
        if not own.get("type") and named:
            own["type"] = named
        guess = EVENT_PARAMETER_TYPES.get(own.get("name", ""))
        builtin = guess in ("string", "number")
        if not own.get("type") and guess and (builtin or guess in known):
            own["type"] = guess
        typed.append(own)
    return typed


def events_block(events, side, known):
    """The events a side's scripts hear, as an interface of their handlers' parameters."""
    interface = "ServerEvents" if side == "server" else "ClientEvents"
    lines = [f"/** The events {side} scripts hear with Events.on(name, handler). */"]
    lines.append(f"interface {interface} {{")
    seen = set()
    for event in sorted(events, key=lambda e: e["name"]):
        if event["name"] in seen:
            continue
        seen.add(event["name"])
        params = parameters(typed_event_parameters(event["name"], event.get("params", []), known))
        lines += comment(event.get("summary", ""), "  ")
        lines.append(f"  {event['name']}: ({params}) => any;")
    if side == "client":
        for name, kind in BROWSER_EVENTS.items():
            if name not in seen:
                lines.append("  /** Web: an event of one of the resource's own views. */")
                lines.append(f"  {name}: (event: {kind}) => any;")
    lines.append("}")
    return lines


def build(api, version):
    types = api["types"]
    for api_type in types:
        for member in api_type.get("members", []):
            for param in member.get("params", []):
                key = (api_type["name"], member["name"], param.get("name"))
                if key in HANDLER_PARAMETERS:
                    param["type"] = HANDLER_PARAMETERS[key]
            data = DATA_MEMBERS.get((api_type["name"], member["name"]))
            if data:
                member["type" if member.get("kind") == "property" else "returns"] = data
    declared = (
        {"ServerEvents", "ClientEvents"}
        | {line.split()[1] for line in HANDLER_TYPES if line.startswith("type ")}
        | {line.split()[1] for line in DATA_TYPES if line.startswith(("interface ", "type "))}
    )
    header = [
        f"// KCDS SDK {version}: generated from the SDK's own API, as https://kcdsynced.com/docs shows it.",
        "// Do not edit: regenerate it from the release instead.",
        "",
    ]
    outputs = {}
    used = {}
    declared_in = {}
    classes = set()
    for side in ("shared", "server", "client"):
        lines = list(header)
        if side == "shared":
            lines += [
                # An interface and a var, as TypeScript's own DOM library declares the console: a
                # project that adds "DOM" to its lib (for fetch, WebSocket and the other web classes
                # server scripts have) merges the two instead of failing on a second console.
                "/** The resource's log (the server's console, the client's log), one line per call. */",
                "interface Console {",
                "  log(...values: any[]): void;",
                "  info(...values: any[]): void;",
                "  warn(...values: any[]): void;",
                "  error(...values: any[]): void;",
                "  debug(...values: any[]): void;",
                "}",
                "declare var console: Console;",
                "",
                "/** Scripts are CommonJS: require a resource's own files (and, on the server, Node's). */",
                "declare function require(id: string): any;",
                "declare const module: { exports: any };",
                "declare const exports: any;",
                "",
            ]
        inside = []
        if side == "shared":
            inside += HANDLER_TYPES + [""] + DATA_TYPES + [""]
        if side != "shared":
            lines.append('/// <reference path="./shared.d.ts" />')
            lines.append("")
        events = []
        used[side] = set()
        declared_in[side] = set()
        if side == "shared":
            # The handler and data types are declared there too: event parameters may name them.
            declared_in[side] |= declared - {"ServerEvents", "ClientEvents"}
        for api_type in types:
            block, types_used = type_block(api_type, side, events)
            if block:
                target = inside if api_type.get("domain") else lines
                target += block + [""]
                used[side] |= types_used
                declared_in[side].add(api_type["name"])
                if any(line.startswith("declare class ") for line in block):
                    classes.add(api_type["name"])
        if side != "shared" and events:
            known = declared_in["shared"] | declared_in[side]
            events_lines = events_block(events, side, known) + [""]
            has_namespaced = any(api_type.get("domain") for api_type in types)
            target = inside if has_namespaced else lines
            target += events_lines
        lines += internal_block(inside)
        if side != "shared" and any(api_type.get("domain") for api_type in types):
            lines += namespace_block(types, declared_in["shared"] | declared_in[side], classes)
        outputs[side] = lines
    # A type a file names but neither it nor shared.d.ts declares is "any" there (a client-only
    # type a server member names, Timeout, ...).
    for side in ("shared", "server", "client"):
        known = declared | declared_in["shared"] | declared_in[side]
        stubs = sorted(
            {
                name
                for text in used[side]
                for name in referenced(text)
                if name not in BUILTIN and name not in known
            }
        )
        if stubs:
            outputs[side] += ["// Types the API names without declaring them here."]
            outputs[side] += [f"type {name} = any;" for name in stubs] + [""]
    return {
        "shared.d.ts": "\n".join(outputs["shared"]),
        "server-api.d.ts": "\n".join(outputs["server"]),
        "client-api.d.ts": "\n".join(outputs["client"]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("api")
    parser.add_argument("out")
    parser.add_argument("--version", default=None)
    options = parser.parse_args()
    api = json.loads(Path(options.api).read_text(encoding="utf-8"))
    version = options.version or api.get("apiVersion") or "dev"
    out = Path(options.out)
    out.mkdir(parents=True, exist_ok=True)
    for name, text in build(api, version).items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
        print(f"{name}: {text.count(chr(10))} lines")


if __name__ == "__main__":
    main()
