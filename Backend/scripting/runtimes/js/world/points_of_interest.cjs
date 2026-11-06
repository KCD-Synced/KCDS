'use strict';

// World.landmarks. Built by Dev/tools/world/build_points_of_interest.py.

const CATALOG = require('./points_of_interest.json');

function landmarks(level) {
  const rows = CATALOG[level] ?? [];
  return rows.map((row) => ({
    name: row.name,
    group: row.group,
    level,
    position: { ...row.position },
  }));
}

module.exports = { landmarks };
