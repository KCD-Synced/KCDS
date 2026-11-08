'use strict';

// World Architect's screen, laid out 1:1 from the owner's final design canvas (its
// 1600 by 900 boards times 1.2 on the Menu movie's 1920 by 1080 stage): the menu bar with the KCDS
// logo, the toolbar (tools, snaps, modes, undo, redo, play as player), the outliner with its
// search, filter and tree, the details with their sections and fields, the content browser with
// its folders, search and tiles, and the status bar. The world view between them takes presses on
// the world and shows the camera speed chip and the axis indicator.
//
// Two layers make it:
// - Scaleform, on one NativeScreen: the panels, the cursor and the presses. Every clip call is a
//   request the game answers in about a frame, so each panel is ONE clip whose single drawing
//   holds its ground, borders, highlights, fields, icons and tiles, and which takes its own presses
//   (the place of a press tells what was pressed).
// - The client's overlay (Client/runtime/architect/editor_overlay.hpp): every text, in the font
//   the KCDS watermark uses (Segoe UI), and the content tiles' pictures. The game's Scaleform has
//   only its embedded fonts and shows no PNG. All the text goes in one message when it changed.
//
// The editor (editor.cjs) gives render() a plain description of what to show.

const {
  STAGE,
  COLORS,
  LAYOUT,
  PANE_SIZES,
  setPanes,
  paneSizes,
  dockFolded,
  wideView,
} = require('./theme.cjs');
const { icon } = require('./icons.cjs');

const FIELD = 0x0f1012;
const BUTTON = 0x1f2226;
const CHIP_ON = 0x2a2418;
const CHIP_ON_LINE = 0x5a4420;
const ICON = 0xc9cdd2;
const GREEN = 0x57c26a;
// The axes mark's arm, stage pixels.
const AXIS_MARK = 26;
// A drawing takes at most 1024 commands; three a line, one 'clear'.
const MOST_LINE_COMMANDS = 1020;

// The part of the segment a-b inside the area (Liang-Barsky), or null when none is.
function clipToView(a, b, area) {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  let enter = 0;
  let leave = 1;
  const edges = [
    [-dx, a.x - area.x],
    [dx, area.x + area.width - a.x],
    [-dy, a.y - area.y],
    [dy, area.y + area.height - a.y],
  ];
  for (const [p, q] of edges) {
    if (p === 0) {
      if (q < 0) {
        return null;
      }
      continue;
    }
    const r = q / p;
    if (p < 0) {
      enter = Math.max(enter, r);
    } else {
      leave = Math.min(leave, r);
    }
    if (enter > leave) {
      return null;
    }
  }
  return [
    { x: a.x + dx * enter, y: a.y + dy * enter },
    { x: a.x + dx * leave, y: a.y + dy * leave },
  ];
}
// Presses at one point this close together are one click heard by two clips, milliseconds.
const SAME_PRESS_MS = 150;
// The hover name's box beside the cursor (stage pixels).
// Right of the game's cursor arrow (about 44 by 50 stage pixels from its tip), level with its tip.
const HOVER = Object.freeze({ right: 48, down: 4, pad: 10, height: 44, most: 420 });
// The menu bar's map button, the switch's question and buttons, the save chip and, at the right
// end, Leave (back to the main menu and the server list).
const MAP_BUTTON = Object.freeze({ x: 850, width: 220 });
const ARCHITECT = Object.freeze({ x: 172, width: 190 });
// The map list under the map button while it is open (a row of chips ran
// out of room with the custom maps): a row a map, the current one marked, at most `most` rows at
// once and the others a wheel turn or a press on the scroll bar away.
const MAP_LIST = Object.freeze({ width: 300, row: 30, pad: 4, most: 12, bar: 6, top: 2 });
const MAP_CONFIRM = Object.freeze({
  ask: { x: 560, width: 520 },
  yes: { x: 1092, width: 96 },
  no: { x: 1196, width: 96 },
});
const SAVE_CHIP = Object.freeze({ x: 1632, width: 188 });
const LEAVE = Object.freeze({ x: 1828, width: 84 });
const AXES = Object.freeze({ x: 0xf06b5f, y: 0x6cc46a, z: 0x5ea2f2 });
// Text sizes in stage pixels: the design's 12, 11 and 13.5 times 1.2.
const TEXT = Object.freeze({ body: 15, small: 13, title: 16 });

// Toolbar places (stage pixels).
const TOOL = Object.freeze({ x: 14, y: 7, width: 36, height: 34, gap: 3 });
const SNAPS = Object.freeze([
  { x: 214, width: 84 },
  { x: 304, width: 76 },
  { x: 386, width: 112 },
  // A fourth switch an editor may have (the studio's Grid).
  { x: 504, width: 76 },
]);
const MODE = Object.freeze({ x: 530, y: 6, width: 128, height: 36, count: 7 });
// The mode tabs after a fourth snap switch (the studio's Grid ends at 580: Props lay under it,
// owner), and wider when there are at most five of them (Animation fitted no more).
const MODE_AFTER_FOUR_SNAPS = 590;
const MODE_WIDE = 148;

// Where the mode tabs go for this many snap switches and modes: { x, width, count }.
function modeBar(snaps, modes) {
  return {
    x: snaps > 3 ? MODE_AFTER_FOUR_SNAPS : MODE.x,
    width: modes <= 5 ? MODE_WIDE : MODE.width,
    count: modes,
  };
}
const UNDO = Object.freeze({ x: 1608, width: 36 });
const REDO = Object.freeze({ x: 1646, width: 36 });
const PLAY = Object.freeze({ x: 1700, width: 206 });

// Outliner places (from the panel's left and top).
const OUTLINER = Object.freeze({
  tabs: 38,
  search: { x: 12, y: 46, width: 262, height: 30 },
  filter: { x: 282, y: 46, width: 32, height: 30 },
  header: 84,
  rows: 110,
  row: 28,
  type: 236,
  // The footer's height at the panel's bottom.
  foot: 34,
});
// The outliner footer's page buttons (‹ and ›), each 34 wide.
const OUTLINER_PAGES = Object.freeze({ back: 246, next: 286, width: 34 });

// The outliner's footer line and how many rows fit above it: the panel is as high as the window
// lets it be (theme.cjs fitStage), 19 rows at 1080.
function outlinerFoot() {
  return LAYOUT.outliner.height - OUTLINER.foot;
}

function outlinerRows() {
  return Math.floor((outlinerFoot() - OUTLINER.rows) / OUTLINER.row);
}

// The places above are the 326-wide outliner's; a wider (dragged) one keeps them at its right
// edge: the search field grows, the filter, the type column and the footer's page buttons move.
function outlinerPlaces() {
  const wider = LAYOUT.outliner.width - PANE_SIZES.outliner;
  return {
    search: { ...OUTLINER.search, width: OUTLINER.search.width + wider },
    filter: { ...OUTLINER.filter, x: OUTLINER.filter.x + wider },
    type: OUTLINER.type + wider,
    back: OUTLINER_PAGES.back + wider,
    next: OUTLINER_PAGES.next + wider,
  };
}

// Details places.
const DETAILS = Object.freeze({
  tabs: 38,
  head: 46,
  rows: 106,
  row: 32,
  count: 18,
  box: { x: 150, width: 220 },
  // The scroll footer's height at the panel's bottom (shown only when the rows do not fit).
  foot: 34,
  // The scroll bar's width at the panel's right edge.
  bar: 6,
});
const DETAIL_ROWS = DETAILS.count;

// How near a panel's edge a press grabs it, stage pixels either side.
const EDGE_REACH = 5;

// The panel edge a press at stage point (x, y) grabs: 'outliner' (its right edge), 'details' (its
// left edge), 'dock' (its top edge), or null.
function edgeAt(x, y) {
  // The wide view has no panels to size.
  if (wideView()) {
    return null;
  }
  const { outliner, details, dock } = LAYOUT;
  const beside = y >= outliner.y && y < outliner.y + outliner.height;
  if (beside && Math.abs(x - outliner.width) <= EDGE_REACH) {
    return 'outliner';
  }
  if (beside && Math.abs(x - details.x) <= EDGE_REACH) {
    return 'details';
  }
  if (Math.abs(y - dock.y) <= EDGE_REACH) {
    return 'dock';
  }
  return null;
}

// The grips on the world view's edges that can be dragged (its left, right and bottom edge): three
// short lines across the middle of each, in the view's own pixels.
function edgeGrips(area) {
  const commands = [['lineStyle', 2, COLORS.faint, 70]];
  const middleY = Math.round(area.height / 2);
  const middleX = Math.round(area.width / 2);
  for (const step of [-6, 0, 6]) {
    commands.push(['moveTo', 3, middleY + step - 2], ['lineTo', 3, middleY + step + 2]);
    commands.push(
      ['moveTo', area.width - 3, middleY + step - 2],
      ['lineTo', area.width - 3, middleY + step + 2],
    );
    commands.push(
      ['moveTo', middleX + step * 2 - 4, area.height - 3],
      ['lineTo', middleX + step * 2 + 4, area.height - 3],
    );
  }
  return commands;
}

// A Details row's value field: it grows with a wider (dragged) panel.
function detailField() {
  const wider = LAYOUT.details.width - PANE_SIZES.details;
  return { x: DETAILS.box.x, width: DETAILS.box.width + wider };
}

