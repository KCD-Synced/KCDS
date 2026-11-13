'use strict';

// Where this machine's kcds-bots engine is (the native, multi-threaded bot client built from
// Backend/integration/local/bots): given with --engine or KCDS_BOTS_ENGINE, from the platform
// package npm installed with @kcds/bots (@kcds/bots-darwin-arm64, -darwin-x64, -linux-x64,
// -win32-x64), or from a build of this repository (build/ next to it).

const fs = require('node:fs');
const path = require('node:path');

const EXECUTABLE = process.platform === 'win32' ? 'kcds-bots.exe' : 'kcds-bots';

// The platform package's name for this machine (npm installs only the one that fits it).
function platformPackage(platform = process.platform, arch = process.arch) {
  return `@kcds/bots-${platform}-${arch}`;
}

// The build folders of a repository checkout an engine may be in, newest layout first.
function repositoryCandidates(repository) {
  const folders = ['build', 'build-release', 'build-win', 'build-linux', 'build-mac'];
  const candidates = [];
  for (const folder of folders) {
    candidates.push(path.join(repository, folder, 'Backend', 'integration', EXECUTABLE));
    candidates.push(path.join(repository, folder, 'Backend', 'integration', 'Release', EXECUTABLE));
  }
  return candidates;
}

// The engine's path, or an Error that says how to get one.
function locateEngine({ engine, env = process.env, resolve = require.resolve } = {}) {
  const given = engine || env.KCDS_BOTS_ENGINE;
  if (given) {
    if (!fs.existsSync(given)) {
      throw new Error(`kcds-bots engine not found at ${given}`);
    }
    return given;
  }
  try {
    const manifest = resolve(`${platformPackage()}/package.json`);
    const installed = path.join(path.dirname(manifest), 'bin', EXECUTABLE);
    if (fs.existsSync(installed)) {
      return installed;
    }
  } catch {
    // Not installed: a checkout of the repository, perhaps.
  }
  const repository = path.resolve(__dirname, '..', '..', '..', '..');
  for (const candidate of repositoryCandidates(repository)) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error(
    `No kcds-bots engine for ${process.platform}-${process.arch}. Install @kcds/bots (it brings ` +
      `${platformPackage()}), or build it from the KCDS repository: cmake -S . -B build ` +
      '-DKCD2S_BUILD_GAME_CLIENT=OFF && cmake --build build --target kcds-bots, or name one with ' +
      '--engine <path> (KCDS_BOTS_ENGINE).',
  );
}

module.exports = { locateEngine, platformPackage, EXECUTABLE };
