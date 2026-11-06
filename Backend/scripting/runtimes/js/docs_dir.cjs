'use strict';

// Where the documentation (github.com/KCD-Synced/KCDS-DOCS) is checked out: KCDS_DOCS_DIR, else a
// KCDS-DOCS folder beside this repository. null when there is none: the tests of the guides'
// examples then skip.

const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '../../../..');

function docsDir() {
  const dir = process.env.KCDS_DOCS_DIR
    ? path.resolve(process.env.KCDS_DOCS_DIR)
    : path.resolve(ROOT, '..', 'KCDS-DOCS');
  return fs.existsSync(path.join(dir, 'README.md')) ? dir : null;
}

const SKIP = 'needs the KCDS-DOCS checkout (KCDS_DOCS_DIR or ../KCDS-DOCS)';

module.exports = { docsDir, SKIP };