// How many Details rows fit in the panel: all of its height, or above the scroll footer when
// there are more rows than fit.
function detailRows(total) {
  const height = LAYOUT.details.height - DETAILS.rows;
  const all = Math.floor(height / DETAILS.row);
  if (total <= all) {
    return all;
  }
  return Math.max(1, Math.floor((height - DETAILS.foot) / DETAILS.row));
}

// The first row shown when the rows from `from` on are scrolled to: kept inside the rows.
function detailStart(from, total) {
  const fit = detailRows(total);
  return Math.max(0, Math.min(from, total - fit));
}

function detailFoot() {
  return LAYOUT.details.height - DETAILS.foot;
}

// Content browser places.
const DOCK = Object.freeze({
  tabs: 34,
  nav: 252,
  folders: 66,
  folderRow: 26,
  top: 42,
  chip: { width: 96 },
  search: { x: 1236, width: 300 },
  pager: { x: 1560, width: 348 },
  tiles: { x: 264, y: 84, width: 157, height: 186, gap: 8, art: 112 },
});
const TILE_COUNT = 10;
// The dock's left column on the tabs without folders.
const DOCK_ABOUT = Object.freeze({
  3: {
    title: 'SAVES',
    lines: ['Your setups on this PC.', 'Load one instead of the work,', 'or merge it in.'],
  },
  4: {
    title: 'AI ASSISTANT',
    lines: ['Let an AI app use the editor:', 'copy a line into it.', 'All it does can be undone.'],
  },
});
// The Validation tab's rows.
const ISSUE = Object.freeze({ top: 80, row: 36 });
const ISSUE_ROWS = 5;
// The Saves tab: the name field, Save as, and each row's buttons.
const SAVES = Object.freeze({
  field: { x: 264, width: 420 },
  saveAs: { x: 696, width: 130 },
  // Reset map (World Architect's only), at the right end above the rows' buttons.
  reset: { x: 1424, width: 140 },
  buttons: Object.freeze([
    { action: 'load', label: 'Load', x: 1240, width: 100 },
    { action: 'merge', label: 'Merge', x: 1352, width: 100 },
    { action: 'delete', label: 'Delete', x: 1464, width: 100 },
  ]),
});
// The dock's tabs: each this wide, from the left (World Architect's five; an editor that names
// its own tabs, the Asset/Animation Studio's, has as many as it names).
const TAB = Object.freeze({ x: 8, width: 112 });
const ARCHITECT_TABS = 5;
const FOLDER_ROWS = 8;
// The most drawing commands one NativeUI draw call takes (1024), less the clear, and the commands
// a shape starts with (where a long drawing may be cut).
const DRAW_PART = 1000;
const SHAPE_STARTS = new Set(['moveTo', 'beginFill', 'lineStyle']);
// The AI tab: its rows (a label, the text in a field, Copy at the panel's right edge) and the
// dot that shows whether an assistant is connected.
const AI = Object.freeze({
  top: 84,
  row: 44,
  label: 220,
  field: { x: 488, height: 32 },
  copy: { x: 1788, width: 112 },
  dots: Object.freeze({ connected: 0x6cc46a, waiting: 0xf0a93b, off: 0xff8a7a }),
});

// The places above are the 1920-wide design's. The editor's stage is as wide as the window's shape
// (theme.cjs fitStage): what sits at the right end moves by how much wider it is, what sits in the
// middle by half that, and the content tiles share the dock's width.
function wider() {
  return LAYOUT.width - STAGE.width;
}

function atRight(place) {
  return { ...place, x: place.x + wider() };
}

function atMiddle(place) {
  return { ...place, x: place.x + wider() / 2 };
}

function dockPlaces() {
  const { tiles } = DOCK;
  const room = LAYOUT.dock.width - tiles.x - 12 - tiles.gap * (TILE_COUNT - 1);
  return {
    ...DOCK,
    search: atRight(DOCK.search),
    pager: atRight(DOCK.pager),
    tiles: { ...tiles, width: Math.floor(room / TILE_COUNT) },
  };
}

function savesPlaces() {
  return { ...SAVES, reset: atRight(SAVES.reset), buttons: SAVES.buttons.map(atRight) };
}

function aiCopy() {
  return atRight(AI.copy);
}

// Drawing commands of a rectangle, filled, with an outline when `line` is given.
function box(x, y, width, height, fill, alpha = 100, line = null) {
  return [
    line === null ? ['lineStyle'] : ['lineStyle', 1, line, 100],
    ['beginFill', fill, alpha],
    ['moveTo', x, y],
    ['lineTo', x + width, y],
    ['lineTo', x + width, y + height],
    ['lineTo', x, y + height],
    ['lineTo', x, y],
    ['endFill'],
  ];
}

// A one-pixel line across or down.
function rule(x, y, width, height) {
  return box(x, y, width, height, COLORS.border);
}

// A tab bar: the underline under the chosen tab and a rule under the bar.
function tabBar(width, height, tabs, chosen) {
  const commands = [...rule(0, height - 1, width, 1)];
  const tab = tabs[chosen];
  if (tab) {
    commands.push(...box(tab.x, height - 3, tab.width, 2, COLORS.accent));
  }
  return commands;
}

// A switch: its track and knob.
function toggle(x, y, on) {
  return [
    ...box(x, y, 34, 18, on ? COLORS.accent : 0x3a3f46),
    ...box(on ? x + 18 : x + 2, y + 2, 14, 14, 0xffffff),
  ];
}

// An icon's commands moved to (x, y) (the content tiles' large icons).
function placed(glyph, x, y) {
  return glyph
    .filter((command) => command[0] !== 'clear')
    .map((command) => {
      if (command[0] === 'moveTo' || command[0] === 'lineTo') {
        return [command[0], command[1] + x, command[2] + y];
      }
      return command;
    });
}

function inside(px, py, x, y, width, height) {
  return px >= x && px < x + width && py >= y && py < y + height;
}

// The open map list's box on the stage for `count` maps, at most `most` rows at once: { x, y,
// width, height, rows }.
function mapListBox(count, most = MAP_LIST.most) {
  const button = atMiddle(MAP_BUTTON);
  const rows = Math.min(count, most);
  return {
    x: button.x + Math.round((button.width - MAP_LIST.width) / 2),
    y: LAYOUT.menu.height + MAP_LIST.top,
    width: MAP_LIST.width,
    height: rows * MAP_LIST.row + 2 * MAP_LIST.pad,
    rows,
  };
}

// Whether a text run ({ x, y, w, h }) reaches into a box ({ x, y, width, height }).
function overlaps(run, area) {
  return (
    run.x < area.x + area.width &&
    run.x + run.w > area.x &&
    run.y < area.y + area.height &&
    run.y + run.h > area.y
  );
}

// A details row that only names a section.
function isSection(row) {
  return row.value === undefined && typeof row.toggle !== 'boolean';
}

class Shell {
  // root: the screen's root clip. handlers: { view(x, y), camSpeed(), tool(i), snap(i), mode(i),
  // undo(), redo(), play(), outlinerSearch(), outlinerFilter(), outliner(row),
  // outlinerPage(step), detail(row, side), tab(i), folder(i), contentSearch(), tile(i),
  // page(step) }. glyphs: the content tiles' icons by name (glyphs.cjs). send: a message to the
  // game client (the overlay's text and pictures).
  //
  // dockRenderer (optional): draws the content tab itself when the view's dock has a `custom`
  // part, as dockRenderer(shell, custom, commands) with the dock's own drawing commands; its
  // presses and releases then go to handlers.dockPress(x, y) and handlers.dockRelease(x, y)
  // (places in the dock). The studio's timeline is one (Client/resources/studio/timeline.cjs).
  constructor(root, handlers, glyphs, send, dockRenderer = null) {
    this.root = root;
    this.handlers = handlers;
    this.glyphs = glyphs;
    this.send = send;
    this.dockRenderer = dockRenderer;
    this.customDock = false;
    // The first Details row shown (scrolled with the footer's buttons and the mouse wheel).
    this.detailFrom = 0;
    // What each panel draws now, and the text sent last, so only changes are sent.
    this.shown = new Map();
    // The panels that take presses, and the last press routed (one click heard by two clips is
    // one press).
    this.pressAreas = [];
    this.lastPress = null;
    // A panel's edge being dragged: { edge, from, sizes } (followSplit), else null.
    this.split = null;
    this.sentText = '';
    this.sentPictures = '';
    // The clip all panels are in, scaled so the editor's stage fills the window with square
    // pixels (placePanels), and each panel with its area.
    this.stageClip = null;
    this.panels = [];
    // The open map list: its clip covers the stage under the menu bar and takes presses only while
    // the list is open (a press beside the list closes it); the first map row shown and how many
    // maps it lists, and how many rows it shows at once (fewer only in a kit test: 'maplistrows').
    this.mapListArea = { x: 0, y: LAYOUT.menu.height, width: LAYOUT.width, height: 0 };
    this.mapListFrom = 0;
    this.mapCount = 0;
    this.mapListMost = MAP_LIST.most;
  }

