'use strict';

// The host movie of native screens: a small SWF 8 movie whose
// ActionScript 2 functions do what needs ActionScript objects, which the game's Invoke cannot
// pass. Each screen loads it into a hidden clip; the runtime calls its functions with paths:
//   kcdsMask(target, mask)     target.setMask(mask), "" removes the mask
//   kcdsHit(target, area)      target.hitArea = area, "" removes it
//   kcdsFormat(target, font, size, color, align, bold, italic)
//                              a TextFormat for the text field, now and for new text
//   kcdsWatch(target, id)      mouse handlers that send fscommand("kcdsClip", "<id>,<event>")
//   kcdsUnwatch(target)        removes them
//   kcdsStage()                "<Stage.width>,<Stage.height>"
//   kcdsLeading(target, n)     the text field's leading: n more pixels between its lines
//   kcdsTabs(target, s0..s14)  the text field's 15 tab stops, pixels from its left, now
//                              and for new text (World Architect's columns)
// kcdsVersion tells that the movie has loaded.

const HOST_VERSION = 6;
const CLIP_EVENTS = Object.freeze({
  press: 'onPress',
  release: 'onRelease',
  releaseOutside: 'onReleaseOutside',
  rollOver: 'onRollOver',
  rollOut: 'onRollOut',
  dragOver: 'onDragOver',
  dragOut: 'onDragOut',
});

const OP = Object.freeze({
  pop: 0x17,
  not: 0x12,
  getVariable: 0x1c,
  setVariable: 0x1d,
  defineLocal: 0x3c,
  newObject: 0x40,
  getMember: 0x4e,
  setMember: 0x4f,
  equals2: 0x49,
  callMethod: 0x52,
  initArray: 0x42,
  add2: 0x47,
  getUrl2: 0x9a,
  return: 0x3e,
  jump: 0x99,
  defineFunction: 0x9b,
  if: 0x9d,
  push: 0x96,
});

function u16(value) {
  return [value & 0xff, (value >> 8) & 0xff];
}

function u32(value) {
  return [value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >>> 24) & 0xff];
}

function text(value) {
  return [...new TextEncoder().encode(value), 0];
}

// One action: its code, then its length and payload when it has one.
function action(code, payload = []) {
  return code >= 0x80 ? [code, ...u16(payload.length), ...payload] : [code];
}

// Push of strings, integers, booleans, null and undefined.
function push(...values) {
  const payload = [];
  for (const value of values) {
    if (typeof value === 'string') {
      payload.push(0, ...text(value));
    } else if (value === null) {
      payload.push(2);
    } else if (value === undefined) {
      payload.push(3);
    } else if (typeof value === 'boolean') {
      payload.push(5, value ? 1 : 0);
    } else {
      payload.push(7, ...u32(value));
    }
  }
  return action(OP.push, payload);
}

// The value of a local or parameter, or the object a path names (eval).
function variable(name) {
  return [...push(name), ...action(OP.getVariable)];
}

function evalPath(parameter) {
  return [...variable(parameter), ...action(OP.getVariable)];
}

function branch(code, offset) {
  return action(code, u16(offset & 0xffff));
}

// if (parameter == "") { empty } else { given }
function ifEmpty(parameter, empty, given) {
  const skipEmpty = [...empty, ...branch(OP.jump, given.length)];
  return [
    ...variable(parameter),
    ...push(''),
    ...action(OP.equals2),
    ...action(OP.not),
    ...branch(OP.if, skipEmpty.length),
    ...skipEmpty,
    ...given,
  ];
}

function callMethod(objectCode, method, argumentCode) {
  return [
    ...argumentCode,
    ...push(1),
    ...objectCode,
    ...push(method),
    ...action(OP.callMethod),
    ...action(OP.pop),
  ];
}

function defineFunction(name, parameters, body) {
  const header = [...text(name), ...u16(parameters.length)];
  for (const parameter of parameters) {
    header.push(...text(parameter));
  }
  header.push(...u16(body.length));
  return [...action(OP.defineFunction, header), ...body];
}

function maskFunction() {
  return defineFunction(
    'kcdsMask',
    ['t', 'm'],
    ifEmpty(
      'm',
      callMethod(evalPath('t'), 'setMask', push(null)),
      callMethod(evalPath('t'), 'setMask', evalPath('m')),
    ),
  );
}

function hitFunction() {
  const set = (valueCode) => [
    ...evalPath('t'),
    ...push('hitArea'),
    ...valueCode,
    ...action(OP.setMember),
  ];
  return defineFunction(
    'kcdsHit',
    ['t', 'h'],
    ifEmpty('h', set(push(undefined)), set(evalPath('h'))),
  );
}

function formatFunction() {
  const members = ['font', 'size', 'color', 'align', 'bold', 'italic'];
  const body = [...push('f', 0, 'TextFormat'), ...action(OP.newObject), ...action(OP.defineLocal)];
  for (const member of members) {
    body.push(...variable('f'), ...push(member), ...variable(member), ...action(OP.setMember));
  }
  body.push(
    ...push('x'),
    ...evalPath('t'),
    ...action(OP.defineLocal),
    ...variable('x'),
    ...push('embedFonts', true),
    ...action(OP.setMember),
    ...callMethod(variable('x'), 'setNewTextFormat', variable('f')),
    ...callMethod(variable('x'), 'setTextFormat', variable('f')),
  );
  return defineFunction('kcdsFormat', ['t', ...members], body);
}

