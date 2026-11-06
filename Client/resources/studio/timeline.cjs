'use strict';

// The Asset/Animation Studio's timeline, drawn in the dock's content tab in Bones
// mode, as a video editor's: the transport on the left (to the start, play or pause, to the end,
// the time, add and remove a key, loop, blend), then a ruler in seconds with the playhead, and a
// track for all keys and one for each turned joint, each key a diamond on it.
//
// The editor gives the timeline's state (editor.cjs timelineView()):
//   { time, span, length, playing, loop, blend, key, keys: [time, ...],
//     tracks: [{ label, joint, keys: [key index, ...], chosen }], drag: { key, time } | null }
// and asks what a press or a release at a place of the dock means (hit()).

const { box, rule, toggle } = require('../architect/shell.cjs');
const { COLORS, LAYOUT } = require('../architect/theme.cjs');

const BUTTON = 0x1f2226;
const FIELD = 0x0f1012;
const KEY_LINE = 0xc9cdd2;
// The lash's keys (the whip posed by hand).
const LASH_KEY = 0x8fb4d8;
const TRACK = 0x1b1d21;
const TRACK_CHOSEN = 0x2a2418;

// Places in the dock (its 1920 by 282 area; the tabs take the top 34).
const NAV = Object.freeze({
  time: { x: 14, y: 42, width: 224, height: 32 },
  transport: { y: 80, height: 34, buttons: [14, 91, 168], width: 70 },
  keys: { y: 122, height: 32, add: 14, remove: 130, width: 108 },
  loop: { y: 162, height: 32 },
  blend: { y: 202, height: 32, x: 96, width: 142 },
});
const LABELS = Object.freeze({ x: 264, width: 160 });
const TIME = Object.freeze({ left: 432, right: 1896 });
const RULER = Object.freeze({ top: 40, height: 24 });
const TRACKS = Object.freeze({ top: 70, row: 24 });
const TRACK_ROWS = 8;
const KEY_SIZE = 7;
// Up to this many keys on the tracks shown are drawn as diamonds, more as lines.
const DIAMONDS_UP_TO = 60;
// A press this near a key (pixels) takes the key.
const KEY_REACH = 9;

const BLEND_NAMES = Object.freeze({ spline: 'Spline', smooth: 'Smooth', linear: 'Linear' });

function pixelsPerSecond(timeline) {
  return (TIME.right - TIME.left) / timeline.span;
}

function xOf(timeline, time) {
  return TIME.left + time * pixelsPerSecond(timeline);
}

// A key's diamond.
function diamond(x, y, size, fill, line) {
  return [
    ['lineStyle', 1, line, 100],
    ['beginFill', fill, 100],
    ['moveTo', x, y - size],
    ['lineTo', x + size, y],
    ['lineTo', x, y + size],
    ['lineTo', x - size, y],
    ['lineTo', x, y - size],
    ['endFill'],
  ];
}

// The ruler's ticks: every tenth of a second, longer and named every half second (every second
// when the span is long).
function rulerCommands(shell, timeline, commands) {
  const area = LAYOUT.dock;
  const named = timeline.span > 6 ? 1 : 0.5;
  const steps = Math.round(timeline.span / 0.1);
  // Each tick a line (two commands, a box was eight).
  commands.push(['lineStyle', 1, COLORS.border, 100]);
  for (let step = 0; step <= steps; step += 1) {
    const time = step / 10;
    const x = Math.round(xOf(timeline, time));
    const major = Math.abs(time / named - Math.round(time / named)) < 1e-6;
    const height = major ? 12 : 5;
    const bottom = RULER.top + RULER.height;
    commands.push(['moveTo', x + 0.5, bottom - height], ['lineTo', x + 0.5, bottom]);
    if (major) {
      shell.text(area.x + x + 3, area.y + RULER.top - 2, 60, 18, `${time.toFixed(1)} s`, {
        size: 12,
        color: COLORS.faint,
      });
    }
  }
  commands.push(...rule(TIME.left, RULER.top + RULER.height, TIME.right - TIME.left, 1));
}

