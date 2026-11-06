# wasmoon 1.16.0 (vendored)

Lua 5.4 compiled to WebAssembly, from the npm package `wasmoon` 1.16.0 (MIT, see LICENSE): `index.js` is
its `dist/index.js` and `glue.wasm` its `dist/glue.wasm`. Lua resources run on it, on the server
(Node) and in the client's resource worker (WebView2), through `../../lua_host.cjs`.

## Local changes

- `LuaEngine.callByteCode` (`doString`, `doFile`): pops the results it moved onto the main stack.
  Upstream left them there without growing the stack, so about 60 calls wrote past it ("memory
  access out of bounds"). The resource runtime runs its code through its own path, which never
  had this; tests and tools that use `doString` did.

- `FunctionTypeExtension.pushValue`: a JavaScript function (alone, or a method with the object it
  is read from) is pushed as the same Lua closure each time while Lua holds it, through a
  weak-valued registry table (`kcds_functions`), so `Exports.get(...) == Exports.get(...)` and
  `Chat.sendToAll == Chat.sendToAll`. Upstream made a new closure each time.

- `FunctionTypeExtension.getValue`: the same Lua function becomes the same JavaScript function
  while that one lives (kept by the Lua function's address, held weakly), so a handler passed to
  `Events.on` and later to `Events.off` is one function. Upstream made a new one each time.

- `Thread.getValue`: nil is `undefined` (upstream: `null`), so a nil argument is "not given" and
  JavaScript defaults apply (`Horse.spawn(position, nil, nil, name)`); a script passes
  JavaScript's `null` with the `null` value.
- `FunctionTypeExtension`'s call wrapper: a Lua call's trailing nils are dropped (arguments not
  given), so `arguments.length` and rest parameters see what the script gave.

- `LuaWasm.initialize` (and the glue's `Ra`/`Ta`): the location may be the module's bytes, a
  `Uint8Array`, which are compiled as they are, without fetch or file reads. The client's resource
  worker may load nothing (its page's CSP is `connect-src 'none'`), so its bundle carries the bytes.

- `Thread.pushValue`: pushes `null` as nil, as it does `undefined`. Upstream pushes it as the
  `null` object when the engine has `injectObjects` (and cannot push it at all without), so a
  member answering null would not be nil in Lua. KCDS engines have `injectObjects` for the `null`
  value a script passes and for `Promise.create`.