  // A panel: one clip at the area's place, its press handler kept with its area. A press is taken
  // as a place on the stage and given to the panel whose area holds it: the game hands a press to
  // the world view's clip even below it, over the Content dock (the press log), so
  // the clip that heard it does not say whose it is. `pressed(panel, px, py, ms)`, when set, hears
  // of each press and how long it took to handle. A release (the end of a drag, the Studio's)
  // goes to the drag handlers wherever it is (routeRelease), for a panel that takes releases.
  panel(area, onPress, takesReleases = false) {
    const clip = this.stageClip.createClip();
    clip.set({ x: area.x, y: area.y });
    this.panels.push({ clip, area });
    const heard = (route) => () => {
      const started = Date.now();
      const px = clip.get('_xmouse');
      const py = clip.get('_ymouse');
      if (typeof px === 'number' && typeof py === 'number') {
        route(area.x + px, area.y + py, started);
      } else {
        // The game did not say where the mouse is: the press is lost, and the log says so.
        this.pressed?.(`${area.x},${area.y} without a place`, 0, 0, Date.now() - started);
      }
    };
    if (onPress) {
      this.pressAreas.push({ area, onPress });
      clip.on(
        'press',
        heard((x, y, started) => this.routePress(x, y, started)),
      );
    }
    if (takesReleases) {
      const release = heard((x, y, started) => this.routeRelease(x, y, started));
      clip.on('release', release);
      clip.on('releaseOutside', release);
    }
    return clip;
  }

  // A press at stage point (x, y) to the panel whose area holds it; one click heard by two clips
  // is one press.
  routePress(x, y, started) {
    const last = this.lastPress;
    if (last && last.x === x && last.y === y && started - last.at < SAME_PRESS_MS) {
      return;
    }
    this.lastPress = { x, y, at: started };
    const edge = this.mapCount === 0 ? edgeAt(x, y) : null;
    if (edge) {
      this.split = { edge, from: { x, y }, sizes: paneSizes() };
      return;
    }
    const target = this.pressAreas.find(({ area }) => {
      if (area === this.mapListArea && this.mapCount === 0) {
        return false;
      }
      return x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height;
    });
    if (!target) {
      // On no panel (a gap between them): the log still hears of it.
      this.pressed?.('outside the panels', x, y, Date.now() - started);
      return;
    }
    const { area, onPress } = target;
    onPress(x - area.x, y - area.y);
    this.pressed?.(`${area.x},${area.y}`, x - area.x, y - area.y, Date.now() - started);
  }

  // A panel's edge dragged (a press on it in routePress): each poll of the editor while the mouse
  // button is `down` moves the edge to the pointer, and the panels go to their new places. True
  // while an edge is dragged (the editor then leaves the press alone).
  followSplit(down) {
    const split = this.split;
    if (!split) {
      return false;
    }
    if (!down) {
      this.split = null;
      return true;
    }
    const pointer = this.viewPointer();
    if (!pointer) {
      return true;
    }
    const dx = pointer.x - split.from.x;
    const dy = pointer.y - split.from.y;
    const change = {};
    if (split.edge === 'outliner') {
      change.outliner = split.sizes.outliner + dx;
    } else if (split.edge === 'details') {
      change.details = split.sizes.details - dx;
    } else {
      change.dock = split.sizes.dock - dy;
    }
    if (setPanes(change)) {
      this.panesMoved();
    }
    return true;
  }

  // The panels' sizes changed: each goes to its place and the editor draws its screen again
  // (how many rows fit changed), or this shell draws the last screen again.
  panesMoved() {
    this.placePanels();
    if (this.handlers.relayout) {
      this.handlers.relayout();
    } else if (this.lastView) {
      this.render(this.lastView);
    }
  }

  // A release at stage point (x, y): the world view's drag (a gizmo's ring) and a custom dock's
  // drag (the studio's timeline) end there, wherever it is; one heard twice is one.
  routeRelease(x, y, started) {
    const last = this.lastRelease;
    if (last && last.x === x && last.y === y && started - last.at < SAME_PRESS_MS) {
      return;
    }
    this.lastRelease = { x, y, at: started };
    this.handlers.viewRelease?.(x, y);
    if (this.customDock) {
      const { dock } = LAYOUT;
      this.handlers.dockRelease?.(x - dock.x, y - dock.y);
    }
  }

  // Where the pointer is on the stage now, through the world's view (a drag in it follows the
  // pointer); null before the screen stands.
  viewPointer() {
    const px = this.view?.get('_xmouse');
    const py = this.view?.get('_ymouse');
    const { view } = LAYOUT;
    return typeof px === 'number' && typeof py === 'number'
      ? { x: view.x + px, y: view.y + py }
      : null;
  }

  // Where the pointer is in the Details panel now (a value dragged sideways follows it); null
  // before the screen stands.
  detailsPointer() {
    const px = this.details?.get('_xmouse');
    const py = this.details?.get('_ymouse');
    return typeof px === 'number' && typeof py === 'number' ? { x: px, y: py } : null;
  }

  // Where the pointer is in the dock now (a drag in a custom dock follows it); null before the
  // screen stands.
  dockPointer() {
    const px = this.dock?.get('_xmouse');
    const py = this.dock?.get('_ymouse');
    return typeof px === 'number' && typeof py === 'number' ? { x: px, y: py } : null;
  }

  build() {
    const { menu, toolbar, outliner, details, dock, status, view } = LAYOUT;
    this.stageClip = this.root.createClip();
    this.lastRelease = null;
    this.view = this.panel(view, (px, py) => this.pressView(px, py), true);
    // The terrain brush's ring, above the world view and below every panel (they cover it).
    this.ring = this.panel(view, null);
    this.menu = this.panel(menu, (px, py) => this.pressMenu(px, py));
    this.toolbar = this.panel(toolbar, (px, py) => this.pressToolbar(px, py));
    this.outliner = this.panel(outliner, (px, py) => this.pressOutliner(px, py));
    this.details = this.panel(details, (px, py) => this.pressDetails(px, py));
    this.dock = this.panel(dock, (px, py) => this.pressDock(px, py), true);
    this.status = this.panel(status, null);
    // The map list above every panel, and asked first for a press while it is open.
    this.fitMapListArea();
    this.mapList = this.panel(this.mapListArea, (px, py) => this.pressMapList(px, py));
    this.pressAreas.unshift(this.pressAreas.pop());
    this.placePanels();
  }

  // The map list's clip covers the stage under the menu bar.
  fitMapListArea() {
    Object.assign(this.mapListArea, {
      y: LAYOUT.menu.height,
      width: LAYOUT.width,
      height: LAYOUT.height - LAYOUT.menu.height,
    });
  }

  // Shows or hides every panel (a picture of the world without them, ai_commands.cjs screenshot).
  setShown(shown) {
    this.stageClip?.set({ visible: shown });
  }

  // The editor's stage changed (theme.cjs fitStage): the game stretches its 1920 by 1080 stage over
  // the window, so the panels' clip is scaled back by the editor's stage, and each panel goes to
  // its area's place. The next render draws them at their new sizes.
  placePanels() {
    if (!this.stageClip) {
      return;
    }
    this.stageClip.set({
      scaleX: STAGE.width / LAYOUT.width,
      scaleY: STAGE.height / LAYOUT.height,
    });
    this.fitMapListArea();
    for (const { clip, area } of this.panels) {
      clip.set({ x: area.x, y: area.y });
    }
  }

  // Lines on the stage ({ from, to, color, width }): the marks and the brush ring, drawn in the
  // layer above the world view and below the panels, each cut to the view.
  drawLines(lines) {
    if (!this.ring) {
      return;
    }
    const { view } = LAYOUT;
    const commands = [];
    for (const line of lines) {
      const cut = clipToView(line.from, line.to, view);
      if (!cut) {
        continue;
      }
      commands.push(
        ['lineStyle', line.width, line.color, 100],
        ['moveTo', Math.round(cut[0].x - view.x), Math.round(cut[0].y - view.y)],
        ['lineTo', Math.round(cut[1].x - view.x), Math.round(cut[1].y - view.y)],
      );
    }
    this.paint(this.ring, commands.slice(0, MOST_LINE_COMMANDS));
  }

  // The editor closed: the overlay draws nothing.
  clear() {
    this.send({ op: 'architectClear' });
    this.sentText = '';
    this.sentPictures = '';
  }

  // Nothing of the editor drawn (a clean view to watch or record the scene): every panel empty
  // and the overlay's texts and pictures gone. What each panel drew is forgotten, so the next
  // render draws them all again.
  hide() {
    for (const clip of [
      this.view,
      this.ring,
      this.menu,
      this.toolbar,
      this.outliner,
      this.details,
      this.dock,
      this.status,
    ]) {
      clip.draw([['clear']]);
    }
    this.shown.clear();
    this.clear();
  }

  // Sends a panel's drawing when it changed. A clip takes at most 1024 commands a call (more threw,
  // and the panel lost its backgrounds: owner, Details grew past it): a long drawing
  // goes in parts, the first clearing the clip, each part starting with its own moveTo.
  paint(clip, commands) {
    const key = JSON.stringify(commands);
    if (this.shown.get(clip) === key) {
      return;
    }
    this.shown.set(clip, key);
    let start = 0;
    do {
      let end = Math.min(commands.length, start + DRAW_PART);
      // A part ends before a moveTo or beginFill, so no shape is cut in two.
      while (end < commands.length && end > start + 1 && !SHAPE_STARTS.has(commands[end][0])) {
        end -= 1;
      }
      const part = commands.slice(start, end);
      clip.draw(start === 0 ? [['clear'], ...part] : part);
      start = end;
    } while (start < commands.length);
  }

  // A line of text in a box of the stage: style { size, color, align ('l', 'c', 'r'), bold }.
  text(x, y, width, height, value, style = {}) {
    if (value === '' || value === undefined || value === null) {
      return;
    }
    this.runs.push({
      x,
      y,
      w: width,
      h: height,
      t: String(value),
      s: style.size ?? TEXT.body,
      c: style.color ?? COLORS.text,
      a: style.align ?? 'l',
      b: Boolean(style.bold),
    });
  }

