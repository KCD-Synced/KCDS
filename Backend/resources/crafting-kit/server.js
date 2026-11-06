// crafting-kit: on a test server every player can brew and smith at once. Each player who joins
// learns every alchemy and smithing recipe and gets the materials for them (a server without a
// persistence resource starts every join with an empty inventory); /kit [alchemy|smithing] gives
// the materials again.

// The general materials, bought or gathered: a recipe that needs anything else (a quest piece,
// an artefact, a unique sword to reforge) is no kit recipe.
const MATERIAL = /^(bsmt_|special_|skin_|herb_)/;
// Each material comes in this many times the most any recipe of the kit takes of it.
const BATCHES = 3;
// No base liquids: the alchemy table has its own (water, wine, spirits, oil); the game's base
// items have no name to show.
function nameOf(item) {
  const found = kcds.players.Items.effective(item);
  return found ? found.name : null;
}

function kitRecipes(kind) {
  return kcds.world.Recipe.all(kind).filter((recipe) =>
    recipe.ingredients.every((ingredient) => {
      const name = nameOf(ingredient.item);
      return name !== null && MATERIAL.test(name);
    }),
  );
}

// The materials of a kind: item name -> amount.
function materialsOf(kind) {
  const amounts = new Map();
  for (const recipe of kitRecipes(kind)) {
    for (const ingredient of recipe.ingredients) {
      const name = nameOf(ingredient.item);
      const most = Math.max(amounts.get(name) ?? 0, ingredient.amount * BATCHES);
      amounts.set(name, most);
    }
  }
  return amounts;
}

const KINDS = ['alchemy', 'smithing'];
const MATERIALS = new Map(KINDS.map((kind) => [kind, materialsOf(kind)]));

function teachRecipes(player) {
  let taught = 0;
  for (const kind of KINDS) {
    for (const recipe of kcds.world.Recipe.all(kind)) {
      if (player.learnRecipe(recipe.id)) {
        taught += 1;
      }
    }
  }
  return taught;
}

function giveMaterials(player, kinds) {
  let given = 0;
  for (const kind of kinds) {
    for (const [name, amount] of MATERIALS.get(kind)) {
      if (player.giveItem(name, amount)) {
        given += 1;
      }
    }
  }
  return given;
}

kcds.core.Events.on('playerReady', (player) => {
  teachRecipes(player);
  const given = giveMaterials(player, KINDS);
  kcds.core.Chat.sendToPlayer(
    player,
    `You know every alchemy and smithing recipe and got ${given} materials. /kit [alchemy|smithing] gives more.`,
    { author: 'Kit' },
  );
});

kcds.core.Events.on('playerCommand', (player, command, args) => {
  if (command.toLowerCase() !== 'kit') {
    return;
  }
  const kinds = KINDS.includes(args[0]) ? [args[0]] : KINDS;
  if (!player.ready) {
    kcds.core.Chat.sendToPlayer(player, 'Wait until you are in the world.', { author: 'Kit' });
    return;
  }
  const given = giveMaterials(player, kinds);
  kcds.core.Chat.sendToPlayer(player, `Kit: ${given} materials (${kinds.join(' and ')}).`, {
    author: 'Kit',
  });
});
