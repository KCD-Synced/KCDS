#!/usr/bin/env node
'use strict';

// kcds-bots: load-test a KCDS server with bots. The commands are in src/cli.
const { main } = require('./src/cli/commands.cjs');

main(process.argv.slice(2)).then((code) => {
  process.exitCode = code;
});
