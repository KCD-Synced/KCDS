// A clip of the game's own on the studio's character (Asset/Animation Studio): one of
// the game's animations by its whole path ("Animations/humans/male/cinematics/.../x.caf", a
// cutscene's motion capture such as the whipping executioner's) played straight on the body's
// skeleton on the script clip layer (player_animation::start_body_clip: no Mannequin request; a
// path no body's set lists is added to it from the game's files), looped or held at its end. A
// request through play_local stopped the engine adapter here (an access violation in ucrtbase
// before the clip was looked up, live): the studio's world has no network player for its
// Mannequin side. The studio's own keys still add their turns on top, and a prop in the hand
// follows the hand.
//
// Only a part of the clip may play (from..to of its length, 0 to 1: a cutscene's strikes without
// its walking in), and in place: the body is kept where it stood when the clip started, so the
// cutscene's walking and turning do not take it away (it goes back there when the clip stops).
//
//   { op: 'studioGameClip', path, loop, from?, to?, inPlace? }   plays it ('' path stops it);
//        the same path again changes only its part, loop and place
//   -> { op: 'studioGameClipState', path, time, seconds, from, to, loop, inPlace, topBody } five
//        times a second while it plays (time -1 once it stopped; seconds: its whole length)
//   { op: 'studioGameClipBake' }   records one round of the part playing, as the body's joints
//        turn (kBakeJoints, legs included, and the parents they need), up to 30 frames a second
//   -> { op: 'studioGameClipBaked', path, part, parts, seconds, frames, names?, parents?, idle?,
//        idleHips? } in parts of 12 frames: each frame its time in the part (s), the hips' place
//        in the body's space and each joint's turn in the body's space (x, y, z, w); the first
//        part names the joints, each one's parent's index (-1 the body) and their turns before
//        the clip. { failed: true } without them. The editor makes a studio animation of it
//        (Client/resources/studio/clip_bake.cjs).
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::game_clip {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world, with the character's entity (send: to the resource runtime).
void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send);

// Whether a clip plays now (the studio's pose then lets the body's playback run).
bool playing();

}  // namespace kcds::runtime::studio::game_clip
