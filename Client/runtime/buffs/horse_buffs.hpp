// The status effects a server put on a horse (Horse.addBuff): every client gives its
// body of the horse the buffs of the HorseBuffs field (kcd2s/integration/horse_buffs.hpp) and keeps
// them on it. A buff that wears off or a body made anew gets them again; one that leaves the list
// is taken off. Only the instances added here are touched, never the game's own.
//
// A buff that is gone again within kHoldSeconds of being added (one that acts once and ends) is
// not added again until the field changes, so it acts once per change and not every second.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::buffs::horses {

using Logger = void (*)(const char* format, ...);

inline constexpr double kHoldSeconds = 2;

void set_logger(Logger logger);

// The horse's soul and its field as the server wrote it; checked at most once a second unless the
// field changed.
void apply(std::uint32_t horse, void* soul, const std::string& field, double now);

// The horse's body is gone: what was added to it went with it.
void forget(std::uint32_t horse);

}  // namespace kcds::runtime::buffs::horses
