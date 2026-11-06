// Whether a fast travel could begin now (the client's Travel.canStart), WHGame 1.5.5.
//
// C_FastTravel's check answers with the code the map's dialogs read:
//   0 yes, 1 not now, 2 not from here, 3 combat, 4 hungry, 5 poisoned, 6 bleeding,
//   7 time paused, 8 trespassing, 9 tired, 10 overloaded, 11 tense circumstances.
// The game asks it only after planning a route to a map point, which fills the fields
// it reads: the route's points, the traveller at + 88, what carries them at + 96 and
// the interior bit at + 73. Here those fields are filled for the one call and put back, so no
// route is searched and the game's own plan stays as it was. Engine thread only.
#pragma once

namespace kcds::runtime::world_map::travel_check {

struct Natives {
  void* (*services)() = nullptr;  // the game services getter
  void* check = nullptr;          // (C_FastTravel)
  void* tag_manager = nullptr;    // (), the holder of the world's tags
  void* has_tag = nullptr;        // (manager, object, tag string)
  void* make_string = nullptr;    // (string out, text)
  void* free_string = nullptr;    // (string)
};

void install(const Natives& natives);

// True when every native above was found.
bool available();

// The game's code, or -1 when it cannot be asked: no player yet, or a native is missing.
int ask();

}  // namespace kcds::runtime::world_map::travel_check
