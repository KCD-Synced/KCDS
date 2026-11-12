#!/usr/bin/env node
'use strict';

// Makes the platform package of one kcds-bots engine (@kcds/bots-<platform>-<arch>): a folder with
// its package.json (npm installs it only on that system and processor) and bin/kcds-bots, which
// @kcds/bots finds (src/engine/locate.cjs). The release workflow packs one for each system it
// builds the engine on and publishes them with @kcds/bots.
//
//   node pack-platform.cjs --engine <built kcds-bots> --platform darwin --arch arm64 \
//        --version 1.0.6 --out <folder>

const fs = require('node:fs');
const path = require('node:path');

const PLATFORMS = Object.freeze({ darwin: 'macOS', linux: 'Linux', win32: 'Windows' });
const ARCHES = Object.freeze(['arm64', 'x64']);

function packPlatform({ engine, platform, arch, version, out }) {
  if (!PLATFORMS[platform] || !ARCHES.includes(arch)) {
    throw new Error(`no platform package for ${platform}-${arch}`);
  }
  if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version)) {
    throw new Error(`version ${version} is not semver`);
  }
  if (!fs.existsSync(engine)) {
    throw new Error(`no engine at ${engine}`);
  }
  const name = `@kcds/bots-${platform}-${arch}`;
  const executable = platform === 'win32' ? 'kcds-bots.exe' : 'kcds-bots';
  const folder = path.join(out, `bots-${platform}-${arch}`);
  fs.rmSync(folder, { recursive: true, force: true });
  fs.mkdirSync(path.join(folder, 'bin'), { recursive: true });
  fs.copyFileSync(engine, path.join(folder, 'bin', executable));
  fs.chmodSync(path.join(folder, 'bin', executable), 0o755);
  const manifest = {
    name,
    version,
    description: `The kcds-bots engine for ${PLATFORMS[platform]} (${arch}); installed by @kcds/bots.`,
    homepage: 'https://kcdsynced.com/docs/kcds-bots',
    license: 'PolyForm-Strict-1.0.0',
    os: [platform],
    cpu: [arch],
    files: ['bin'],
  };
  fs.writeFileSync(path.join(folder, 'package.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  fs.writeFileSync(
    path.join(folder, 'README.md'),
    `# ${name}\n\nThe multi-threaded kcds-bots engine for ${PLATFORMS[platform]} (${arch}). ` +
      'Install @kcds/bots, not this: it brings the engine of your machine.\n',
  );
  return { name, folder };
}

function main(argv) {
  const options = {};
  for (let index = 0; index < argv.length; index += 2) {
    options[argv[index].replace(/^--/, '')] = argv[index + 1];
  }
  const packed = packPlatform(options);
  console.log(`${packed.name}: ${packed.folder}`);
}

if (require.main === module) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    console.error(`pack-platform: ${error.message}`);
    process.exitCode = 2;
  }
}

module.exports = { packPlatform };
