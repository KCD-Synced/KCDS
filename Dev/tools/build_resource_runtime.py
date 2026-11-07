"""Bundle only our trusted client SDK bootstrap; server resources are never baked in."""

import argparse
import base64
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULES = [
    "Client/resources/sealed_files.cjs",
    "Client/resources/bundle_cache.cjs",
    "Client/resources/runtime.cjs",
    "Client/resources/entities.cjs",
    "Client/resources/entity_transfer.cjs",
    "Client/resources/web.cjs",
    "Client/resources/loading_screen.cjs",
    "Client/resources/input.cjs",
    "Client/resources/screen_effects.cjs",
    "Client/resources/hud.cjs",
    "Client/resources/chat.cjs",
    "Client/resources/action_hints.cjs",
    "Client/resources/world_map.cjs",
    "Client/resources/book.cjs",
    "Client/resources/vendor_screen.cjs",
    "Client/resources/sync_channel.cjs",
    "Client/resources/native_ui.cjs",
    "Client/resources/native_host_movie.cjs",
    "Client/resources/character_creator.cjs",
    "Client/resources/creator_panel.cjs",
    "Client/resources/tutorial_hint.cjs",
    "Client/resources/noclip.cjs",
    "Client/resources/local_voice.cjs",
    "Client/resources/keybinds.cjs",
    "Client/resources/camera.cjs",
    "Client/resources/render.cjs",
    "Client/resources/vfx.cjs",
    "Client/resources/lights.cjs",
    "Client/resources/prefabs.cjs",
    "Client/resources/collision.cjs",
    "Client/resources/terrain.cjs",
    "Client/resources/vegetation.cjs",
    "Client/resources/architect/catalogue.cjs",
    "Client/resources/architect/theme.cjs",
    "Client/resources/architect/glyphs.cjs",
    "Client/resources/architect/icons.cjs",
    "Client/resources/architect/marks.cjs",
    "Client/resources/architect/shell.cjs",
    "Client/resources/architect/collision_layer.cjs",
    "Client/resources/architect/house_fronts.cjs",
    "Client/resources/architect/ai_connect.cjs",
    "Client/resources/architect/ai_commands.cjs",
    "Client/resources/architect/materials.cjs",
    "Client/resources/architect/material_work.cjs",
    "Client/resources/architect/interiors.cjs",
    "Client/resources/architect/opening.cjs",
    "Dev/tools/architect-mcp/models/bytes.cjs",
    "Dev/tools/architect-mcp/models/cgf_writer.cjs",
    "Dev/tools/architect-mcp/models/open_core.cjs",
    "Client/resources/architect/blender_export.cjs",
    "Client/resources/architect/server_export.cjs",
    "Client/resources/architect/movers.cjs",
    "Client/resources/architect/editor.cjs",
    "Client/resources/studio/glyphs.cjs",
    "Client/resources/studio/animation.cjs",
    "Client/resources/studio/clip_bake.cjs",
    "Client/resources/studio/animation_tools.cjs",
    "Client/resources/studio/lash_keys.cjs",
    "Client/resources/studio/lash_motion.cjs",
    "Client/resources/studio/lash_design.cjs",
    "Client/resources/studio/timeline.cjs",
    "Client/resources/studio/outfits.cjs",
    "Client/resources/studio/placing.cjs",
    "Client/resources/studio/shapes.cjs",
    "Client/resources/studio/camera.cjs",
    "Client/resources/studio/editor.cjs",
    "Client/resources/world.cjs",
    "Client/resources/previews.cjs",
    "Backend/scripting/runtimes/js/props/catalog.cjs",
    "Backend/scripting/runtimes/js/props/catalog.json",
    "Backend/scripting/runtimes/js/props/collision.json",
    "Backend/scripting/runtimes/js/props/prefabs.json",
    "Client/resources/local_npcs.cjs",
    "Client/resources/gizmos.cjs",
    "Client/resources/nameplates.cjs",
    "Client/resources/discord.cjs",
    "Client/resources/game.cjs",
    "Client/resources/audio.cjs",
    "Client/resources/assets.cjs",
    "Client/resources/follow.cjs",
    "Client/resources/tabletop.cjs",
    "Client/resources/emotes.cjs",
    "Client/resources/blips.cjs",
    "Client/resources/inventory.cjs",
    "Client/resources/progression.cjs",
    "Client/resources/buffs.cjs",
    "Client/resources/worker.cjs",
    "Backend/scripting/runtimes/js/services.cjs",
    "Backend/scripting/runtimes/js/sdk_namespace.cjs",
    "Backend/scripting/runtimes/js/languages/lua_host.cjs",
    "Backend/scripting/runtimes/js/languages/csharp_host.cjs",
    "Backend/scripting/runtimes/js/languages/vendor/wasmoon/index.js",
    "Backend/scripting/runtimes/js/languages/vendor/wasmoon/glue.wasm",
    "Backend/scripting/runtimes/js/shared_state.cjs",
    "Backend/scripting/runtimes/js/math.cjs",
    "Backend/scripting/runtimes/js/camera/shots.cjs",
    "Backend/scripting/runtimes/js/vfx/vfx_values.cjs",
    "Backend/scripting/runtimes/js/vfx/catalog.json",
    "Backend/scripting/runtimes/js/world/mesh_categories.cjs",
    "Backend/scripting/runtimes/js/lights/light_values.cjs",
    "Backend/scripting/runtimes/js/appearance/appearances.cjs",
    "Backend/scripting/runtimes/js/npcs/npc_values.cjs",
    "Backend/scripting/runtimes/js/stats/player_stats.cjs",
    "Backend/scripting/runtimes/js/posture/player_posture.cjs",
    "Backend/scripting/runtimes/js/progression/catalog.cjs",
    "Backend/scripting/runtimes/js/progression/player_progression.cjs",
    "Backend/scripting/runtimes/js/progression/catalog.json",
    "Backend/scripting/runtimes/js/buffs/catalog.cjs",
    "Backend/scripting/runtimes/js/buffs/player_buffs.cjs",
    "Backend/scripting/runtimes/js/buffs/catalog.json",
    "Backend/scripting/runtimes/js/markers/blip_types.cjs",
    "Backend/scripting/runtimes/js/inventory/catalog.cjs",
    "Backend/scripting/runtimes/js/inventory/catalog.json",
    "Backend/scripting/runtimes/js/appearance/catalog.json",
]