function renderTimeline(shell, timeline, commands) {
  const area = LAYOUT.dock;
  const left = area.x;
  const top = area.y;
  const textStyle = { size: 13, color: COLORS.text, align: 'c' };
  // The transport.
  const { time, transport, keys, loop, blend } = NAV;
  commands.push(...box(time.x, time.y, time.width, time.height, FIELD, 100, COLORS.border));
  shell.text(
    left + time.x,
    top + time.y,
    time.width,
    time.height,
    `${timeline.time.toFixed(2)} s  /  ${timeline.length.toFixed(2)} s`,
    { size: 15, color: COLORS.accentText, align: 'c', bold: true },
  );
  const transportLabels = ['|◀', timeline.playing ? '❚❚' : '▶', '▶|'];
  transport.buttons.forEach((x, index) => {
    const on = index === 1 && timeline.playing;
    commands.push(
      ...box(
        x,
        transport.y,
        transport.width,
        transport.height,
        on ? TRACK_CHOSEN : BUTTON,
        100,
        on ? COLORS.accent : COLORS.border,
      ),
    );
    shell.text(
      left + x,
      top + transport.y,
      transport.width,
      transport.height,
      transportLabels[index],
      { ...textStyle, size: 15 },
    );
  });
  commands.push(
    ...box(keys.add, keys.y, keys.width, keys.height, BUTTON, 100, COLORS.border),
    ...box(keys.remove, keys.y, keys.width, keys.height, BUTTON, 100, COLORS.border),
  );
  shell.text(left + keys.add, top + keys.y, keys.width, keys.height, '+ Key', textStyle);
  shell.text(left + keys.remove, top + keys.y, keys.width, keys.height, '− Key', textStyle);
  shell.text(left + 14, top + loop.y, 120, loop.height, 'Loop', { size: 13, color: COLORS.dim });
  commands.push(...toggle(196, loop.y + 7, timeline.loop));
  shell.text(left + 14, top + blend.y, 80, blend.height, 'Blend', { size: 13, color: COLORS.dim });
  commands.push(...box(blend.x, blend.y, blend.width, blend.height, FIELD, 100, COLORS.border));
  shell.text(left + blend.x, top + blend.y, 24, blend.height, '‹', textStyle);
  shell.text(
    left + blend.x + 24,
    top + blend.y,
    blend.width - 48,
    blend.height,
    BLEND_NAMES[timeline.blend] ?? timeline.blend,
    textStyle,
  );
  shell.text(left + blend.x + blend.width - 24, top + blend.y, 24, blend.height, '›', textStyle);

  // The ruler and the tracks.
  rulerCommands(shell, timeline, commands);
  const rows = rowsOf(timeline);
  rows.forEach((row, index) => {
    const y = TRACKS.top + index * TRACKS.row;
    commands.push(
      ...box(
        LABELS.x,
        y + 1,
        TIME.right - LABELS.x,
        TRACKS.row - 2,
        row.chosen ? TRACK_CHOSEN : TRACK,
      ),
    );
    shell.text(left + LABELS.x + 8, top + y, LABELS.width - 12, TRACKS.row, row.label, {
      size: 12,
      color: row.chosen ? COLORS.accentText : index === 0 ? COLORS.dim : COLORS.text,
      bold: index === 0,
    });
    // Many keys (a baked clip's 30 on every track) are drawn as short upright lines, the chosen
    // or dragged one as a diamond: the game draws at most 1024 commands in a frame, and a
    // diamond each (8) went past it, so the dock kept the picture before.
    const marks = rows.length * timeline.keys.length > DIAMONDS_UP_TO;
    if (marks) {
      commands.push(['lineStyle', 3, KEY_LINE, 100]);
    }
    // The lash's keys (its own times): diamonds, the one at the playhead chosen.
    if (row.lash) {
      row.times.forEach((time, keyIndex) => {
        const chosen = keyIndex === timeline.lashKey;
        commands.push(
          ...diamond(
            xOf(timeline, time),
            y + TRACKS.row / 2,
            KEY_SIZE - 1,
            chosen ? COLORS.accent : LASH_KEY,
            chosen ? 0xffffff : COLORS.border,
          ),
        );
      });
      if (marks) {
        commands.push(['lineStyle', 3, KEY_LINE, 100]);
      }
      return;
    }
    for (const keyIndex of row.keys) {
      const dragged = timeline.drag && timeline.drag.key === keyIndex;
      const x = xOf(timeline, dragged ? timeline.drag.time : timeline.keys[keyIndex]);
      const chosen = keyIndex === timeline.key;
      if (marks && !chosen && !dragged) {
        commands.push(['moveTo', x, y + 7], ['lineTo', x, y + TRACKS.row - 7]);
        continue;
      }
      commands.push(
        ...diamond(
          x,
          y + TRACKS.row / 2,
          KEY_SIZE,
          chosen || dragged ? COLORS.accent : KEY_LINE,
          chosen ? 0xffffff : COLORS.border,
        ),
      );
      if (marks) {
        commands.push(['lineStyle', 3, KEY_LINE, 100]);
      }
    }
  });
  // The playhead over the ruler and the tracks.
  const playX = Math.round(xOf(timeline, timeline.time));
  const bottom = TRACKS.top + rows.length * TRACKS.row;
  commands.push(
    ...box(playX - 5, RULER.top, 11, 10, COLORS.accent),
    ...box(playX, RULER.top, 2, bottom - RULER.top, COLORS.accent),
  );
  const shown = rows.length - 1 - (timeline.lashKeys?.length ? 1 : 0);
  if (timeline.tracks.length > shown) {
    shell.text(
      left + LABELS.x + 8,
      top + bottom,
      600,
      18,
      `${timeline.tracks.length - shown} more turned joints: choose one to see its track`,
      { size: 12, color: COLORS.faint },
    );
  }
}

