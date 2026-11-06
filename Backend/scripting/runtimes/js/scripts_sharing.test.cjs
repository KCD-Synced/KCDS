'use strict';

// Sharing between resources: exports, imports and messages. Each scenario runs with its scripts
// in JavaScript, in Lua and in C#, and across languages.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  LANGUAGES,
  ENTRY,
  entryContent,
  serverResource,
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const LEDGER = {
  javascript: `
const balances = new Map();
function balance(playerId) {
  return balances.get(playerId) ?? 0;
}
function pay(playerId, amount) {
  const next = balance(playerId) + amount;
  if (next < 0) {
    return false;
  }
  balances.set(playerId, next);
  return true;
}
kcds.core.Exports.register('balance', balance);
kcds.core.Exports.register('pay', pay);
console.log('register undeclared ' + kcds.core.Exports.register('secret', 1));
`,
  lua: `
local balances = {}
local function balance(playerId)
  return balances[playerId] or 0
end
local function pay(playerId, amount)
  local after = balance(playerId) + amount
  if after < 0 then
    return false
  end
  balances[playerId] = after
  return true
end
kcds.core.Exports.register("balance", balance)
kcds.core.Exports.register("pay", pay)
print("register undeclared " .. tostring(kcds.core.Exports.register("secret", 1)))
`,
  csharp: `
public class Ledger : Script
{
    private readonly Dictionary<double, double> balances = new();

    private double Balance(double playerId)
    {
        return balances.GetValueOrDefault(playerId);
    }

    private bool Pay(double playerId, double amount)
    {
        var after = Balance(playerId) + amount;
        if (after < 0)
        {
            return false;
        }
        balances[playerId] = after;
        return true;
    }

    public override void OnStart()
    {
        Exports.Register("balance", new Func<double, double>(Balance));
        Exports.Register("pay", new Func<double, double, bool>(Pay));
        var stored = Exports.Register("secret", 1);
        Log.Info("register undeclared " + (stored ? "true" : "false"));
    }
}
`,
};

const STABLES = {
  javascript: `
let heldPay = null;
kcds.core.Events.on('playerCommand', (player, command) => {
  if (command === 'grant') {
    kcds.core.Exports.get('ledger', 'pay')(player.id, 600);
    return;
  }
  if (command !== 'buyhorse') {
    return;
  }
  const payFrom = kcds.core.Exports.get('ledger', 'pay');
  heldPay = heldPay ?? payFrom;
  if (!payFrom(player.id, -500)) {
    kcds.core.Chat.sendToPlayer(player, 'You cannot afford a horse.');
    return;
  }
  const horse = kcds.characters.Horse.spawn(player.position, undefined, undefined, player.nickname + "'s horse");
  console.log('bought ' + horse.name + ' balance ' + kcds.core.Exports.get('ledger', 'balance')(player.id));
});
kcds.core.Events.on('stables:held', () => {
  console.log('held is current ' + (heldPay === kcds.core.Exports.get('ledger', 'pay')));
});
`,
  lua: `
local heldPay = nil
kcds.core.Events.on("playerCommand", function(player, command)
  if command == "grant" then
    kcds.core.Exports.get("ledger", "pay")(player.id, 600)
    return
  end
  if command ~= "buyhorse" then
    return
  end
  local payFrom = kcds.core.Exports.get("ledger", "pay")
  heldPay = heldPay or payFrom
  if not payFrom(player.id, -500) then
    kcds.core.Chat.sendToPlayer(player, "You cannot afford a horse.")
    return
  end
  local horse = kcds.characters.Horse.spawn(player.position, nil, nil, player.nickname .. "'s horse")
  print("bought " .. horse.name .. " balance " .. kcds.core.Exports.get("ledger", "balance")(player.id))
end)
kcds.core.Events.on("stables:held", function()
  print("held is current " .. tostring(heldPay == kcds.core.Exports.get("ledger", "pay")))
end)
`,
  csharp: `
public class Stables : Script
{
    private JsValue? heldPay;

    public override void OnStart()
    {
        Events.On("playerCommand", (Player player, string command) =>
        {
            if (command == "grant")
            {
                Exports.Get("ledger", "pay").Invoke(player.Id, 600);
                return;
            }
            if (command != "buyhorse")
            {
                return;
            }
            var payFrom = Exports.Get("ledger", "pay");
            heldPay ??= payFrom;
            if (!payFrom.Invoke(player.Id, -500).AsBool())
            {
                Chat.SendToPlayer(player, "You cannot afford a horse.");
                return;
            }
            var horse = Horse.Spawn(player.Position, null, null, player.Nickname + "'s horse");
            var balance = Exports.Get("ledger", "balance").Invoke(player.Id);
            Log.Info("bought " + horse.Name + " balance " + balance);
        });
        Events.On("stables:held", () =>
        {
            var current = heldPay.HasValue && heldPay.Value.Is(Exports.Get("ledger", "pay"));
            Log.Info("held is current " + (current ? "true" : "false"));
        });
    }
}
`,
};