def generate():
    # Compiled WebAssembly (the Lua engine) travels as base64 and loads as its bytes.
    modules = {}
    for name in MODULES:
        if name.endswith(".wasm"):
            modules[name] = base64.b64encode((ROOT / name).read_bytes()).decode("ascii")
        else:
            modules[name] = (ROOT / name).read_text(encoding="utf8")
    bootstrap = """
'use strict';
const sources = SOURCES;
const modules = new Map();
function load(name) {
  if (modules.has(name)) {
    return modules.get(name).exports;
  }
  if (!Object.hasOwn(sources, name)) {
    throw Error('Unknown trusted SDK module');
  }
  const module = { exports: {} };
  modules.set(name, module);
  const require = (request) => {
    const parts = name.split('/').slice(0, -1);
    for (const part of request.split('/')) {
      if (part === '..') {
        parts.pop();
      } else if (part !== '.' && part !== '') {
        parts.push(part);
      }
    }
    return load(parts.join('/'));
  };
  if (name.endsWith('.json')) {
    module.exports = JSON.parse(sources[name]);
  } else if (name.endsWith('.wasm')) {
    const text = atob(sources[name]);
    const bytes = new Uint8Array(text.length);
    for (let index = 0; index < text.length; index += 1) {
      bytes[index] = text.charCodeAt(index);
    }
    module.exports = bytes;
  } else {
    new Function('module', 'exports', 'require', sources[name])(
      module, module.exports, require
    );
  }
  return module.exports;
}
load('Client/resources/worker.cjs');
""".replace(
        "SOURCES", json.dumps(modules, ensure_ascii=True)
    )
    # The trusted page never evaluates downloaded resource code. A worker has
    # no DOM, filesystem or native host objects; its inherited CSP denies network
    # except the runtime's own .NET files (C# resources), which the
    # runtime program serves from its dotnet folder (Client/resources/runtime_files.hpp).
    html = """<!doctype html><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' blob: https://runtime.kcds.invalid/dotnet/; worker-src blob:; connect-src https://runtime.kcds.invalid/dotnet/; frame-src 'none';">
<script>
const worker = new Worker(
  URL.createObjectURL(new Blob([BOOTSTRAP], { type: 'text/javascript' }))
);
let heartbeat = Date.now();
// Synchronous requests (Client/resources/sync_channel.cjs): the worker waits on this buffer
// while the game client answers; this page is cross-origin isolated so it can share it.
const syncBuffer = self.crossOriginIsolated ? new SharedArrayBuffer(8 + 262144) : null;
const syncState = syncBuffer ? new Int32Array(syncBuffer, 0, 2) : null;
const syncEncoder = new TextEncoder();
if (syncBuffer) {
  worker.postMessage({ op: 'syncChannel', buffer: syncBuffer });
}
function syncReply({ id, value }) {
  if (!syncState || !Number.isInteger(id)) return;
  let bytes = syncEncoder.encode(JSON.stringify({ v: value === undefined ? null : value }));
  if (bytes.length > 262144) bytes = syncEncoder.encode('{"v":null}');
  new Uint8Array(syncBuffer, 8, 262144).set(bytes);
  Atomics.store(syncState, 1, bytes.length);
  Atomics.store(syncState, 0, id);
  Atomics.notify(syncState, 0);
}
// A cross-origin isolated page can start before WebView2 injects chrome.webview; messages for
// the game client wait in order until this page listens for its answers.
const outbox = [];
let attached = false;
function toHost(message) {
  if (attached) {
    chrome.webview.postMessage(message);
  } else {
    outbox.push(message);
  }
}
function attachHost() {
  if (!self.chrome || !chrome.webview) {
    setTimeout(attachHost, 10);
    return;
  }
  chrome.webview.addEventListener('message', ({ data }) => {
    if (data && data.op === 'syncReply') {
      syncReply(data);
      return;
    }
    worker.postMessage(data);
  });
  attached = true;
  for (const message of outbox.splice(0)) {
    chrome.webview.postMessage(message);
  }
}
worker.onmessage = ({ data }) => {
  if (data.op === 'heartbeat') {
    heartbeat = Date.now();
    return;
  }
  toHost(data);
};
worker.onerror = (event) => {
  toHost({ op: 'log', resource: 'host', text: event.message });
};
attachHost();
setInterval(() => {
  if (Date.now() - heartbeat > 10000) {
    worker.terminate();
    toHost({ op: 'runtimeFailed', text: 'Client resource worker stalled' });
  }
}, 1000);
</script>""".replace(
        "BOOTSTRAP", json.dumps(bootstrap, ensure_ascii=True)
    )
    return html


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    html = generate()
    # Escape closing script tags in SDK strings so they cannot end the trusted
    # page's script element. No server-provided script participates in this build.
    closing = html.rfind("</script>")
    html = html[:closing].replace("</script", "<\\/script") + html[closing:]
    chunks = [html[index : index + 4096] for index in range(0, len(html), 4096)]
    header = "#pragma once\ninline constexpr char kResourceRuntimeHtml[] =\n"
    header += "\n".join('R"KCDS(' + chunk + ')KCDS"' for chunk in chunks) + ";\n"
    if ')KCDS"' in html:
        raise ValueError("Raw-string terminator in trusted SDK source")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf8")


if __name__ == "__main__":
    main()
