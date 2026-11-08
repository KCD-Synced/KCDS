'use strict';

// The CharacterCreator's panel: a NativeUI screen in the empty frame
// beside the figure, while the creator's stage is up (character_creator.cjs).
//
// Tabs for the parts (Body, Face, Hair and, on a man, Beard); each tab lists the part's families
// (a skin tone, a face, a hair style) and the variants of the chosen family (a hairstyle's
// colours). Buttons and the key strip name the creator's keys as the player's layout names them.
// Every clip call answers in about a frame, so the panel is built once and only its texts change:
// each list is one text field, and its row comes from the pointer's place on a press. The texts
// are plain: the game's embedded fonts do not draw HTML text.

const { Appearances } = require('../../Backend/scripting/runtimes/js/appearance/appearances.cjs');

// The empty frame right of the figure, in the Menu movie's stage units (1920 by 1080).
const FRAME = Object.freeze({ x: 1000, y: 200, width: 760, height: 790 });
const ROWS = 11;
const LIST_TOP = 190;
const COLUMN_WIDTH = 350;
const COLORS = Object.freeze({
  text: 0xd8c8a0,
  dim: 0x8a7a5a,
  chosen: 0xffe08a,
  frame: 0xb08a4a,
  fill: 0x14100a,
});
const MARK = '> ';
const NO_MARK = '   ';
const TAB_LABELS = Object.freeze({ body: 'Body', head: 'Face', hair: 'Hair', beard: 'Beard' });

// A catalog name as a reader would say it: m_hair_001_black is "Hair 001 black".
function prettify(name) {
  const words = name
    .replace(/^[mfx]_/, '')
    .split('_')
    .filter(Boolean);
  if (words.length === 0) {
    return name;
  }
  const text = words.join(' ');
  return text.charAt(0).toUpperCase() + text.slice(1);
}

class CreatorPanel {
  constructor(NativeUI, creator, keys) {
    this.creator = creator;
    this.keys = keys;
    this.tab = 'body';
    this.column = 'families';
    this.pages = { families: 0, variants: 0 };
    this.built = false;
    this.closed = false;
    this.screen = NativeUI.createScreen();
    this.screen.on('ready', () => this.build());
  }

  close() {
    this.closed = true;
    this.screen.close();
  }

  look() {
    return this.creator.look();
  }

  tabs() {
    const look = this.look();
    return look && look.gender === 'female'
      ? ['body', 'head', 'hair']
      : ['body', 'head', 'hair', 'beard'];
  }

  // The families of the shown part: the game's default first, then the options standing on their
  // own; for the beard, the beards the face can wear.
  // Only what the creator offers (its choices): a family shows while it or an option under it is
  // offered.
  families() {
    const look = this.look();
    const allows = (name) => this.creator.allows(this.tab, name);
    if (this.tab === 'beard') {
      return [
        { label: 'None', name: '' },
        ...Appearances.beards(look.head, look.gender).map((option) => ({
          label: prettify(option.name),
          name: option.name,
        })),
      ].filter((row) => allows(row.name));
    }
    const options = Appearances.options(this.tab, look.gender);
    const offered = (family) =>
      options.some((option) => allows(option.name) && this.rootOf(option.name) === family);
    return [
      { label: 'Default', name: '' },
      ...options
        .filter((option) => option.group === null)
        .map((option) => ({ label: prettify(option.name), name: option.name })),
    ].filter((row) => (row.name === '' ? allows('') : offered(row.name)));
  }

  // The family an option belongs to: the option at the root of its groups (a skin tone's family
  // may itself belong to a body).
  rootOf(name) {
    const look = this.look();
    let option = Appearances.find(this.tab, name, look.gender);
    for (let depth = 0; option && option.group !== null && depth < 8; depth += 1) {
      option = Appearances.find(this.tab, option.group, look.gender);
    }
    return option ? option.name : '';
  }

  // The family the shown part's choice belongs to; '' for the game's default.
  family() {
    const name = this.look()[this.tab];
    if (name === '' || this.tab === 'beard') {
      return name;
    }
    return this.rootOf(name);
  }

  // The family and every option under it.
  variants() {
    const family = this.family();
    if (family === '' || this.tab === 'beard') {
      return [];
    }
    return Appearances.options(this.tab, this.look().gender)
      .filter((option) => this.rootOf(option.name) === family)
      .filter((option) => this.creator.allows(this.tab, option.name))
      .map((option) => ({ label: prettify(option.name), name: option.name }));
  }

  rows(column) {
    return column === 'families' ? this.families() : this.variants();
  }

  chosenIndex(column) {
    const look = this.look();
    const wanted = column === 'families' ? this.family() : look[this.tab];
    return this.rows(column).findIndex((row) => row.name === wanted);
  }

