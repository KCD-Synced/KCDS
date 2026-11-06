// A rider's horse reservation never stops the engine adapter when the game has lost the horse.
//
// The game's AI keeps, per horse, which character it belongs to (the selected-horse manager,
// services + 224 -> + 440, wh::xgenaimodule::C_SelectedHorseDecision). A rider's NPC state
// reserves its horse while it rides (wh::xgenaimodule::NPCState::C_StanceElementRequired, stance
// 5) and releases it when that state ends. Reserving and releasing both find the horse by its id
// in the game's actor registry (qword_185494490, vtable slot 3, a plain search) and then set
// or clear a flag on it without checking that it was found.
//
// Live: a player flew away with NoClip from a player riding a synced horse. The
// replica's riding state ended (a posture change through the posture state's SetBodyPosition)
// and its release no longer found the horse: the game read address 0,
// the engine adapter stopped and the NoClip flight stayed frozen in the air. It did not happen
// again on demand (kit: the rider seated, the other client teleported and flown 130-280 m away),
// so the moment the registry loses the horse is not known.
//
// Both are hooked: a horse the game does not find is neither reserved nor released (there is
// nothing of it left to flag; the assignment stays until the horse is reserved again); a horse it
// finds goes through the game's own code.
//
// Engine thread.
#ifndef KCDS_RUNTIME_RIDING_HORSE_RESERVATION_HPP
#define KCDS_RUNTIME_RIDING_HORSE_RESERVATION_HPP

namespace kcds::runtime::riding {

using Logger = void (*)(const char* format, ...);

struct HorseReservationNatives {
  void* reserve = nullptr;    // (assignments, horse id*)
  void* release = nullptr;    // (assignments, horse id*)
  void** registry = nullptr;  // qword_185494490, the actor registry
};

// False when one of them is missing or cannot be hooked.
bool guard_horse_reservations(const HorseReservationNatives& natives, Logger logger);

}  // namespace kcds::runtime::riding

#endif  // KCDS_RUNTIME_RIDING_HORSE_RESERVATION_HPP
