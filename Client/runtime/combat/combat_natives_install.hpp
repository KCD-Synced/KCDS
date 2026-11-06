// Combat natives found and hooks installed at startup: the perfect block's start
// and window, the dodges, the physical contact's natives and the effects of hits on
// players and items.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter(), in this order.
void install_perfect_block_hooks();
void install_dodge_hooks();
void resolve_contact_natives();
void resolve_item_effect_natives();
// Called by initialize_adapter() later, after MinHook is initialized: the price of another
// player's blow (PricedBlow), the trample diagnostics, and the optional guards (the
// UI-blocked near miss, the replica pair mark check, the replica push key filter).
void install_hit_pricing();
void install_trample_diagnostics();
void install_optional_combat_guards();

}  // namespace kcds::game_client