// What a press at (px, py) in the dock means:
//   { kind: 'start' | 'play' | 'end' | 'addKey' | 'removeKey' | 'loop' }
//   { kind: 'blend', direction }
//   { kind: 'key', key, joint }      a key's diamond (joint null on the All keys track)
//   { kind: 'scrub', time, joint }   the ruler or a track elsewhere: the playhead goes there
//   { kind: 'joint', joint }         a track's name
// or null for nothing.
function hit(timeline, px, py) {
  const { transport, keys, loop, blend } = NAV;
  if (px < LABELS.x - 8) {
    if (py >= transport.y && py < transport.y + transport.height) {
      const index = transport.buttons.findIndex((x) => px >= x && px < x + transport.width);
      return index >= 0 ? { kind: ['start', 'play', 'end'][index] } : null;
    }
    if (py >= keys.y && py < keys.y + keys.height) {
      if (px >= keys.add && px < keys.add + keys.width) {
        return { kind: 'addKey' };
      }
      if (px >= keys.remove && px < keys.remove + keys.width) {
        return { kind: 'removeKey' };
      }
      return null;
    }
    if (py >= loop.y && py < loop.y + loop.height) {
      return { kind: 'loop' };
    }
    if (py >= blend.y && py < blend.y + blend.height && px >= blend.x) {
      return { kind: 'blend', direction: px < blend.x + blend.width / 2 ? -1 : 1 };
    }
    return null;
  }
  const rows = rowsOf(timeline);
  const row = py >= TRACKS.top ? Math.floor((py - TRACKS.top) / TRACKS.row) : -1;
  const track = rows[row] ?? null;
  if (track?.lash && px >= TIME.left - KEY_REACH) {
    const near = track.times
      .map((time, index) => ({ index, gap: Math.abs(xOf(timeline, time) - px) }))
      .filter((one) => one.gap <= KEY_REACH)
      .sort((a, b) => a.gap - b.gap)[0];
    if (near) {
      return { kind: 'lashKey', index: near.index, time: track.times[near.index] };
    }
  }
  if (px < TIME.left - KEY_REACH) {
    return track && track.joint !== null ? { kind: 'joint', joint: track.joint } : null;
  }
  if (py < RULER.top || (row >= rows.length && py >= TRACKS.top)) {
    return null;
  }
  if (track && !track.lash) {
    const near = track.keys
      .map((keyIndex) => ({ keyIndex, gap: Math.abs(xOf(timeline, timeline.keys[keyIndex]) - px) }))
      .filter((one) => one.gap <= KEY_REACH)
      .sort((a, b) => a.gap - b.gap)[0];
    if (near) {
      return { kind: 'key', key: near.keyIndex, joint: track.joint };
    }
  }
  const seconds = (px - TIME.left) / pixelsPerSecond(timeline);
  const time = Math.min(timeline.span, Math.max(0, Math.round(seconds * 100) / 100));
  return { kind: 'scrub', time, joint: track ? track.joint : null };
}

// The rows shown: All keys, the lash's keys when it has them (its own times), then the joints'
// tracks, at most TRACK_ROWS.
function rowsOf(timeline) {
  const all = { label: 'All keys', joint: null, keys: timeline.keys.map((_, index) => index) };
  const lash = timeline.lashKeys?.length
    ? [{ label: 'Lash', joint: null, lash: true, keys: [], times: timeline.lashKeys }]
    : [];
  return [all, ...lash, ...timeline.tracks].slice(0, TRACK_ROWS);
}

// The time at a place across the dock (a dragged key's), to a hundredth of a second.
function timeAt(timeline, px) {
  const seconds = (px - TIME.left) / pixelsPerSecond(timeline);
  return Math.min(timeline.span, Math.max(0, Math.round(seconds * 100) / 100));
}

// The span the timeline shows: the animation and a little after it, at least two seconds, in
// half seconds.
function spanOf(length) {
  return Math.max(2, Math.ceil((length + 0.5) * 2) / 2);
}

module.exports = { renderTimeline, hit, timeAt, spanOf, TRACK_ROWS };
