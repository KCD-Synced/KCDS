// The game's own exhaustion faint ("falling asleep"), told apart from a knock-out.
//
// A player whose exhaust stays at 0 faints (WHGame 1.5.5): its "exhausted" buff
// (C_ExhaustedBuffInstance, faint check, only for a soul with the player flag
// soul + 0xCF0 bit 0x10) gives the soul the buff "unconscious" with C_UnconsciousBuffInitParams
// (duration ExtremeExhaustionFaintDuration, reason 3). A lethal hit's knock-out
// gives the same buff with reason 1, a plain AddBuff of one reason 0 (no end of its
// own). The instance, C_UnconsciousSoulBuffInstance (factory), keeps:
//
//   + 0     its vtable
//   + 216   the time left (float)
//   + 220   the reason (its update, counts a reason-0 instance down never)
//   + 232   whether it is active
//
// The class "Unconsciousness" is exclusive, so a soul holds one such instance at a time: a lethal
// blow on a fainted player replaces the faint with a knock-out. The game ends a faint by itself;
// in a server world its skip-time screen does not start (hooked_start_skip_time), so world time
// does not jump for anyone.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::knockout::faint {

// The soul's buff instances: a vector of pointers (soul + 1352 .. + 1360).
inline constexpr std::size_t kSoulBuffsBegin = 1352;
inline constexpr std::size_t kSoulBuffsEnd = 1360;
// C_UnconsciousSoulBuffInstance's reason, and the exhaustion faint's.
inline constexpr std::size_t kUnconsciousReason = 220;
inline constexpr std::uint32_t kFaintReason = 3;

// Whether the soul's unconsciousness is the exhaustion faint: it holds an instance of the
// unconscious buff class (its vtable `unconscious_vtable`, resolved and validated by the caller)
// whose reason is the faint's. False without a soul or a vtable. Engine thread; reads only.
bool holds_faint(const void* soul, const void* unconscious_vtable);

}  // namespace kcds::runtime::knockout::faint
