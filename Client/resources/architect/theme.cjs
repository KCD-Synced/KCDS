'use strict';

// World Architect's look and layout, from the owner's final design canvas (the
// "pro redesign": a dark editor with an amber accent). The canvas is 1600 by 900; the Menu movie's
// stage is 1920 by 1080, so every place and size here is the canvas's times 1.2.

const STAGE = Object.freeze({ width: 1920, height: 1080 });

const COLORS = Object.freeze({
  // Panels, the darker bars and fields, section headers.
  panel: 0x17191c,
  bar: 0x0f1012,
  section: 0x1b1d21,
  border: 0x2c3036,
  // Text: normal, secondary, faint.
  text: 0xe4e6e9,
  dim: 0xa3a9b1,
  faint: 0x868d96,
  // The amber accent, its text and the chosen row's ground.
  accent: 0xf0a93b,
  accentText: 0xf5b75a,
  chosen: 0x3a2e1a,
  danger: 0xffb4a6,
});

// The panels, in the editor's stage pixels. The game stretches its Menu movie's 1920 by 1080 stage
// over the whole window (each axis on its own), so the editor lays itself out on a stage of the
// window's own shape instead (a narrow window squeezed every panel): 1080 high,
// as wide as the window's shape makes it, the panels at their size and the world view and the
// content strip taking the rest. A window too narrow for MIN_STAGE_WIDTH makes the stage higher
// (everything smaller) instead. fitStage() sets the places here; they start at 1920 by 1080.
const MIN_STAGE_WIDTH = 1800;
const SIDE = Object.freeze({ outliner: 326, details: 384, top: 84, dock: 282, status: 30 });
const LAYOUT = {
  width: STAGE.width,
  height: STAGE.height,
  menu: { x: 0, y: 0, width: 1920, height: 36 },
  toolbar: { x: 0, y: 36, width: 1920, height: 48 },
  outliner: { x: 0, y: 84, width: 326, height: 684 },
  view: { x: 326, y: 84, width: 1210, height: 684 },
  details: { x: 1536, y: 84, width: 384, height: 684 },
  dock: { x: 0, y: 768, width: 1920, height: 282 },
  status: { x: 0, y: 1050, width: 1920, height: 30 },
};

// The editor's stage for a window `width` by `height` pixels: { width, height }.
function stageFor(width, height) {
  if (!(width > 0) || !(height > 0)) {
    return { width: STAGE.width, height: STAGE.height };
  }
  const shape = width / height;
  const stageHeight = Math.max(STAGE.height, Math.ceil(MIN_STAGE_WIDTH / shape));
  return { width: Math.round(stageHeight * shape), height: stageHeight };
}

// Lays the panels out on a stage `width` by `height` (stageFor). The areas are changed in place,
// so whoever holds one sees the new place. True when anything moved.
function fitStage(width, height) {
  if (width === LAYOUT.width && height === LAYOUT.height) {
    return false;
  }
  const middle = height - SIDE.top - SIDE.dock - SIDE.status;
  LAYOUT.width = width;
  LAYOUT.height = height;
  LAYOUT.menu.width = width;
  LAYOUT.toolbar.width = width;
  Object.assign(LAYOUT.outliner, { height: middle });
  Object.assign(LAYOUT.view, {
    width: width - SIDE.outliner - SIDE.details,
    height: middle,
  });
  Object.assign(LAYOUT.details, { x: width - SIDE.details, height: middle });
  Object.assign(LAYOUT.dock, { y: SIDE.top + middle, width });
  Object.assign(LAYOUT.status, { y: height - SIDE.status, width });
  return true;
}

const FONT = Object.freeze({ face: 'DefaultFont', size: 16, small: 14, title: 18 });
// A list's line height.
const ROW = 28;

module.exports = { STAGE, COLORS, LAYOUT, FONT, ROW, stageFor, fitStage };
