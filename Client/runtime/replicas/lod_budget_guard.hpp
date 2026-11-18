// The game's actor LOD budget update with its queue kept to actors it can take
// (lod_budget_queue.hpp): an actor queued twice or in neither of its lists stopped the game.
//
// Engine thread.
#pragma once

namespace kcds::runtime::replicas {

using BudgetLogger = void (*)(const char* format, ...);

// budget_update: C_ActorMonsterLODBudgetting's update (the budget). False when it is missing or
// the hook failed.
bool install_lod_budget_guard(void* budget_update, BudgetLogger logger);

}  // namespace kcds::runtime::replicas
