# @kcds/types

TypeScript declarations of the KCDS SDK, for the server and client scripts of KCDS resources. KCDS is a
multiplayer mod for Kingdom Come: Deliverance II.

This version declares the SDK of KCDS {version}. Install the version your server runs:

```text
npm install --save-dev @kcds/types@{npm_version}
```

A KCDS hotfix x.y.z.w is the package version x.y.z-hotfix.w.

## Server and client

Server and client scripts have different APIs, so a resource is checked as two TypeScript projects. Each one
names its side in `types`.

`tsconfig.server.json`:

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "commonjs",
    "lib": ["ES2022"],
    "types": ["@kcds/types/server"],
    "strict": true,
    "rootDir": "src/server",
    "outDir": "dist/server"
  },
  "include": ["src/server/**/*.ts"]
}
```

`tsconfig.client.json` is the same, with `client` in place of `server`.

Build both with `tsc -p tsconfig.server.json && tsc -p tsconfig.client.json`. Using a client type in a server file is
a compile error.

## Files

| File              | Declares                                                        |
| ----------------- | --------------------------------------------------------------- |
| `server.d.ts`     | a server project's entry: `shared.d.ts` and `server-api.d.ts`   |
| `client.d.ts`     | a client project's entry: `shared.d.ts` and `client-api.d.ts`   |
| `shared.d.ts`     | what both sides have: `console`, `require` and the named types  |
| `server-api.d.ts` | what server scripts have: the SDK's types and events            |
| `client-api.d.ts` | what client scripts have: the SDK's types and events            |

The guide, with a whole example resource: {docs}. Every member of the SDK, with examples:
https://kcdsynced.com/docs.
