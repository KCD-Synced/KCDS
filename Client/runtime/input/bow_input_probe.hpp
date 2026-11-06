#pragma once

namespace kcds::game_client {

// Engine thread only, from the existing KCDS_TEST_HOOKS InputScript special-action route.
// Returns true for recognized test actions, including a logged refusal. This does not mean
// a shot occurred. No installation or production hook; exact KCDS_TEST_HOOKS=1 is required.
// bow_primary / bow_secondary: mode 1 presses, mode 2 releases via the real UI callbacks.
// bow_primary_release / bow_secondary_release: mode 1 invokes that explicit release callback.
// bow_probe_state: mode 1 logs the native control mode and controller state, without input.
// bow_draw_fixture / bow_holster_fixture: mode 1 uses existing native weapon helpers;
// request logs do not prove the native animation completed. Releases must happen first.
// Presses require the local, currently held catalog bow_a row many487:weapon in slot 0.
// Releases also allow a holstered fixture bow or an expansion with no weapon/action;
// a changed weapon or inconsistent native action is refused and logged.
bool scripted_bow_probe(const char* action, unsigned mode);

}  // namespace kcds::game_client
