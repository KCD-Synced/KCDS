'use strict';

// A coarse grid over the XY plane that tells which areas can contain a point. A body
// that moved asks the grid for the areas registered in its cell and tests only those, so the work
// of a position update grows with the areas near the body and not with every area on the server.
//
// An area is entered into every cell its bounds touch. One that would take more than MAX_CELLS
// cells (a sphere across the whole map, say) goes on a short list that every query includes
// instead.

const CELL_SIZE = 64;
const MAX_CELLS = 1024;
// Cell coordinates are kept inside 16 bits so a cell is one number; a position beyond that lands
// in the outermost cell, where the exact containment test still decides.
const CELL_LIMIT = 32767;
const NONE = Object.freeze([]);

function cellOf(coordinate) {
  const cell = Math.floor(coordinate / CELL_SIZE);
  return Math.max(-CELL_LIMIT, Math.min(CELL_LIMIT, cell));
}

function keyOf(cellX, cellY) {
  return (cellX + CELL_LIMIT + 1) * 65536 + (cellY + CELL_LIMIT + 1);
}

class AreaGrid {
  constructor() {
    // cell key -> the areas whose bounds touch that cell
    this.cells = new Map();
    // The areas too large for the cells.
    this.wide = [];
    // area -> the keys it was entered under, or null for the wide list
    this.entered = new Map();
  }

  add(area, bounds) {
    this.remove(area);
    const fromX = cellOf(bounds.min.x);
    const toX = cellOf(bounds.max.x);
    const fromY = cellOf(bounds.min.y);
    const toY = cellOf(bounds.max.y);
    if ((toX - fromX + 1) * (toY - fromY + 1) > MAX_CELLS) {
      this.wide.push(area);
      this.entered.set(area, null);
      return;
    }
    const keys = [];
    for (let cellX = fromX; cellX <= toX; cellX += 1) {
      for (let cellY = fromY; cellY <= toY; cellY += 1) {
        const key = keyOf(cellX, cellY);
        keys.push(key);
        const list = this.cells.get(key);
        if (list) {
          list.push(area);
        } else {
          this.cells.set(key, [area]);
        }
      }
    }
    this.entered.set(area, keys);
  }

  remove(area) {
    if (!this.entered.has(area)) {
      return;
    }
    const keys = this.entered.get(area);
    this.entered.delete(area);
    if (keys === null) {
      this.wide.splice(this.wide.indexOf(area), 1);
      return;
    }
    for (const key of keys) {
      const list = this.cells.get(key);
      list.splice(list.indexOf(area), 1);
      if (list.length === 0) {
        this.cells.delete(key);
      }
    }
  }

  // The areas whose bounds touch the cell of (x, y); the caller tests them exactly. The array is
  // the grid's own and must not be changed or kept.
  near(x, y) {
    return this.cells.get(keyOf(cellOf(x), cellOf(y))) ?? NONE;
  }
}

module.exports = { AreaGrid, CELL_SIZE };