  // ---- Presses ----

  pressView(px, py) {
    if (inside(px, py, 8, 8, 170, 30)) {
      this.handlers.camSpeed();
      return;
    }
    const { view } = LAYOUT;
    this.handlers.view(view.x + px, view.y + py);
  }

  pressToolbar(px, py) {
    const { handlers } = this;
    for (let index = 0; index < 4; index += 1) {
      const x = TOOL.x + index * (TOOL.width + TOOL.gap);
      if (inside(px, py, x, TOOL.y, TOOL.width, TOOL.height)) {
        handlers.tool(index);
        return;
      }
    }
    for (const [index, snap] of SNAPS.entries()) {
      if (inside(px, py, snap.x, 7, snap.width, 34)) {
        handlers.snap(index);
        return;
      }
    }
    const modes = this.modeBar ?? modeBar(SNAPS.length - 1, MODE.count);
    if (px >= modes.x && px < modes.x + modes.count * modes.width) {
      handlers.mode(Math.floor((px - modes.x) / modes.width));
      return;
    }
    const undo = atRight(UNDO);
    const redo = atRight(REDO);
    const play = atRight(PLAY);
    if (inside(px, py, undo.x, 7, undo.width, 34)) {
      handlers.undo();
    } else if (inside(px, py, redo.x, 7, redo.width, 34)) {
      handlers.redo();
    } else if (inside(px, py, play.x, 7, play.width, 34)) {
      handlers.play();
    }
  }

  pressOutliner(px, py) {
    const { handlers } = this;
    const { search, filter, back, next } = outlinerPlaces();
    if (inside(px, py, search.x, search.y, search.width, search.height)) {
      handlers.outlinerSearch();
      return;
    }
    if (inside(px, py, filter.x, filter.y, filter.width, filter.height)) {
      handlers.outlinerFilter();
      return;
    }
    if (py >= outlinerFoot()) {
      if (px >= back && px < next - 6) {
        handlers.outlinerPage(-1);
      } else if (px >= next - 6) {
        handlers.outlinerPage(1);
      }
      return;
    }
    const row = Math.floor((py - OUTLINER.rows) / OUTLINER.row);
    if (py >= OUTLINER.rows && row < outlinerRows()) {
      handlers.outliner(row);
    }
  }

  // The Saves tab: the name field (typing), Save as, and a row's Load, Merge or Delete. An editor
  // without one of them (the Asset/Animation Studio has no Merge) leaves its handler out.
  pressSaves(px, py) {
    const { handlers } = this;
    const { field, saveAs, reset, buttons } = savesPlaces();
    if (inside(px, py, field.x, DOCK.top, field.width, 30)) {
      handlers.saveName?.();
      return;
    }
    if (inside(px, py, saveAs.x, DOCK.top, saveAs.width, 30)) {
      handlers.saveAs?.();
      return;
    }
    if (inside(px, py, reset.x, DOCK.top, reset.width, 30)) {
      handlers.resetMap?.();
      return;
    }
    const row = Math.floor((py - ISSUE.top) / ISSUE.row);
    if (py < ISSUE.top || row >= ISSUE_ROWS) {
      return;
    }
    const button = buttons.find((entry) => px >= entry.x && px < entry.x + entry.width);
    if (button) {
      handlers.saveRow?.(row, button.action);
    }
  }

  pressDetails(px, py) {
    const total = this.detailTotal ?? 0;
    const fit = detailRows(total);
    if (total > fit && py >= detailFoot()) {
      // The scroll footer: up a page, down a page.
      const width = LAYOUT.details.width;
      if (px >= width - 80 && px < width - 44) {
        this.scrollDetails(-fit);
      } else if (px >= width - 44) {
        this.scrollDetails(fit);
      }
      return;
    }
    const row = Math.floor((py - DETAILS.rows) / DETAILS.row);
    if (py < DETAILS.rows || row >= fit) {
      return;
    }
    const field = detailField();
    let side = 0;
    if (px >= field.x && px < field.x + 30) {
      side = -1;
    } else if (px >= field.x + field.width - 30 && px < field.x + field.width + 10) {
      side = 1;
    }
    this.handlers.detail(this.detailFrom + row, side);
  }

  // Scrolls the Details rows by `step` rows and draws them again.
  scrollDetails(step) {
    const total = this.detailTotal ?? 0;
    this.detailFrom = detailStart(this.detailFrom + step, total);
    if (this.lastView) {
      this.render(this.lastView);
    }
  }

  pressDock(px, py) {
    const { handlers } = this;
    if (py < DOCK.tabs) {
      const tab = Math.floor((px - TAB.x) / TAB.width);
      if (px >= TAB.x && tab >= 0 && tab < (this.dockTabs ?? ARCHITECT_TABS)) {
        // A tab of a folded dock opens the dock again too.
        if (dockFolded() && setPanes({ dock: PANE_SIZES.dock })) {
          this.panesMoved();
        }
        handlers.tab(tab);
      }
      return;
    }
    if (dockFolded()) {
      return;
    }
    if (this.customDock) {
      handlers.dockPress?.(px, py);
      return;
    }
    if (this.savesTab) {
      if (px >= DOCK.nav) {
        this.pressSaves(px, py);
      }
      return;
    }
    if (this.ownAiTab) {
      this.pressAi(px, py);
      return;
    }
    if (px < DOCK.nav) {
      if (this.dockTab >= 3) {
        return;
      }
      const row = Math.floor((py - DOCK.folders) / DOCK.folderRow);
      if (py >= DOCK.folders && row < FOLDER_ROWS) {
        handlers.folder(row);
      }
      return;
    }
    if (this.dockTab === 3) {
      this.pressSaves(px, py);
      return;
    }
    if (this.dockTab === 4) {
      this.pressAi(px, py);
      return;
    }
    if (py < DOCK.tiles.y - 4) {
      const { search, pager } = dockPlaces();
      if (px >= search.x && px < search.x + search.width) {
        handlers.contentSearch();
      } else if (px >= pager.x && px < pager.x + 120) {
        handlers.page(-1);
      } else if (px >= pager.x + 250) {
        handlers.page(1);
      }
      return;
    }
    const { tiles } = dockPlaces();
    if (this.dockTab === 2) {
      const row = Math.floor((py - ISSUE.top) / ISSUE.row);
      if (py >= ISSUE.top && row < ISSUE_ROWS && px >= tiles.x) {
        handlers.issue(row);
      }
      return;
    }
    const index = Math.floor((px - tiles.x) / (tiles.width + tiles.gap));
    if (py < tiles.y + tiles.height && px >= tiles.x && index < TILE_COUNT) {
      handlers.tile(index);
    }
  }

  // ---- Drawing ----

  // view: { camera, menu, toolbar, outliner, details, dock, status } (editor.cjs view()).
  render(view) {
    // Kept so a scroll draws the same screen again (wheel, the Details footer).
    this.lastView = view;
    this.runs = [];
    this.pictures = [];
    this.renderView(view.camera);
    this.renderMenu(view.menu);
    this.renderToolbar(view.toolbar);
    if (wideView()) {
      // The side panels and the dock are put away: nothing of them is drawn.
      for (const clip of [this.outliner, this.details, this.dock]) {
        this.paint(clip, []);
      }
    } else {
      this.renderOutliner(view.outliner);
      this.renderDetails(view.details);
      this.renderDock(view.dock);
    }
    this.renderStatus(view.status);
    // Last: the open map list covers the panels under it, their text too.
    this.renderMapList(view.menu);
    // The overlay draws the text on the editor's stage, as large as the window.
    const stage = { width: LAYOUT.width, height: LAYOUT.height };
    const text = JSON.stringify([stage, this.runs]);
    if (text !== this.sentText) {
      this.sentText = text;
      this.send({ op: 'architectText', stage, runs: this.runs });
    }
    const pictures = JSON.stringify(this.pictures);
    if (pictures !== this.sentPictures) {
      this.sentPictures = pictures;
      this.send({ op: 'architectPictures', items: this.pictures });
    }
  }

  renderView(camera) {
    const area = LAYOUT.view;
    const bottom = area.height - 34;
    // The world's axes as the camera sees them (camera.axes), or a fixed mark without a camera.
    const seen = camera.axes ?? {
      x: { x: 0.9, y: 0.35 },
      y: { x: -0.9, y: 0.35 },
      z: { x: 0, y: -1 },
    };
    const axes = [];
    for (const name of ['z', 'x', 'y']) {
      axes.push(
        ['lineStyle', 2, AXES[name], 100],
        ['moveTo', 40, bottom],
        [
          'lineTo',
          Math.round(40 + seen[name].x * AXIS_MARK),
          Math.round(bottom + seen[name].y * AXIS_MARK),
        ],
      );
    }
    const commands = [
      ...box(0, 0, area.width, area.height, 0x000000, 0),
      ...box(8, 8, 170, 30, FIELD, 78),
      ...axes,
      ...edgeGrips(area),
    ];
    // The name of what is under the cursor, in a box beside it, kept inside the view.
    const hover = camera.hover;
    if (hover) {
      const width = Math.min(
        HOVER.most,
        HOVER.pad * 2 + Math.round(Math.max(hover.label.length, 12) * 9.5),
      );
      const left = Math.min(hover.x - area.x + HOVER.right, area.width - width - 4);
      const top = Math.min(hover.y - area.y + HOVER.down, area.height - HOVER.height - 4);
      commands.push(...box(left, top, width, HOVER.height, FIELD, 92, COLORS.border));
      this.text(
        area.x + left + HOVER.pad,
        area.y + top + 2,
        width - HOVER.pad * 2,
        22,
        hover.label,
        {
          bold: true,
        },
      );
      this.text(
        area.x + left + HOVER.pad,
        area.y + top + 22,
        width - HOVER.pad * 2,
        18,
        hover.kind,
        {
          size: TEXT.small,
          color: COLORS.dim,
        },
      );
    }
    this.paint(this.view, commands);
    this.text(area.x + 18, area.y + 8, 160, 30, `Cam speed ${camera.speed}`, {
      size: TEXT.small,
    });
  }