const DISCORD_LOG = {
  javascript: `
kcds.core.Messages.handle('post', (payload, reply) => {
  const text = typeof payload === 'object' && payload !== null ? payload.text : undefined;
  if (typeof text !== 'string') {
    reply({ ok: false });
    return;
  }
  console.log('[log] ' + text);
  reply({ ok: true });
});
kcds.core.Messages.handle('hang', () => {});
`,
  lua: `
kcds.core.Messages.handle("post", function(payload, reply)
  local text = type(payload) == "userdata" and payload.text or nil
  if type(text) ~= "string" then
    reply({ ok = false })
    return
  end
  print("[log] " .. text)
  reply({ ok = true })
end)
kcds.core.Messages.handle("hang", function() end)
`,
  csharp: `
public class DiscordLog : Script
{
    public override void OnStart()
    {
        Messages.Handle("post", (JsValue payload, JsValue reply) =>
        {
            var text = payload["text"];
            if (text.Type != "string")
            {
                reply.Invoke(new { ok = false });
                return;
            }
            Log.Info("[log] " + text);
            reply.Invoke(new { ok = true });
        });
        Messages.Handle("hang", () => { });
    }
}
`,
};

const SALES = {
  javascript: `
function withTimeout(promise, ms) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timed out')), ms);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      },
    );
  });
}
async function logSale(type, text) {
  try {
    const answer = await withTimeout(kcds.core.Messages.request('discord-log', type, { text }), 1000);
    console.log('sale logged ' + JSON.stringify(answer));
  } catch (error) {
    console.log('sale kept: ' + error.message);
  }
}
kcds.core.Events.on('sales:sell', (type, text) => logSale(type, text));
kcds.core.Messages.send('discord-log', 'post', { text: 'fire and forget' });
kcds.core.Messages.send('nobody', 'post', {});
`,
  // A handler that returns a promise is waited for, as an async JavaScript handler is.
  lua: `
local function withTimeout(promise, ms)
  return Promise.create(function(resolve, reject)
    local timer = setTimeout(function() reject("timed out") end, ms)
    promise
      :next(function(value)
        clearTimeout(timer)
        resolve(value)
      end)
      :catch(function(problem)
        clearTimeout(timer)
        reject(problem)
      end)
  end)
end
local function logSale(kind, text)
  local ok, pending = pcall(kcds.core.Messages.request, "discord-log", kind, { text = text })
  if not ok then
    print("sale kept: " .. pending.message)
    return
  end
  return withTimeout(pending, 1000)
    :next(function(answer)
      print("sale logged " .. json.encode(answer))
    end)
    :catch(function(problem)
      print("sale kept: " .. (type(problem) == "string" and problem or problem.message))
    end)
end
kcds.core.Events.on("sales:sell", function(kind, text) return logSale(kind, text) end)
kcds.core.Messages.send("discord-log", "post", { text = "fire and forget" })
kcds.core.Messages.send("nobody", "post", {})
`,
  // The handler answers a Task, waited for as a promise is.
  csharp: `
public class Sales : Script
{
    private static async Task<JsValue> WithTimeout(Task<JsValue> pending, double milliseconds)
    {
        var first = await Task.WhenAny(pending, Timers.Delay(milliseconds));
        if (first != pending)
        {
            throw new TimeoutException("timed out");
        }
        return await pending;
    }

    private static async Task LogSale(string type, JsValue text)
    {
        try
        {
            var answer = await WithTimeout(Messages.Request("discord-log", type, new { text }), 1000);
            Log.Info("sale logged " + answer.ToJson());
        }
        catch (Exception problem)
        {
            Log.Info("sale kept: " + problem.Message);
        }
    }

    public override void OnStart()
    {
        Events.On("sales:sell", (string type, JsValue text) => LogSale(type, text));
        Messages.Send("discord-log", "post", new { text = "fire and forget" });
        Messages.Send("nobody", "post", new { });
    }
}
`,
};

