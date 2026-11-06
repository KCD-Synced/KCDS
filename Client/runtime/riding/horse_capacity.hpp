// The most a server horse's saddlebags carry (Horse.maxCarryWeight),
// WHGame 1.5.5.
//
// A soul's inventory capacity is its derived stat 23 (stats/player_stats.cpp), which the game asks
// of the soul's derived-stat getter (SoulDerivedStat, riding/trample.hpp native_soul_stat). The
// getter is hooked: a server horse's soul with a limit answers 23 with it, so the game's own
// saddlebags (their weight bar and what they take) keep to it on every client. A breed's stamina
// and speed are answered the same way (set_stat); every other soul and stat is the
// game's own. Engine thread only.
#pragma once

namespace kcds::runtime::riding::horse_capacity {

using Logger = void (*)(const char* format, ...);

// Hooks the derived-stat getter (its address).
void install(void* derived_stat, Logger log);

// A server horse's soul and its limit, kg; below 0 the game's own.
void set(void* soul, float weight);

// A server horse's soul and one of its derived stats (MaxStamina 6, RunSpeedBase 32,
// NormalizedRunSpeedBase 33); below 0 the game's own.
void set_stat(void* soul, int id, float value);
inline constexpr int kMaxStamina = 6;
inline constexpr int kRunSpeedBase = 32;
inline constexpr int kNormalizedRunSpeedBase = 33;

// A horse's speed as the inspect screen shows it (30 to 60; the game's horses about 35) on the
// soul: NormalizedRunSpeedBase and RunSpeedBase at the value the screen shows that as, between the
// game's RunSpeedBaseGuiMinHorse (0.847, 30) and RunSpeedBaseGuiMaxHorse (0.951, 60); below 0 the
// game's own.
void set_speed(void* soul, float speed);

// The soul is gone (its horse's body was removed).
void forget(void* soul);

}  // namespace kcds::runtime::riding::horse_capacity