  // The menu bar: the editor's name, the map in the middle (a press offers the other maps, and a
  // switch asks first) and whether the work is saved at the right end.
  renderMenu(menu) {
    const area = LAYOUT.menu;
    this.menuPicker = menu.picker;
    const commands = [
      ...box(0, 0, area.width, area.height, COLORS.bar, 97),
      ...rule(0, area.height - 1, area.width, 1),
      ...rule(166, 9, 1, 18),
    ];
    // The editor's name, a button that opens the Saves tab (save, load and merge setups) and closes
    // it again: World Architect's, unless the editor names itself (the Asset/Animation Studio shares
    // this screen). A folder marks it, not a dropdown's arrow: it opens no list.
    commands.push(...box(ARCHITECT.x, 5, ARCHITECT.width, 26, BUTTON, 100, COLORS.border));
    commands.push(...icon('folder', ARCHITECT.x + ARCHITECT.width - 17, 18, 12, COLORS.dim));
    this.text(ARCHITECT.x + 10, 5, ARCHITECT.width - 30, 26, menu.title ?? 'World Architect', {
      size: TEXT.title,
      bold: true,
    });
    if (menu.picker === undefined) {
      // An editor without a map picker (the Asset/Animation Studio): nothing more here.
    } else if (menu.picker === null || menu.picker === 'choose') {
      // The map button; while its list is open (renderMapList) it is marked, and a press on it
      // closes the list.
      const open = menu.picker === 'choose';
      const { x, width } = atMiddle(MAP_BUTTON);
      const fill = open ? CHIP_ON : BUTTON;
      commands.push(...box(x, 5, width, 26, fill, 100, open ? CHIP_ON_LINE : COLORS.border));
      commands.push(...icon(open ? 'up' : 'down', x + width - 18, 18, 12, COLORS.dim));
      commands.push(...icon('map', x + 20, 18, 14, COLORS.dim));
      this.text(x + 36, 5, width - 60, 26, menu.mapLabel, {
        bold: true,
        color: open ? COLORS.accentText : COLORS.text,
      });
    } else {
      const ask = atMiddle(MAP_CONFIRM.ask);
      const yes = atMiddle(MAP_CONFIRM.yes);
      const no = atMiddle(MAP_CONFIRM.no);
      this.text(ask.x, 5, ask.width, 26, menu.question, { size: TEXT.small, align: 'r' });
      commands.push(...box(yes.x, 5, yes.width, 26, COLORS.accent, 100));
      this.text(yes.x, 5, yes.width, 26, menu.yes ?? 'Switch', {
        size: TEXT.small,
        color: 0x1a1408,
        bold: true,
        align: 'c',
      });
      commands.push(...box(no.x, 5, no.width, 26, FIELD, 100, COLORS.border));
      this.text(no.x, 5, no.width, 26, 'Cancel', {
        size: TEXT.small,
        color: COLORS.dim,
        align: 'c',
      });
    }
    const saved = menu.unsaved === 0;
    const { x, width } = atRight(SAVE_CHIP);
    commands.push(...box(x, 7, width, 22, saved ? FIELD : CHIP_ON, 100, COLORS.border));
    commands.push(...box(x + 12, 14, 8, 8, saved ? GREEN : COLORS.accent));
    this.text(x + 28, 7, width - 36, 22, menu.saved, {
      size: TEXT.small,
      color: saved ? COLORS.dim : COLORS.accentText,
    });
    const leave = atRight(LEAVE);
    commands.push(...box(leave.x, 5, leave.width, 26, BUTTON, 100, COLORS.border));
    this.text(leave.x, 5, leave.width, 26, 'Leave', { bold: true, align: 'c' });
    this.paint(this.menu, commands);
  }

  pressMenu(px, py) {
    const { handlers } = this;
    if (py < 5 || py > 31) {
      return;
    }
    // The editor's name button opens the Saves tab.
    if (inside(px, py, ARCHITECT.x, 5, ARCHITECT.width, 26)) {
      handlers.architect?.();
      return;
    }
    // Leave: the work saved, back to the main menu and the server list.
    const leave = atRight(LEAVE);
    if (inside(px, py, leave.x, 5, leave.width, 26)) {
      handlers.leave?.();
      return;
    }
    // An editor without a map picker (the Asset/Animation Studio) has no more buttons here.
    if (this.menuPicker === undefined) {
      return;
    }
    if (this.menuPicker === null) {
      const button = atMiddle(MAP_BUTTON);
      if (inside(px, py, button.x, 5, button.width, 26)) {
        handlers.mapPicker('choose');
      }
    } else if (this.menuPicker === 'choose') {
      // The list is open: a press anywhere on the bar (the map button too) closes it.
      handlers.mapPicker(null);
    } else {
      const yes = atMiddle(MAP_CONFIRM.yes);
      const no = atMiddle(MAP_CONFIRM.no);
      if (inside(px, py, yes.x, 5, yes.width, 26)) {
        handlers.mapSwitch();
      } else if (inside(px, py, no.x, 5, no.width, 26)) {
        handlers.mapPicker(null);
      }
    }
  }

  // The open map list under the map button: the maps from mapListFrom on, the current one marked,
  // and a scroll bar when they do not all fit. The overlay draws every text above the panels, so
  // the text of the panels under the list is left out while it is open.
  renderMapList(menu) {
    if (!this.mapList) {
      return;
    }
    const maps = menu.picker === 'choose' && Array.isArray(menu.maps) ? menu.maps : [];
    if (maps.length === 0) {
      this.mapCount = 0;
      this.paint(this.mapList, []);
      return;
    }
    const list = mapListBox(maps.length, this.mapListMost);
    if (this.mapCount === 0) {
      // Just opened: the current map in sight.
      const current = Math.max(
        0,
        maps.findIndex((map) => map.current),
      );
      this.mapListFrom = current - Math.floor(list.rows / 2);
    }
    this.mapCount = maps.length;
    this.mapListFrom = Math.max(0, Math.min(this.mapListFrom, maps.length - list.rows));
    this.runs = this.runs.filter((run) => !overlaps(run, list));
    const { pad, row, bar } = MAP_LIST;
    const top = list.y - this.mapListArea.y;
    const scrolls = maps.length > list.rows;
    const rowWidth = list.width - 2 * pad - (scrolls ? bar + 4 : 0);
    const commands = [
      ...box(list.x, top, list.width, list.height, COLORS.panel, 100, COLORS.border),
    ];
    maps.slice(this.mapListFrom, this.mapListFrom + list.rows).forEach((map, index) => {
      const y = pad + index * row;
      if (map.current) {
        commands.push(...box(list.x + pad, top + y, rowWidth, row, CHIP_ON, 100, CHIP_ON_LINE));
      }
      this.text(list.x + pad + 12, list.y + y, rowWidth - 24, row, map.label, {
        size: TEXT.small,
        color: map.current ? COLORS.accentText : COLORS.text,
        bold: map.current,
      });
    });
    if (scrolls) {
      // The bar's track and its thumb: where the shown rows are among all the maps.
      const trackX = list.x + list.width - pad - bar;
      const trackHeight = list.height - 2 * pad;
      const thumbHeight = Math.max(16, Math.round((trackHeight * list.rows) / maps.length));
      const thumbY = Math.round(
        ((trackHeight - thumbHeight) * this.mapListFrom) / (maps.length - list.rows),
      );
      commands.push(...box(trackX, top + pad, bar, trackHeight, FIELD));
      commands.push(...box(trackX, top + pad + thumbY, bar, thumbHeight, COLORS.dim));
    }
    this.paint(this.mapList, commands);
  }

  // A press while the map list is open (the place from the list clip's area): a map row chooses
  // that map (the switch asks first), the scroll bar pages the list, and a press beside the list
  // closes it.
  pressMapList(px, py) {
    const x = px + this.mapListArea.x;
    const y = py + this.mapListArea.y;
    const list = mapListBox(this.mapCount, this.mapListMost);
    if (!inside(x, y, list.x, list.y, list.width, list.height)) {
      this.handlers.mapPicker(null);
      return;
    }
    const scrolls = this.mapCount > list.rows;
    if (scrolls && x >= list.x + list.width - MAP_LIST.pad - MAP_LIST.bar - 4) {
      // Above the middle of the bar a page up, below it a page down.
      this.scrollMapList(y < list.y + list.height / 2 ? -list.rows : list.rows);
      return;
    }
    const row = Math.floor((y - list.y - MAP_LIST.pad) / MAP_LIST.row);
    if (row >= 0 && row < list.rows && this.mapListFrom + row < this.mapCount) {
      this.handlers.mapChoose(this.mapListFrom + row);
    }
  }

