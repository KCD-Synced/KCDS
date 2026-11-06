// The level's animated gates as the server has them (GateState):
// the Malesov drawbridge and the Suchdol portcullis, WHGame 1.5.5.
//
// The level shows such a gate as one of its layers; in its open state a static mesh (a brush). A
// gate the server sent is shown by its own animated model (the .cdf of the level's animated
// layer, kGates), spawned at the layer's place as props/prop_entity.hpp spawns a prop. Its clip
// is started held (speed 0) and the clip's point is set every frame from the server's state, as
// the entity binding's SetAnimationTime does (ISkeletonAnim slot 29): the closing clip forwards
// while the gate closes; while it opens the gate's own opening clip forwards (the portcullis's
// winch), or else the closing clip backwards; over the clip's length from when the move began,
// so every player sees the same smooth move and a late joiner joins it part way.
//
// The animated model has no collision; the gate's own static models give it, spawned unseen
// (opacity 0) and physicalized static where the level has them: the open model while the way is
// free, the closed model (or a primitive box scaled to the closed pose) while the gate blocks it.
// The one not in use is hidden, which takes its collision away.
//
// The gates of the world's level are taken over as soon as the world is ready, before the server
// says anything: the level's static open model is taken out for good through a level edit of this
// game's own (level/level_edits.hpp) and the animated model stands in its place, open as the level
// has it, so no player ever sees the old model beside the moving one. Engine thread.
#pragma once

#include <string_view>
#include <vector>

#include "kcd2s/integration/gates.hpp"
#include "kcd2s/integration/level_edits.hpp"

namespace kcds::runtime::gates {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Every frame in a ready world of that map (the world config's) with the states the server sent
// since the last frame, oldest first; adds the level edits of the gates' static models to
// `level_edits` (given to level::frame after).
void frame(std::string_view map, const std::vector<kcd2s::integration::GateState>& states,
           std::vector<kcd2s::integration::LevelEditChange>& level_edits);

// The world is left: its gate models went with it; the server sends the next world's states.
void reset();

}  // namespace kcds::runtime::gates
