'use strict';

// C# resources for tests: builds the C# resource host once (Dev/tools/build_csharp_host.py)
// and C# sources into a resource assembly, as its author would with `dotnet build`, against the
// built Kcds.Sdk.dll. Builds are cached by the sources' and the SDK's content in the system's
// temporary folder, so a test run builds each distinct resource once.

const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '../../..');
const DOTNET_DIR = path.join(ROOT, 'Backend/scripting/runtimes/js/languages/dotnet');
const CACHE = path.join(os.tmpdir(), 'kcds-csharp-tests');

let hostBuilt = false;

// The C# resource host, built if its sources changed.
function ensureHost() {
  if (hostBuilt) {
    return;
  }
  const python = process.platform === 'win32' ? 'python' : 'python3';
  execFileSync(python, [path.join(ROOT, 'Dev/tools/build_csharp_host.py')], { stdio: 'pipe' });
  hostBuilt = true;
}

function projectFile(name) {
  const sdk = path.join(DOTNET_DIR, 'Kcds.Sdk.dll');
  return `<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net9.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <AssemblyName>${name}</AssemblyName>
    <NoWarn>$(NoWarn);CA1416</NoWarn>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="Kcds.Sdk">
      <HintPath>${sdk}</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
`;
}

// Builds C# sources ({ "File.cs": code }) into an assembly; its bytes.
function buildCSharp(name, sources) {
  ensureHost();
  const hash = crypto.createHash('sha256');
  hash.update(name);
  hash.update(fs.readFileSync(path.join(DOTNET_DIR, 'Kcds.Sdk.dll')));
  for (const file of Object.keys(sources).sort()) {
    hash.update(file);
    hash.update(sources[file]);
  }
  const key = hash.digest('hex').slice(0, 16);
  const folder = path.join(CACHE, `${name}-${key}`);
  const output = path.join(folder, 'out', `${name}.dll`);
  if (!fs.existsSync(output)) {
    fs.rmSync(folder, { recursive: true, force: true });
    fs.mkdirSync(folder, { recursive: true });
    fs.writeFileSync(path.join(folder, `${name}.csproj`), projectFile(name));
    for (const [file, code] of Object.entries(sources)) {
      fs.writeFileSync(path.join(folder, file), code);
    }
    try {
      // Errors only: a build of many files (the guide's examples) has warnings enough to fill
      // the output buffer.
      execFileSync(
        'dotnet',
        [
          'build',
          '-c',
          'Release',
          '-o',
          path.join(folder, 'out'),
          '--nologo',
          '-clp:ErrorsOnly;NoSummary',
        ],
        {
          cwd: folder,
          stdio: 'pipe',
          maxBuffer: 64 * 1024 * 1024,
        },
      );
    } catch (error) {
      const text = String(error.stdout ?? '') + String(error.stderr ?? '');
      const problems = text.split('\n').filter((line) => /error/i.test(line));
      const why = problems.length > 0 ? [...new Set(problems)].join('\n') : String(error.message);
      throw new Error(`C# build of ${name} failed:\n${why}`);
    }
  }
  return fs.readFileSync(output);
}

// Writes a C# server resource: its built assembly as bin/<name>.dll and its manifest.
function csharpResource(root, name, sources, kcds = {}) {
  const bytes = buildCSharp(name.replace(/[^A-Za-z0-9]/g, '_'), sources);
  const directory = path.join(root, name);
  fs.mkdirSync(path.join(directory, 'bin'), { recursive: true });
  fs.writeFileSync(path.join(directory, 'bin', `${name}.dll`), bytes);
  fs.writeFileSync(
    path.join(directory, 'package.json'),
    JSON.stringify({ name, kcds: { server: `bin/${name}.dll`, ...kcds } }),
  );
}

module.exports = { DOTNET_DIR, ensureHost, buildCSharp, csharpResource };