  // Scrolls the open map list by `step` rows and draws it again.
  scrollMapList(step) {
    const before = this.mapListFrom;
    const rows = mapListBox(this.mapCount, this.mapListMost).rows;
    this.mapListFrom = Math.max(0, Math.min(this.mapListFrom + step, this.mapCount - rows));
    if (this.mapListFrom !== before && this.lastView) {
      this.render(this.lastView);
    }
    return this.mapListFrom !== before;
  }

  renderToolbar(toolbar) {
    const area = LAYOUT.toolbar;
    const top = area.y;
    const commands = [
      ...box(0, 0, area.width, area.height, COLORS.panel, 97),
      ...rule(0, area.height - 1, area.width, 1),
      ...box(TOOL.x - 2, 5, 4 * (TOOL.width + TOOL.gap) + 1, 38, FIELD),
    ];
    toolbar.tools.forEach((name, index) => {
      const x = TOOL.x + index * (TOOL.width + TOOL.gap);
      const chosen = index === toolbar.tool;
      if (chosen) {
        commands.push(...box(x, TOOL.y, TOOL.width, TOOL.height, COLORS.chosen));
      }
      const color = chosen ? COLORS.accentText : ICON;
      commands.push(...icon(name, x + TOOL.width / 2, TOOL.y + TOOL.height / 2, 19, color));
    });
    commands.push(...rule(176, 12, 1, 26), ...icon('magnet', 196, 24, 17, COLORS.faint));
    toolbar.snaps.forEach((snap, index) => {
      const { x, width } = SNAPS[index];
      const fill = snap.on ? CHIP_ON : BUTTON;
      const line = snap.on ? CHIP_ON_LINE : COLORS.border;
      commands.push(...box(x, 8, width, 32, fill, 100, line));
      const color = snap.on ? COLORS.accentText : COLORS.dim;
      this.text(x, top + 8, width, 32, snap.label, { size: TEXT.small, color, align: 'c' });
    });
    // The presses use the same places (pressToolbar).
    const modes = modeBar(toolbar.snaps.length, toolbar.modes.length);
    this.modeBar = modes;
    commands.push(...box(modes.x - 2, 5, modes.count * modes.width + 4, 38, FIELD));
    toolbar.modes.forEach((mode, index) => {
      const x = modes.x + index * modes.width;
      const chosen = index === toolbar.mode;
      if (chosen) {
        commands.push(...box(x, MODE.y + 1, modes.width, MODE.height - 2, COLORS.chosen));
      }
      let color = chosen ? COLORS.accentText : ICON;
      if (mode.later) {
        color = 0x5b6169;
      }
      commands.push(...icon(mode.icon, x + 22, MODE.y + MODE.height / 2, 18, color));
      const label = chosen ? COLORS.accentText : mode.later ? 0x5b6169 : COLORS.text;
      this.text(x + 38, top + MODE.y, modes.width - 40, MODE.height, mode.label, {
        color: label,
        bold: chosen,
      });
    });
    const undo = atRight(UNDO);
    const redo = atRight(REDO);
    const play = atRight(PLAY);
    commands.push(
      ...icon('undo', undo.x + 18, 24, 19, ICON),
      ...icon('redo', redo.x + 18, 24, 19, ICON),
      ...rule(1690 + wider(), 12, 1, 26),
      ...box(play.x, 8, play.width, 32, BUTTON, 100, COLORS.border),
      ...icon('play', play.x + 22, 24, 16, 0x6cc46a),
    );
    this.paint(this.toolbar, commands);
    // The editor's own word for it (the Asset/Animation Studio's Play), or World Architect's.
    this.text(play.x + 40, top + 8, play.width - 48, 32, toolbar.play ?? 'Play as player');
  }

  renderOutliner(outliner) {
    const area = LAYOUT.outliner;
    const { x: left, y: top } = area;
    const { search, filter, type, back, next } = outlinerPlaces();
    const searchLine = outliner.typing ? COLORS.accent : COLORS.border;
    const filterFill = outliner.filtered ? CHIP_ON : BUTTON;
    const filterLine = outliner.filtered ? CHIP_ON_LINE : COLORS.border;
    const filterIcon = outliner.filtered ? COLORS.accentText : COLORS.dim;
    const commands = [
      ...box(0, 0, area.width, area.height, COLORS.panel, 95),
      ...rule(area.width - 1, 0, 1, area.height),
      ...tabBar(area.width - 1, OUTLINER.tabs, [{ x: 10, width: 86 }], 0),
      ...box(search.x, search.y, search.width, search.height, FIELD, 100, searchLine),
      ...icon('search', search.x + 15, search.y + 15, 14, COLORS.faint, 2),
      ...box(filter.x, filter.y, filter.width, filter.height, filterFill, 100, filterLine),
      ...icon('filter', filter.x + 16, filter.y + 15, 14, filterIcon, 2),
      ...rule(0, OUTLINER.header + 25, area.width - 1, 1),
      ...rule(0, outlinerFoot(), area.width - 1, 1),
    ];
    this.text(left + 10, top, 86, OUTLINER.tabs, 'Outliner', { bold: true, align: 'c' });
    const searchColor = outliner.typing || outliner.query ? COLORS.text : COLORS.faint;
    this.text(
      left + search.x + 30,
      top + search.y,
      search.width - 36,
      search.height,
      outliner.search,
      {
        size: TEXT.small,
        color: searchColor,
      },
    );
    const header = { size: TEXT.small, color: COLORS.faint };
    this.text(left + 14, top + OUTLINER.header, 200, 26, 'Name', header);
    this.text(left + type, top + OUTLINER.header, 80, 26, 'Type', header);
    outliner.lines.forEach((line, index) => {
      const y = OUTLINER.rows + index * OUTLINER.row;
      const middle = y + OUTLINER.row / 2;
      const chosen = index === outliner.chosen;
      if (chosen) {
        commands.push(...box(0, y, area.width - 1, OUTLINER.row, COLORS.chosen));
      }
      let nameX = 52;
      if (line.group) {
        const chevron = line.collapsed ? 'right' : 'down';
        commands.push(...icon(chevron, 20, middle, 11, COLORS.faint, 2));
        commands.push(...icon('folder', 36, middle, 15, COLORS.dim));
      } else {
        commands.push(...icon(line.icon, 44, middle, 15, chosen ? COLORS.accentText : 0x8d949c));
        nameX = 60;
      }
      const color = chosen ? 0xffffff : COLORS.text;
      this.text(left + nameX, top + y, type - nameX - 6, OUTLINER.row, line.name, {
        color,
      });
      this.text(left + type, top + y, 80, OUTLINER.row, line.type, {
        size: TEXT.small,
        color: COLORS.faint,
      });
    });
    this.paint(this.outliner, commands);
    const foot = { size: TEXT.small, color: COLORS.faint };
    this.text(left + 14, top + outlinerFoot(), back - 14, 34, outliner.footer, foot);
    const page = OUTLINER_PAGES.width;
    this.text(left + back, top + outlinerFoot(), page, 34, '‹', { ...foot, align: 'c' });
    this.text(left + next, top + outlinerFoot(), page, 34, '›', { ...foot, align: 'c' });
  }

  // The mouse wheel (notches, away from you positive): over the Details panel its list scrolls
  // when it is longer than the panel.
  wheel(notches) {
    // The open map list scrolls first, wherever the cursor is.
    if (this.mapCount > 0) {
      const step = Math.sign(notches) * Math.max(1, Math.round(Math.abs(notches)));
      return notches !== 0 && this.scrollMapList(-step);
    }
    const pointer = this.detailsPointer();
    const area = LAYOUT.details;
    const over =
      pointer &&
      pointer.x >= 0 &&
      pointer.y >= 0 &&
      pointer.x < area.width &&
      pointer.y < area.height;
    if (!over || !this.lastView || notches === 0) {
      return false;
    }
    const before = this.detailFrom;
    // Two rows a notch; the whole screen again, so its text goes to the game in one piece.
    this.scrollDetails(-Math.sign(notches) * Math.max(1, Math.round(Math.abs(notches))) * 2);
    return this.detailFrom !== before;
  }