const PANEL = {
  javascript: `
    const pay = kcds.core.Exports.get('ledger', 'pay');
    console.log('paid ' + pay(1, 30) + ' balance ' + kcds.core.Imports.get('ledger').balance(1));
    try {
      kcds.core.Exports.get('ledger', 'secret');
    } catch (error) {
      console.log('secret ' + error.message);
    }
    `,
  lua: `
    local pay = kcds.core.Exports.get("ledger", "pay")
    print("paid " .. tostring(pay(1, 30)) .. " balance " .. kcds.core.Imports.get("ledger").balance(1))
    local ok, problem = pcall(kcds.core.Exports.get, "ledger", "secret")
    if not ok then
      print("secret " .. problem.message)
    end
    `,
  csharp: `
public class Panel : Script
{
    public override void OnStart()
    {
        var pay = Exports.Get("ledger", "pay");
        var paid = pay.Invoke(1, 30);
        Log.Info("paid " + paid + " balance " + Imports.Get("ledger").Call("balance", 1));
        try
        {
            Exports.Get("ledger", "secret");
        }
        catch (JsError problem)
        {
            Log.Info("secret " + problem.Message);
        }
    }
}
`,
};

// A consumer that pays a tip from the ledger: its code, and for C# the code built.
function tips(language, amount) {
  if (language === 'lua') {
    return `kcds.core.Events.on("tips:give", function(id) kcds.core.Exports.get("ledger", "pay")(id, ${amount}) end)`;
  }
  if (language === 'csharp') {
    return `
public class Tips : Script
{
    public override void OnStart()
    {
        Events.On("tips:give", (double id) =>
        {
            Exports.Get("ledger", "pay").Invoke(id, ${amount});
        });
    }
}
`;
  }
  return `kcds.core.Events.on('tips:give', (id) => kcds.core.Exports.get('ledger', 'pay')(id, ${amount}));`;
}

