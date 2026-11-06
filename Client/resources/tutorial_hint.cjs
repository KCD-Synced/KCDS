'use strict';

// The game's own tutorial hint with a resource's content: a KCDS
// extension of the Hud global next to showTutorial.
//
// The box in the top-left corner is the HUD movie's tutorial: the hint is shown and hidden through
// the HUD element (NativeUI), in the game's own markup, <head> for the title and <body> for each
// paragraph; $action; tokens in the text are drawn as the key bound to that action. The game
// client reports the game's own tutorial calls on the HUD ({ op: 'tutorialHintGame', call, id },
// Client/runtime/hud/tutorial_hint.cpp): one that hides the current tutorial took the hint down
// (the player closed it, or opened a menu), one that shows another tutorial replaced it.

const TITLE_MAX = 128;
const TEXT_MAX = 4096;
const DURATION_MAX_MS = 600000;
const OPTION_KEYS = new Set(['title', 'text', 'closeHint', 'durationMs']);
// HUD.xml ShowTutorial: id, text, duration, in dialogue, priority, layout, action hint, overlay.
const PRIORITY = 100;
const LAYOUT = 0;

function escapeMarkup(text) {
  return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function checkOptions(options) {
  if (options === null || typeof options !== 'object' || Array.isArray(options)) {
    throw new TypeError('Hud.showTutorialHint: options must be an object');
  }
  for (const key of Object.keys(options)) {
    if (!OPTION_KEYS.has(key)) {
      throw new TypeError(`Hud.showTutorialHint: unknown option ${key}`);
    }
  }
  const { title = '', text, closeHint = true, durationMs = 0 } = options;
  if (typeof title !== 'string' || title.length > TITLE_MAX) {
    throw new TypeError(`Hud.showTutorialHint: title must be a string of at most ${TITLE_MAX}`);
  }
  if (typeof text !== 'string' || text.length === 0 || text.length > TEXT_MAX) {
    throw new TypeError(`Hud.showTutorialHint: text must be a string of 1 to ${TEXT_MAX}`);
  }
  if (typeof closeHint !== 'boolean') {
    throw new TypeError('Hud.showTutorialHint: closeHint must be a boolean');
  }
  if (!Number.isInteger(durationMs) || durationMs < 0 || durationMs > DURATION_MAX_MS) {
    throw new RangeError(`Hud.showTutorialHint: durationMs must be 0 to ${DURATION_MAX_MS}`);
  }
  return { title, text, closeHint, durationMs };
}

// The game's tutorial markup: the title as its head, every line of the text as a paragraph.
function markup(title, text) {
  const head = title === '' ? '' : `<head>${escapeMarkup(title)}</head>`;
  const body = text
    .split('\n')
    .map((line) => `<body>${escapeMarkup(line)}</body>`)
    .join('');
  return head + body;
}

class ResourceTutorialHint {
  // hud() is the HUD element (a NativeElement); emit(name, args) dispatches to the resources.
  constructor({ hud, emit, log }) {
    this.hud = hud;
    this.emit = emit;
    this.log = log;
    this.current = null;
    this.next = 1;
  }

  show(owner, rawOptions) {
    const options = checkOptions(rawOptions);
    const id = `kcds_hint_${this.next}`;
    this.next += 1;
    if (this.current) {
      this.end('replaced', false);
    }
    const element = this.hud();
    element.call('ShowTutorial', [
      id,
      markup(options.title, options.text),
      0,
      false,
      PRIORITY,
      LAYOUT,
      options.closeHint,
      '',
    ]);
    const hint = { id, owner, timer: null };
    if (options.durationMs > 0) {
      hint.timer = setTimeout(() => {
        if (this.current === hint) {
          this.end('timeout', true);
        }
      }, options.durationMs);
    }
    this.current = hint;
    return id;
  }

  hide(id) {
    if (!this.current || (id !== undefined && id !== this.current.id)) {
      return false;
    }
    this.end('script', true);
    return true;
  }

  isShown(id) {
    return this.current !== null && (id === undefined || id === this.current.id);
  }

  // The game's own tutorial calls on the HUD.
  receiveGame({ call, id }) {
    if (!this.current) {
      return;
    }
    if (call === 'ShowTutorial' && id !== this.current.id && !id.startsWith('kcds_hint_')) {
      this.end('replaced', false);
    } else if (
      call === 'HideCurrentTutorial' ||
      call === 'HideAllTutorials' ||
      call === 'HudHidden' ||
      call === 'TutorialsStopped'
    ) {
      this.end('player', false);
    } else if (call === 'HideTutorial' && id === this.current.id) {
      this.end('player', false);
    }
  }

  removeOwner(owner) {
    if (this.current && this.current.owner === owner) {
      this.end('resourceStopped', true);
    }
  }

  end(reason, takeDown) {
    const hint = this.current;
    this.current = null;
    clearTimeout(hint.timer);
    if (takeDown) {
      try {
        this.hud().call('HideTutorial', [hint.id]);
      } catch (error) {
        this.log('host', `tutorial hint: ${error}`);
      }
    }
    this.emit('tutorialHintClosed', [hint.id, reason]);
  }

  facade(owner) {
    const hints = this;
    return {
      showTutorialHint(options) {
        return hints.show(owner, options);
      },
      hideTutorialHint(id) {
        if (id !== undefined && typeof id !== 'string') {
          throw new TypeError('Hud.hideTutorialHint: id must be a string');
        }
        return hints.hide(id);
      },
      isTutorialHintShown(id) {
        return hints.isShown(id);
      },
    };
  }
}

module.exports = { ResourceTutorialHint, markup, checkOptions };
