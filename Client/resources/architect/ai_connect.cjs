'use strict';

// The AI tab of World Architect and the Asset/Animation Studio (architect/ai_bridge.hpp,
// Dev/tools/architect-mcp): whether an assistant is connected, and what to give an AI app to
// connect one. One MCP server serves both editors. Each row shows a text and copies another:
// the token shows as dots and the Windows user's folder as %USERPROFILE% (a stream or a screenshot
// shows neither).

// ai: the game client's answer to aiBridge() ({ port, listening, connected, server, token }), or
// null when it does not answer.
function aiConnectView(ai) {
  if (!ai) {
    return {
      state: 'off',
      status: 'The game client does not answer (the editors’ local mode only).',
      rows: [],
    };
  }
  let state = 'waiting';
  let status = `Waiting for an AI assistant on 127.0.0.1:${ai.port}`;
  if (ai.connected) {
    state = 'connected';
    status = 'An AI assistant is connected: it works in this editor as you do (Undo undoes it).';
  } else if (!ai.listening) {
    state = 'off';
    status = `Not listening: port ${ai.port} is taken (set KCDS_ARCHITECT_AI_PORT).`;
  }
  const server = String(ai.server ?? '');
  const home = /^[A-Za-z]:\\Users\\[^\\]+/;
  const shownServer = server.replace(home, '%USERPROFILE%');
  const command = (path) => `claude mcp add kcds-world-architect -- node "${path}"`;
  const entry = (path) =>
    JSON.stringify({ mcpServers: { 'kcds-world-architect': { command: 'node', args: [path] } } });
  const token = String(ai.token ?? '');
  return {
    state,
    status,
    note: 'Needs Node.js 18 or newer. The server finds this game and its token by itself.',
    rows: [
      { label: 'Claude Code', shown: command(shownServer), text: command(server) },
      { label: 'Other AI apps (MCP)', shown: entry(shownServer), text: entry(server) },
      {
        label: 'Token',
        shown: token ? '•'.repeat(24) + '   (hidden: Copy copies it)' : 'none yet',
        text: token,
      },
    ],
  };
}

module.exports = { aiConnectView };
