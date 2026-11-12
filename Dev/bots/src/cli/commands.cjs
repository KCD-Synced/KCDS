'use strict';

// The commands of kcds-bots; each returns the process's exit code.

const fs = require('node:fs');

const args = require('./args.cjs');
const { runScenario } = require('../run/run.cjs');
const { mergeResults } = require('../results/merge.cjs');
const { renderReport, DEFAULT_THRESHOLDS } = require('../report/report.cjs');
const { compareRuns } = require('../compare/compare.cjs');

function readResult(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

async function main(argv, { log = console.log, logError = console.error } = {}) {
  const [command, ...rest] = argv;
  if (!command || command === '--help' || command === '-h' || command === 'help') {
    log(args.USAGE);
    return 0;
  }
  if (command === '--version') {
    log(require('../../package.json').version);
    return 0;
  }
  try {
    if (command === 'scenarios') {
      log(args.listScenarios().join('\n'));
      return 0;
    }
    if (command === 'report') {
      const options = args.parseReport(rest);
      const thresholds = options.thresholds
        ? { ...DEFAULT_THRESHOLDS, ...readResult(options.thresholds) }
        : DEFAULT_THRESHOLDS;
      const text = renderReport(options.results.map(readResult), {
        thresholds,
        title: options.title,
      });
      if (options.out) {
        fs.writeFileSync(options.out, text);
        log(`kcds-bots: report ${options.out}`);
      } else {
        log(text);
      }
      return 0;
    }
    if (command === 'merge') {
      const options = args.parseMerge(rest);
      const merged = mergeResults(options.results.map(readResult));
      fs.writeFileSync(options.out, `${JSON.stringify(merged, null, 1)}\n`);
      log(
        `kcds-bots: ${merged.clients.bots} bots of ${options.results.length} results in ${options.out}`,
      );
      return 0;
    }
    if (command === 'compare') {
      const options = args.parseCompare(rest);
      const comparison = compareRuns(
        readResult(options.baseline),
        options.results.map(readResult),
        { allowance: options.allowance },
      );
      log(comparison.lines.join('\n'));
      log(comparison.ok ? 'kcds-bots: no regression' : 'kcds-bots: REGRESSION');
      return comparison.ok ? 0 : 1;
    }
    const runArguments = command === 'run' ? rest : argv;
    const result = await runScenario(args.parseRun(runArguments), { log, logError });
    return result.interrupted ? 130 : 0;
  } catch (error) {
    logError(`kcds-bots: ${error.message}`);
    return 2;
  }
}

module.exports = { main };