  renderDetails(details) {
    const area = LAYOUT.details;
    const { x: left, y: top } = area;
    const field = detailField();
    // A new selection (another title or kind) shows its rows from the top; the same one stays
    // scrolled.
    const total = details.rows.length;
    const shown = `${details.title}\0${details.kind}`;
    if (this.detailTitle !== shown) {
      this.detailTitle = shown;
      this.detailFrom = 0;
    }
    this.detailFrom = detailStart(this.detailFrom ?? 0, total);
    this.detailTotal = total;
    const fit = detailRows(total);
    const from = this.detailFrom;
    const commands = [
      ...box(0, 0, area.width, area.height, COLORS.panel, 95),
      ...rule(0, 0, 1, area.height),
      ...tabBar(area.width, DETAILS.tabs, [{ x: 10, width: 78 }], 0),
      ...box(14, DETAILS.head + 2, 44, 44, CHIP_ON),
      ...icon(details.icon, 36, DETAILS.head + 24, 24, COLORS.accentText),
      ...rule(0, DETAILS.rows - 4, area.width, 1),
    ];
    this.text(left + 10, top, 78, DETAILS.tabs, 'Details', { bold: true, align: 'c' });
    this.text(left + 68, top + DETAILS.head, area.width - 80, 26, details.title, {
      size: TEXT.title,
      bold: true,
    });
    this.text(left + 68, top + DETAILS.head + 24, area.width - 80, 22, details.kind, {
      size: TEXT.small,
      color: COLORS.dim,
    });
    details.rows.slice(from, from + fit).forEach((row, index) => {
      const y = DETAILS.rows + index * DETAILS.row;
      if (isSection(row)) {
        commands.push(...box(0, y, area.width, DETAILS.row, COLORS.section));
        commands.push(
          ...icon(row.folded ? 'right' : 'down', 20, y + DETAILS.row / 2, 11, COLORS.faint, 2),
        );
        this.text(left + 34, top + y, area.width - 40, DETAILS.row, row.label, {
          size: TEXT.small,
          color: ICON,
          bold: true,
        });
        return;
      }
      this.text(left + 14, top + y, field.x - 18, DETAILS.row, row.label, { color: COLORS.dim });
      if (typeof row.toggle === 'boolean') {
        commands.push(...toggle(field.x + field.width - 36, y + 7, row.toggle));
        return;
      }
      const height = DETAILS.row - 8;
      commands.push(...box(field.x, y + 4, field.width, height, FIELD, 100, COLORS.border));
      if (row.axis) {
        commands.push(...box(field.x, y + 4, 3, height, AXES[row.axis]));
      }
      const valueX = row.step ? field.x + 30 : field.x + 10;
      const valueWidth = row.step ? field.width - 60 : field.width - 20;
      this.text(left + valueX, top + y, valueWidth, DETAILS.row, row.value, {
        size: TEXT.small,
        align: row.step ? 'c' : 'l',
      });
      if (row.step) {
        commands.push(...rule(field.x + 30, y + 6, 1, height - 4));
        commands.push(...rule(field.x + field.width - 30, y + 6, 1, height - 4));
        const sign = { color: COLORS.accentText, align: 'c', bold: true };
        this.text(left + field.x, top + y, 30, DETAILS.row, '−', sign);
        this.text(left + field.x + field.width - 30, top + y, 30, DETAILS.row, '+', sign);
      }
    });
    if (total > fit) {
      // The scroll bar beside the rows and the footer with its page buttons.
      const track = fit * DETAILS.row;
      const thumb = Math.max(24, Math.round((track * fit) / total));
      const travel = track - thumb;
      const at = DETAILS.rows + Math.round((travel * from) / Math.max(1, total - fit));
      commands.push(...box(area.width - DETAILS.bar - 2, DETAILS.rows, DETAILS.bar, track, FIELD));
      commands.push(...box(area.width - DETAILS.bar - 2, at, DETAILS.bar, thumb, COLORS.border));
      commands.push(...rule(0, detailFoot(), area.width, 1));
      const foot = { size: TEXT.small, color: COLORS.faint };
      const last = Math.min(total, from + fit);
      const shown = `rows ${from + 1}–${last} of ${total}`;
      this.text(left + 14, top + detailFoot(), area.width - 100, DETAILS.foot, shown, foot);
      const arrow = { ...foot, color: COLORS.accentText, bold: true, align: 'c' };
      this.text(left + area.width - 80, top + detailFoot(), 36, DETAILS.foot, '▲', arrow);
      this.text(left + area.width - 44, top + detailFoot(), 36, DETAILS.foot, '▼', arrow);
    }
    this.paint(this.details, commands);
  }

  renderDock(dock) {
    this.dockTab = dock.tab;
    this.dockTabs = dock.tabs.length;
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    const { tiles, search, pager } = dockPlaces();
    const content = dock.tab === 0;
    // A tab named Timeline (the studio's) draws the dock's custom part, as Content may.
    const timelineTab = dock.tabs[dock.tab] === 'Timeline';
    const chipX = tiles.x + dock.crumbWidth + 16;
    const commands = [
      ...box(0, 0, area.width, area.height, COLORS.panel, 97),
      ...rule(0, 0, area.width, 1),
      ...tabBar(
        area.width,
        DOCK.tabs,
        dock.tabs.map((label, index) => ({ x: TAB.x + index * TAB.width, width: TAB.width })),
        dock.tab,
      ),
      ...rule(DOCK.nav, DOCK.tabs, 1, area.height - DOCK.tabs),
    ];
    this.customDock =
      (content || timelineTab) && Boolean(dock.custom) && typeof this.dockRenderer === 'function';
    // The Saves tab (an editor that has saves gives dock.saves).
    this.savesTab = dock.tabs[dock.tab] === 'Saves' && Boolean(dock.saves);
    // An editor's own AI tab, found by its name (World Architect's is its fifth tab, below).
    this.ownAiTab =
      dock.tabs[dock.tab] === 'AI' && Boolean(dock.ai) && typeof this.dockRenderer === 'function';
    dock.tabs.forEach((label, index) => {
      const color = index === dock.tab ? COLORS.text : COLORS.dim;
      this.text(left + TAB.x + index * TAB.width, top, TAB.width, DOCK.tabs, label, {
        color,
        bold: index === dock.tab,
        align: 'c',
      });
    });
    // A dock folded to its tab bar (its top edge dragged down) shows only the tabs.
    if (dockFolded()) {
      this.customDock = false;
      this.savesTab = false;
      this.ownAiTab = false;
      this.paint(this.dock, commands);
      return;
    }
    // An editor with a dock of its own (the Asset/Animation Studio: its timeline, its own tab
    // order) draws it; World Architect's tabs follow below.
    if (this.customDock) {
      this.dockRenderer(this, dock.custom, commands);
      this.paint(this.dock, commands);
      return;
    }
    if (this.savesTab && typeof this.dockRenderer === 'function') {
      this.renderOwnSaves(dock, commands);
      this.paint(this.dock, commands);
      return;
    }
    if (this.ownAiTab) {
      this.renderOwnAi(dock, commands);
      this.paint(this.dock, commands);
      return;
    }
    if (dock.tab >= 3) {
      // Saves and AI have no folders: the column says what the tab is for.
      const about = DOCK_ABOUT[dock.tab];
      this.text(left + 14, top + DOCK.top - 8, 220, 22, about.title, {
        size: 12,
        color: COLORS.faint,
        bold: true,
      });
      about.lines.forEach((line, index) => {
        this.text(left + 14, top + DOCK.folders + index * 22, 230, 22, line, {
          size: TEXT.small,
          color: COLORS.dim,
        });
      });
    } else {
      this.text(left + 14, top + DOCK.top - 8, 200, 22, 'GAME CONTENT', {
        size: 12,
        color: COLORS.faint,
        bold: true,
      });
    }
    if (dock.tab < 3 && dock.folder >= 0) {
      const y = DOCK.folders + dock.folder * DOCK.folderRow;
      commands.push(...box(0, y, DOCK.nav, DOCK.folderRow, COLORS.chosen));
    }
    (dock.tab < 3 ? dock.folders : []).forEach((folder, index) => {
      const y = top + DOCK.folders + index * DOCK.folderRow;
      const chosen = index === dock.folder;
      this.text(left + 22, y, 170, DOCK.folderRow, folder.label, {
        color: chosen ? 0xffffff : COLORS.text,
      });
      this.text(left + 170, y, 70, DOCK.folderRow, folder.count, {
        size: TEXT.small,
        color: COLORS.faint,
        align: 'r',
      });
    });
    if (content) {
      const searchLine = dock.typing ? COLORS.accent : COLORS.border;
      commands.push(
        ...box(chipX, DOCK.top + 1, DOCK.chip.width, 28, CHIP_ON),
        ...box(search.x, DOCK.top, search.width, 30, FIELD, 100, searchLine),
        ...icon('search', search.x + 15, DOCK.top + 15, 14, COLORS.faint, 2),
      );
      this.text(left + tiles.x, top + DOCK.top, 900, 30, dock.crumb, { color: COLORS.dim });
      this.text(left + chipX, top + DOCK.top + 1, DOCK.chip.width, 28, dock.chip, {
        size: TEXT.small,
        color: COLORS.accentText,
        align: 'c',
      });
      const searchColor = dock.typing || dock.query ? COLORS.text : COLORS.faint;
      this.text(left + search.x + 30, top + DOCK.top, search.width - 36, 30, dock.search, {
        size: TEXT.small,
        color: searchColor,
      });
      const pagerStyle = { size: TEXT.small, color: COLORS.dim, align: 'c' };
      this.text(left + pager.x, top + DOCK.top, 120, 30, '‹', pagerStyle);
      this.text(left + pager.x + 120, top + DOCK.top, 130, 30, dock.pager[1], pagerStyle);
      this.text(left + pager.x + 250, top + DOCK.top, 98, 30, '›', pagerStyle);
      dock.tiles.forEach((tile, index) => {
        const x = tiles.x + index * (tiles.width + tiles.gap);
        const line = index === dock.tile ? COLORS.accent : COLORS.border;
        commands.push(
          ...box(x, tiles.y, tiles.width, tiles.height, BUTTON, 100, line),
          ...box(x + 1, tiles.y + 1, tiles.width - 2, tiles.art, 0x24272c),
        );
        if (tile.loading) {
          this.text(left + x, top + tiles.y, tiles.width, tiles.art, 'Loading…', {
            size: TEXT.small,
            color: COLORS.faint,
            align: 'c',
          });
        } else if (tile.picture) {
          this.pictures.push({
            key: tile.picture,
            x: left + x + 1,
            y: top + tiles.y + 1,
            w: tiles.width - 2,
            h: tiles.art,
          });
        } else {
          const glyph = this.glyphs[tile.glyph] ?? [];
          commands.push(...placed(glyph, x + tiles.width / 2, tiles.y + tiles.art / 2));
        }
        const textX = left + x + 8;
        const nameY = top + tiles.y + tiles.art + 6;
        this.text(textX, nameY, tiles.width - 16, 24, tile.name, { size: TEXT.small });
        this.text(textX, nameY + 22, tiles.width - 16, 22, tile.meta, {
          size: 12,
          color: COLORS.faint,
        });
      });
    } else if (dock.tabs[dock.tab] === 'History') {
      this.text(left + tiles.x, top + DOCK.top, 900, 30, 'History · newest first', {
        color: COLORS.dim,
      });
      dock.history.forEach((line, index) => {
        const y = top + tiles.y + index * 22;
        this.text(left + tiles.x + 8, y, 1600, 22, line, { size: TEXT.small });
      });
    } else if (dock.tab === 2) {
      this.renderIssues(dock, commands);
    } else if (dock.tab === 3) {
      this.renderSaves(dock, commands);
    } else {
      this.renderAi(dock, commands);
    }
    this.paint(this.dock, commands);
  }

