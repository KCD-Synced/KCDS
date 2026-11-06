// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "reaction_window.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"
#include "../replicas/remote.hpp"

namespace kcds::game_client {

// Clips the game itself starts on a replica while a combat reaction (a block or parry) is replayed
// skeleton -> layer -> the clip and when (ReactionClip, reaction_window.hpp).
unsigned native_action_type(void* movement);
// The native actions in which the game's own clip may be a variant of its owner's: guard (6),
// block or parry (7), and the pair SYNC_PERFECT_BLOCK (9) and its hit (10).
bool variant_reaction_action(unsigned action) {
  return action == 6 || action == 7 || action == 9 || action == 10;
}
std::mutex reaction_clips_mutex;
std::unordered_map<void*, std::array<ReactionClip, 16>> reaction_clips;
// Replicas the game may animate itself for a moment after a replayed reaction: skeleton -> until
// (GetTickCount64). The game starts the reaction's clips on its next updates, not within the
// replay call (the parry clip came one frame later and was refused).
std::unordered_map<void*, ULONGLONG> reaction_windows;
// How long the game animates the defender's replica itself after a reaction, in ms.
constexpr ULONGLONG kReactionWindowMs = 700;

void open_reaction_window(core::EntityId player) {
  const auto replica = remotes.find(player);
  if (replica == remotes.end() || !replica->second.skeleton) {
    return;
  }
  std::lock_guard<std::mutex> lock(reaction_clips_mutex);
  reaction_windows[replica->second.skeleton] = GetTickCount64() + kReactionWindowMs;
}

// When a replica's reaction window has closed: its layers on which the game started a clip that
// the owner's own clips never matched go back to the owner's current clip (the apply loop plays
// it again). The game's reaction stayed there before, because nothing else reaches a layer the
// owner's clip did not change on (a master-struck replica kept its hit reaction, 303479
// and 303482 on layer 11, frozen in that pose, while its owner only had the swing deflected).
// Layers the owner does not drive stay with the game, and so does the base layer: its owner's
// clip is a whole-body move (a swing, a hit), and playing it again replayed the swing after the
// exchange; the owner's next base clip follows soon anyway. The overlay layers' owner clips
// are held poses (the guard's arms), safe to play again.
constexpr unsigned kFirstReturnedLayer = 1;
void return_reaction_layers(void* skeleton, const local::AnimationState& clips,
                            std::uint64_t (&applied)[16]) {
  std::lock_guard<std::mutex> lock(reaction_clips_mutex);
  const auto window = reaction_windows.find(skeleton);
  if (window == reaction_windows.end() || GetTickCount64() < window->second) {
    return;
  }
  reaction_windows.erase(window);
  const auto played = reaction_clips.find(skeleton);
  if (played == reaction_clips.end()) {
    return;
  }
  for (unsigned layer = kFirstReturnedLayer; layer < 16; ++layer) {
    const ReactionClip reaction = played->second[layer];
    if (reaction.clip < 0 || !clips[layer].sequence || !applied[layer]) {
      continue;
    }
    // The game already plays the owner's own clip there (its attack's strike after a replayed
    // swing): handing the layer back restarted that clip, and the replica struck twice for one
    // attack of its owner.
    if (reaction.clip == clips[layer].clip) {
      continue;
    }
    // One below the owner's sequence: the clip plays again, blending from the reaction's.
    applied[layer] = clips[layer].sequence - 1;
    log("COMBAT REACTION replica clip=%d layer=%u returned to the owner's clip=%d", reaction.clip,
        layer, clips[layer].clip);
  }
  reaction_clips.erase(played);
}

}  // namespace kcds::game_client