for (const language of LANGUAGES) {
  test(`sharing (${language}): a shop calls the ledger exports and restarts with it`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'ledger', LEDGER, { exports: ['balance', 'pay'] }, language);
    serverResource(f.root, 'stables', STABLES, { requires: ['ledger'] }, language);
    await f.host.startAll();
    assert.ok(f.linesOf('ledger').includes('register undeclared true'));
    const player = f.report(61, { nickname: 'Henry', position: { x: 5, y: 6, z: 7 } });
    f.host.receiveChat(player.id, '/buyhorse');
    const refusals = f.sent.filter((message) => message.op === 'chat');
    assert.deepEqual(
      refusals.map((message) => message.text),
      ['You cannot afford a horse.'],
    );
    f.host.receiveChat(player.id, '/grant');
    f.host.receiveChat(player.id, '/buyhorse');
    assert.ok(
      f.linesOf('stables').includes("bought Henry's horse balance 100"),
      f.linesOf('stables').join('\n'),
    );
    const [horse] = f.host.horses.values();
    assert.deepEqual(horse.position.toJSON(), { x: 5, y: 6, z: 7 });
    // An undeclared export was not stored.
    assert.equal(f.host.services.owners.get('ledger').exports.has('secret'), false);
    // The library restarts: the dependent restarts with it and must read exports again.
    const generation = f.host.resources.get('stables').generation;
    await f.host.ensure('ledger');
    assert.notEqual(f.host.resources.get('stables').generation, generation);
    assert.equal(f.host.horses.size, 0);
  });

  test(`sharing (${language}): an export held across a library restart keeps the old code`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'ledger', LEDGER, { exports: ['balance', 'pay'] }, language);
    // This consumer does not declare the dependency, so it is not restarted with the library.
    serverResource(f.root, 'stables', STABLES, {}, language);
    await f.host.startAll();
    const player = f.report(62);
    f.host.receiveChat(player.id, '/buyhorse');
    await f.host.services.emit('shared', 'stables:held', []);
    await f.host.ensure('ledger');
    await f.host.services.emit('shared', 'stables:held', []);
    const held = f.linesOf('stables').filter((line) => line.startsWith('held is current'));
    assert.deepEqual(held, ['held is current true', 'held is current false']);
    assert.ok(f.linesOf('stables').includes('Undeclared resource dependency: ledger'));
  });

  test(`sharing (${language}): a sale is logged through Messages that answer, refuse or stay silent`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'discord-log', DISCORD_LOG, {}, language);
    serverResource(
      f.root,
      'sales',
      SALES,
      { requires: [{ name: 'discord-log', optional: true }] },
      language,
    );
    await f.host.startAll();
    const sell = (type, text) => f.host.services.emit('shared', 'sales:sell', [type, text]);
    await sell('post', 'a horse');
    await sell('post', 42);
    await sell('unknown', 'x');
    await sell('hang', 'slow');
    // The hanging request times out after its 1000 ms (long enough for a loaded test run's replies).
    await new Promise((resolve) => setTimeout(resolve, 1200));
    // Stopping the logger stops its dependents first, including an optional dependent.
    await f.host.stop('discord-log');
    assert.equal(f.host.resources.has('sales'), false);
    assert.deepEqual(
      f.linesOf('discord-log').filter((line) => line.startsWith('[log]')),
      ['[log] fire and forget', '[log] a horse'],
    );
    assert.deepEqual(
      f.linesOf('sales').filter((line) => line.startsWith('sale')),
      [
        'sale logged {"ok":true}',
        'sale logged {"ok":false}',
        'sale kept: Resource does not handle this message type',
        'sale kept: timed out',
      ],
    );
  });

  test(`sharing (${language}): the shop carries on when the logger is not installed at all`, async (t) => {
    const f = serverFixture(t);
    serverResource(
      f.root,
      'sales',
      SALES,
      { requires: [{ name: 'discord-log', optional: true }] },
      language,
    );
    await f.host.startAll();
    await f.host.services.emit('shared', 'sales:sell', ['post', 'a cart']);
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.deepEqual(
      f.linesOf('sales').filter((line) => line.startsWith('sale')),
      ['sale kept: Resource is not available'],
    );
    assert.equal(f.host.resources.has('sales'), true);
  });

  test(`sharing (${language}): client resources share exports on their own side`, async (t) => {
    const f = clientFixture(t);
    const ledger = f.resource(
      'ledger',
      LEDGER,
      { exports: ['balance', 'pay'] },
      'ledger',
      language,
    );
    const panel = f.resource('panel', PANEL, { requires: ['ledger'] }, 'panel', language);
    await f.runtime.apply({ resources: [ledger, panel] }, f.read);
    assert.deepEqual(
      f.linesOf('panel').filter((line) => line !== 'client resource started'),
      ['paid true balance 30', 'secret Export not found'],
    );
    assert.equal(
      f.sent.some((message) => message.op === 'event'),
      false,
    );
  });

  test(`sharing (${language}): library state behind functions survives a consumer reload`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'ledger', LEDGER, { exports: ['balance', 'pay'] }, language);
    const first = { [language]: tips(language, 7) };
    serverResource(f.root, 'tips', first, { requires: ['ledger'] }, language);
    await f.host.startAll();
    await f.host.services.emit('shared', 'tips:give', [9]);
    fs.writeFileSync(
      path.join(f.root, 'tips', ENTRY[language]),
      entryContent('tips', { [language]: tips(language, 3) }, language),
    );
    await f.host.ensure('tips');
    await f.host.services.emit('shared', 'tips:give', [9]);
    const ledger = f.host.services.owners.get('ledger');
    assert.equal(ledger.exports.get('balance')(9), 10);
  });
}

// A library in one language and its consumer in another: the same exports, the
// same restart rules, for every pair of languages.
for (const [library, consumer] of [
  ['javascript', 'lua'],
  ['lua', 'javascript'],
  ['javascript', 'csharp'],
  ['csharp', 'javascript'],
  ['lua', 'csharp'],
  ['csharp', 'lua'],
]) {
  test(`sharing: a ${library} ledger serves a ${consumer} shop`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'ledger', LEDGER, { exports: ['balance', 'pay'] }, library);
    serverResource(f.root, 'stables', STABLES, { requires: ['ledger'] }, consumer);
    await f.host.startAll();
    const player = f.report(63, { nickname: 'Henry' });
    f.host.receiveChat(player.id, '/grant');
    f.host.receiveChat(player.id, '/buyhorse');
    assert.ok(
      f.linesOf('stables').includes("bought Henry's horse balance 100"),
      f.linesOf('stables').join('\n'),
    );
    await f.host.services.emit('shared', 'stables:held', []);
    assert.ok(f.linesOf('stables').includes('held is current true'));
  });
}