  // An editor's own Saves tab (the Asset/Animation Studio's): the left column says what the tab
  // is for (its dock.saves.help), the rest is World Architect's Saves tab.
  renderOwnSaves(dock, commands) {
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    this.text(left + 14, top + DOCK.top - 8, 200, 22, 'SAVES', {
      size: 12,
      color: COLORS.faint,
      bold: true,
    });
    (dock.saves.help ?? []).forEach((line, index) => {
      this.text(left + 14, top + DOCK.folders + index * 22, DOCK.nav - 20, 22, line, {
        size: TEXT.small,
        color: COLORS.dim,
      });
    });
    this.renderSaves(dock, commands);
  }

  // A press on the AI tab: a row's Copy copies that row (aiCopy).
  pressAi(px, py) {
    const row = Math.floor((py - AI.top) / AI.row);
    const copy = aiCopy();
    if (py >= AI.top && row < 3 && px >= copy.x && px < copy.x + copy.width) {
      this.handlers.aiCopy?.(row);
    }
  }

  // An editor's own AI tab (the Asset/Animation Studio's, at another place in its tab order): the
  // left column says what the tab is for, the rest is World Architect's AI tab.
  renderOwnAi(dock, commands) {
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    const about = DOCK_ABOUT[4];
    this.text(left + 14, top + DOCK.top - 8, 220, 22, about.title, {
      size: 12,
      color: COLORS.faint,
      bold: true,
    });
    about.lines.forEach((line, index) => {
      this.text(left + 14, top + DOCK.folders + index * 22, DOCK.nav - 20, 22, line, {
        size: TEXT.small,
        color: COLORS.dim,
      });
    });
    this.renderAi(dock, commands);
  }

  // The AI tab: the assistant's state, then what an AI app is given to connect, each in a field
  // with Copy at the right.
  renderAi(dock, commands) {
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    const { tiles } = dockPlaces();
    const ai = dock.ai;
    const dot = AI.dots[ai.state] ?? AI.dots.off;
    commands.push(...box(tiles.x, DOCK.top + 11, 9, 9, dot));
    this.text(left + tiles.x + 18, top + DOCK.top, 1000, 30, ai.status, { bold: true });
    if (ai.note) {
      this.text(left + aiCopy().x - 700, top + DOCK.top, 812, 30, ai.note, {
        size: TEXT.small,
        color: COLORS.faint,
        align: 'r',
      });
    }
    const fieldWidth = aiCopy().x - 12 - AI.field.x;
    ai.rows.forEach((entry, index) => {
      const y = AI.top + index * AI.row;
      const middle = y + (AI.row - AI.field.height) / 2;
      this.text(left + tiles.x, top + middle, AI.label, AI.field.height, entry.label, {
        bold: true,
      });
      commands.push(
        ...box(AI.field.x, middle, fieldWidth, AI.field.height, FIELD, 100, COLORS.border),
      );
      this.text(
        left + AI.field.x + 12,
        top + middle,
        fieldWidth - 24,
        AI.field.height,
        entry.shown,
        {
          size: TEXT.small,
          color: COLORS.dim,
        },
      );
      commands.push(
        ...box(aiCopy().x, middle, aiCopy().width, AI.field.height, CHIP_ON, 100, CHIP_ON_LINE),
      );
      this.text(left + aiCopy().x, top + middle, aiCopy().width, AI.field.height, 'Copy', {
        color: COLORS.accentText,
        bold: true,
        align: 'c',
      });
    });
  }

  // The Saves tab: the name field and Save as, then each save with Load, Merge and Delete.
  renderSaves(dock, commands) {
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    const { tiles } = dockPlaces();
    const { field, saveAs, reset, buttons } = savesPlaces();
    const saves = dock.saves;
    commands.push(
      ...box(
        field.x,
        DOCK.top,
        field.width,
        30,
        FIELD,
        100,
        saves.typing ? COLORS.accent : COLORS.border,
      ),
    );
    this.text(left + field.x + 10, top + DOCK.top, field.width - 20, 30, saves.name, {
      color: saves.typing || saves.named ? COLORS.text : COLORS.dim,
    });
    commands.push(...box(saveAs.x, DOCK.top, saveAs.width, 30, CHIP_ON, 100, CHIP_ON_LINE));
    this.text(left + saveAs.x, top + DOCK.top, saveAs.width, 30, 'Save as', {
      color: COLORS.accentText,
      bold: true,
      align: 'c',
    });
    this.text(left + saveAs.x + saveAs.width + 16, top + DOCK.top, 560, 30, saves.folder, {
      size: TEXT.small,
      color: COLORS.dim,
    });
    // Reset map: the game's own map again (asked first in the menu bar).
    if (saves.reset) {
      commands.push(...box(reset.x, DOCK.top, reset.width, 30, BUTTON, 100, COLORS.border));
      this.text(left + reset.x, top + DOCK.top, reset.width, 30, 'Reset map', {
        bold: true,
        align: 'c',
      });
    }
    saves.rows.slice(0, ISSUE_ROWS).forEach((save, index) => {
      const y = ISSUE.top + index * ISSUE.row;
      commands.push(...rule(tiles.x, y + ISSUE.row - 1, area.width - tiles.x - 12, 1));
      this.text(left + tiles.x, top + y, 520, ISSUE.row, save.name, { bold: true });
      this.text(left + tiles.x + 540, top + y, 420, ISSUE.row, save.when, {
        size: TEXT.small,
        color: COLORS.dim,
      });
      buttons.forEach((button) => {
        commands.push(
          ...box(button.x, y + 5, button.width, ISSUE.row - 10, BUTTON, 100, COLORS.border),
        );
        this.text(left + button.x, top + y + 5, button.width, ISSUE.row - 10, button.label, {
          size: TEXT.small,
          align: 'c',
        });
      });
    });
    if (saves.rows.length === 0) {
      this.text(
        left + tiles.x,
        top + ISSUE.top,
        1200,
        ISSUE.row,
        'No saves yet: name one and press Save as.',
        {
          color: COLORS.dim,
        },
      );
    }
  }

  // The Validation tab: each issue's level chip, text and object; a press goes to it.
  renderIssues(dock, commands) {
    const area = LAYOUT.dock;
    const { x: left, y: top } = area;
    const { tiles } = dockPlaces();
    const chips = {
      Warning: { fill: 0x2a2418, text: COLORS.accentText },
      Note: { fill: 0x1a2533, text: 0x9cc6f5 },
      Passed: { fill: 0x1a2a1d, text: 0x9fd9a0 },
    };
    this.text(left + tiles.x, top + DOCK.top, 900, 30, dock.issueSummary, { color: COLORS.dim });
    dock.issues.slice(0, ISSUE_ROWS).forEach((issue, index) => {
      const y = ISSUE.top + index * ISSUE.row;
      const chip = chips[issue.level] ?? chips.Note;
      commands.push(...box(tiles.x, y + 5, 80, ISSUE.row - 10, chip.fill));
      commands.push(...rule(tiles.x, y + ISSUE.row - 1, area.width - tiles.x - 12, 1));
      this.text(left + tiles.x, top + y + 5, 80, ISSUE.row - 10, issue.level, {
        size: 12,
        color: chip.text,
        bold: true,
        align: 'c',
      });
      this.text(left + tiles.x + 96, top + y, 1100, ISSUE.row, issue.text);
      this.text(left + tiles.x + 1210 + wider(), top + y, 420, ISSUE.row, issue.object, {
        size: TEXT.small,
        color: COLORS.dim,
      });
    });
  }

  renderStatus(status) {
    const area = LAYOUT.status;
    const { y: top } = area;
    this.paint(this.status, [
      ...box(0, 0, area.width, area.height, COLORS.bar, 97),
      ...rule(0, 0, area.width, 1),
      ...box(1824 + wider(), 11, 7, 7, GREEN),
    ]);
    const style = { size: TEXT.small, color: COLORS.dim };
    this.text(14, top, 620, area.height, status.selected, { size: TEXT.small });
    this.text(660 + wider() / 2, top, 400, area.height, status.cursor, style);
    this.text(1360 + wider(), top, 220, area.height, status.snaps, style);
    this.text(1600 + wider(), top, 200, area.height, status.objects, style);
    this.text(1838 + wider(), top, 80, area.height, 'Local', { size: TEXT.small, color: GREEN });
  }
}

module.exports = {
  Shell,
  TILE_COUNT,
  outlinerRows,
  DETAIL_ROWS,
  FOLDER_ROWS,
  detailRows,
  detailStart,
  mapListBox,
  DOCK,
  box,
  rule,
  toggle,
};