  // Puts a name on the shown part; a new face drops a beard it cannot wear.
  choose(name) {
    const look = this.look();
    // A family the creator does not offer itself gives its first option it offers.
    if (!this.creator.allows(this.tab, name) && this.tab !== 'beard') {
      const offered = Appearances.options(this.tab, look.gender).find(
        (option) => this.creator.allows(this.tab, option.name) && this.rootOf(option.name) === name,
      );
      if (!offered) {
        return;
      }
      name = offered.name;
    }
    look[this.tab] = name;
    if (
      this.tab === 'head' &&
      look.beard !== '' &&
      !Appearances.beards(look.head, look.gender).some((option) => option.name === look.beard)
    ) {
      look.beard = '';
    }
    this.creator.setLook(look);
    this.followChoice();
    this.refresh();
  }

  // The pages turn to the chosen rows.
  followChoice() {
    for (const column of ['families', 'variants']) {
      const index = this.chosenIndex(column);
      this.pages[column] = index < 0 ? 0 : Math.floor(index / ROWS);
    }
  }

  build() {
    if (this.closed) {
      return;
    }
    const root = this.screen.root;
    const backdrop = root.createClip();
    backdrop.draw([
      ['lineStyle', 2, COLORS.frame, 80],
      ['beginFill', COLORS.fill, 94],
      ['moveTo', 0, 0],
      ['lineTo', FRAME.width, 0],
      ['lineTo', FRAME.width, FRAME.height],
      ['lineTo', 0, FRAME.height],
      ['lineTo', 0, 0],
      ['endFill'],
    ]);
    backdrop.set({ x: FRAME.x, y: FRAME.y });
    const options = this.creator.options();
    this.title = this.text(backdrop, 24, 14, FRAME.width - 48, 48, 'DisplayFont', 34);
    this.title.setText(options.title);
    this.genderCells = this.cells(backdrop, 70, 40, 3, (index) => this.pressGender(index));
    this.tabCells = this.cells(backdrop, 120, 44, 4, (index) => this.pressTab(index));
    this.lists = {};
    for (const [column, left] of [
      ['families', 24],
      ['variants', 24 + COLUMN_WIDTH + 12],
    ]) {
      const field = this.text(backdrop, left, LIST_TOP, COLUMN_WIDTH, ROWS * 34 + 12);
      const hit = this.hitArea(backdrop, left, LIST_TOP, COLUMN_WIDTH, ROWS * 34 + 12);
      hit.on('press', () => this.pressRow(column, hit));
      const pager = this.button(backdrop, left, LIST_TOP + ROWS * 34 + 16, COLUMN_WIDTH, 34, (x) =>
        this.pressPage(column, x),
      );
      this.lists[column] = { field, pager };
    }
    // The buttons in two rows of two: the key names of some layouts are long.
    const actions = ['random', 'reset', 'accept', 'cancel'];
    this.actionCells = [
      ...this.cells(backdrop, FRAME.height - 150, 36, 2, (index) =>
        this.creator.key(actions[index]),
      ),
      ...this.cells(backdrop, FRAME.height - 112, 36, 2, (index) =>
        this.creator.key(actions[2 + index]),
      ),
    ];
    this.strip = this.text(
      backdrop,
      24,
      FRAME.height - 62,
      FRAME.width - 48,
      40,
      'DefaultFont',
      18,
    );
    this.lineHeight = this.measure(this.lists.families.field);
    this.built = true;
    this.followChoice();
    this.refresh();
  }

  text(parent, x, y, width, height, font = 'DefaultFont', size = 24) {
    const field = parent.createText({ x, y, width, height });
    field.setTextStyle({ font, size, color: 0xd8c8a0 });
    return field;
  }

  // An invisible area that takes presses.
  hitArea(parent, x, y, width, height) {
    const hit = parent.createClip();
    hit.draw([
      ['beginFill', 0x000000, 0],
      ['moveTo', 0, 0],
      ['lineTo', width, 0],
      ['lineTo', width, height],
      ['lineTo', 0, height],
      ['endFill'],
    ]);
    hit.set({ x, y });
    return hit;
  }

  // A text with a hit area over it; onPress gets the pointer's x across it (0..1).
  button(parent, x, y, width, height, onPress) {
    const field = this.text(parent, x, y, width, height);
    const hit = this.hitArea(parent, x, y, width, height);
    hit.on('press', () => {
      const pointer = hit.get('_xmouse');
      onPress(typeof pointer === 'number' ? pointer / width : 0);
    });
    return field;
  }

  // A row of count equal cells across the panel, each a text with its own hit area; onPress gets
  // the cell's index.
  cells(parent, y, height, count, onPress, size = 24) {
    const width = (FRAME.width - 48) / count;
    const fields = [];
    for (let index = 0; index < count; index += 1) {
      const x = 24 + index * width;
      fields.push(this.text(parent, x, y, width, height, 'DefaultFont', size));
      this.hitArea(parent, x, y, width, height).on('press', () => onPress(index));
    }
    return fields;
  }