// kcdsTabs(t, a0, ..., a14): var x = eval(t); var f = new TextFormat();
// f.tabStops = [a0, ..., a14]; x.setNewTextFormat(f); x.setTextFormat(f);
// The stops come as numbers (a split string gives strings, which the game ignores).
// A host call takes at most 16 arguments: the target and 15 stops.
const TAB_STOPS = 15;

// kcdsLeading(t, n): var x = eval(t); var f = new TextFormat(); f.leading = n;
// x.setNewTextFormat(f); x.setTextFormat(f);
function leadingFunction() {
  const body = [
    ...push('x'),
    ...evalPath('t'),
    ...action(OP.defineLocal),
    ...push('f', 0, 'TextFormat'),
    ...action(OP.newObject),
    ...action(OP.defineLocal),
    ...variable('f'),
    ...push('leading'),
    ...variable('n'),
    ...action(OP.setMember),
    ...callMethod(variable('x'), 'setNewTextFormat', variable('f')),
    ...callMethod(variable('x'), 'setTextFormat', variable('f')),
  ];
  return defineFunction('kcdsLeading', ['t', 'n'], body);
}

function tabsFunction() {
  const names = [];
  for (let index = 0; index < TAB_STOPS; index += 1) {
    names.push(`a${index}`);
  }
  // initArray takes the count on top and then the elements, the first element nearest.
  const array = [];
  for (const name of [...names].reverse()) {
    array.push(...variable(name));
  }
  array.push(...push(TAB_STOPS), ...action(OP.initArray));
  const body = [
    ...push('x'),
    ...evalPath('t'),
    ...action(OP.defineLocal),
    ...push('f', 0, 'TextFormat'),
    ...action(OP.newObject),
    ...action(OP.defineLocal),
    ...variable('f'),
    ...push('tabStops'),
    ...array,
    ...action(OP.setMember),
    ...callMethod(variable('x'), 'setNewTextFormat', variable('f')),
    ...callMethod(variable('x'), 'setTextFormat', variable('f')),
  ];
  return defineFunction('kcdsTabs', ['t', ...names], body);
}

function tag(code, payload) {
  if (payload.length < 0x3f) {
    return [...u16((code << 6) | payload.length), ...payload];
  }
  return [...u16((code << 6) | 0x3f), ...u32(payload.length), ...payload];
}

// A handler that sends fscommand("kcdsClip", this.kcdsId + ",<event>").
function clipHandler(event) {
  const body = [
    ...push('FSCommand:kcdsClip'),
    ...variable('this'),
    ...push('kcdsId'),
    ...action(OP.getMember),
    ...push(`,${event}`),
    ...action(OP.add2),
    ...action(OP.getUrl2, [0]),
  ];
  return defineFunction('', [], body);
}

function watchFunction() {
  const body = [
    ...push('x'),
    ...evalPath('t'),
    ...action(OP.defineLocal),
    ...variable('x'),
    ...push('kcdsId'),
    ...variable('id'),
    ...action(OP.setMember),
  ];
  for (const [event, handler] of Object.entries(CLIP_EVENTS)) {
    body.push(...variable('x'), ...push(handler), ...clipHandler(event), ...action(OP.setMember));
  }
  return defineFunction('kcdsWatch', ['t', 'id'], body);
}

function unwatchFunction() {
  const body = [...push('x'), ...evalPath('t'), ...action(OP.defineLocal)];
  for (const handler of Object.values(CLIP_EVENTS)) {
    body.push(...variable('x'), ...push(handler, undefined), ...action(OP.setMember));
  }
  return defineFunction('kcdsUnwatch', ['t'], body);
}

function stageFunction() {
  const size = (member) => [...variable('Stage'), ...push(member), ...action(OP.getMember)];
  const body = [
    ...size('width'),
    ...push(','),
    ...action(OP.add2),
    ...size('height'),
    ...action(OP.add2),
    ...action(OP.return),
  ];
  return defineFunction('kcdsStage', [], body);
}

// The host movie as SWF bytes.
function hostMovie() {
  const actions = [
    ...maskFunction(),
    ...hitFunction(),
    ...formatFunction(),
    ...watchFunction(),
    ...unwatchFunction(),
    ...stageFunction(),
    ...leadingFunction(),
    ...tabsFunction(),
    ...push('kcdsVersion', HOST_VERSION),
    ...action(OP.setVariable),
    0,
  ];
  const body = [
    0x00, // empty frame rectangle (Nbits 0)
    ...u16(24 << 8), // 24 frames a second
    ...u16(1), // one frame
    ...tag(69, u32(0)), // FileAttributes: ActionScript 2, no network access
    ...tag(12, actions), // DoAction
    ...tag(1, []), // ShowFrame
    ...tag(0, []), // End
  ];
  return Uint8Array.from([0x46, 0x57, 0x53, 8, ...u32(body.length + 8), ...body]);
}

module.exports = { hostMovie, HOST_VERSION, CLIP_EVENTS, TAB_STOPS };