  // The height of a list line, from the field's text height over ROWS lines.
  measure(field) {
    field.setText(Array(ROWS).fill('X').join('\n'));
    const height = field.get('textHeight');
    return typeof height === 'number' && height > 0 ? height / ROWS : 34;
  }

  key(name) {
    if (!this.built) {
      return;
    }
    if (name === 'tab') {
      const tabs = this.tabs();
      this.tab = tabs[(tabs.indexOf(this.tab) + 1) % tabs.length];
      this.creator.frame(this.tab);
      this.column = 'families';
      this.followChoice();
      this.refresh();
    } else if (name === 'left' || name === 'right') {
      this.column = name === 'left' || this.variants().length === 0 ? 'families' : 'variants';
      this.refresh();
    } else if (name === 'up' || name === 'down') {
      const rows = this.rows(this.column);
      if (rows.length > 0) {
        const at = this.chosenIndex(this.column);
        const next = (Math.max(at, 0) + (name === 'down' ? 1 : rows.length - 1)) % rows.length;
        this.choose(rows[next].name);
      }
    }
  }

  pressGender(index) {
    const wanted = ['male', 'female'][index];
    if (wanted && this.look().gender !== wanted && this.creator.switchGender()) {
      this.afterLookReset();
    }
  }

  pressTab(index) {
    const tabs = this.tabs();
    if (index < tabs.length) {
      this.tab = tabs[index];
      this.creator.frame(this.tab);
      this.column = 'families';
      this.followChoice();
      this.refresh();
    }
  }

  pressRow(column, hit) {
    const pointer = hit.get('_ymouse');
    if (typeof pointer !== 'number') {
      return;
    }
    const row = Math.floor(pointer / this.lineHeight);
    const rows = this.rows(column);
    const index = this.pages[column] * ROWS + row;
    if (row >= 0 && row < ROWS && index < rows.length) {
      this.column = column;
      this.choose(rows[index].name);
    }
  }

  pressPage(column, x) {
    const pages = Math.max(1, Math.ceil(this.rows(column).length / ROWS));
    const step = x < 0.5 ? -1 : 1;
    this.pages[column] = (this.pages[column] + step + pages) % pages;
    this.refresh();
  }

  // After a gender switch, a random look or a reset.
  afterLookReset() {
    if (!this.tabs().includes(this.tab)) {
      this.tab = 'body';
      this.creator.frame(this.tab);
    }
    this.followChoice();
    this.refresh();
  }

  refresh() {
    if (!this.built || this.closed || !this.look()) {
      return;
    }
    const look = this.look();
    const options = this.creator.options();
    const keys = this.keys;
    const mark = (chosen, label) => (chosen ? `[${label}]` : ` ${label} `);
    const genders = options.genders === 'both' ? ['male', 'female'] : [options.genders];
    const genderLabels = genders.map((gender) =>
      mark(look.gender === gender, gender === 'male' ? 'Man' : 'Woman'),
    );
    if (options.genders === 'both') {
      genderLabels.push(`${keys.gender}: switch`);
    }
    this.genderCells.forEach((cell, index) => cell.setText(genderLabels[index] ?? ''));
    const tabs = this.tabs();
    this.tabCells.forEach((cell, index) =>
      cell.setText(
        index < tabs.length ? mark(tabs[index] === this.tab, TAB_LABELS[tabs[index]]) : '',
      ),
    );
    for (const column of ['families', 'variants']) {
      const rows = this.rows(column);
      const chosen = this.chosenIndex(column);
      const start = this.pages[column] * ROWS;
      const lines = rows
        .slice(start, start + ROWS)
        .map((row, offset) => (start + offset === chosen ? MARK : NO_MARK) + row.label);
      const { field, pager } = this.lists[column];
      field.setText(lines.join('\n'));
      const active = this.column === column;
      if (this.lists[column].active !== active) {
        this.lists[column].active = active;
        field.setTextStyle({ color: active ? COLORS.chosen : COLORS.text });
      }
      const pages = Math.ceil(rows.length / ROWS);
      pager.setText(pages > 1 ? `<<    ${this.pages[column] + 1} / ${pages}    >>` : '');
    }
    if (!this.staticTexts) {
      this.staticTexts = true;
      const actions = [
        `${keys.random}: Random`,
        `${keys.reset}: Reset`,
        `${keys.accept}: Accept`,
        `${keys.cancel}: Cancel`,
      ];
      this.actionCells.forEach((cell, index) => cell.setText(actions[index]));
      this.strip.setText(
        `${keys.tab}: next tab    ${keys.up} ${keys.down}: choose    ${keys.left} ${keys.right}: family / variant`,
      );
    }
  }
}

function createCreatorPanel(NativeUI, creator, keys) {
  return new CreatorPanel(NativeUI, creator, keys);
}

module.exports = { createCreatorPanel, prettify };
