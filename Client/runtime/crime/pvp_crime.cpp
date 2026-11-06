#include "pvp_crime.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::crime::pvp_crime {
namespace {

// (called from the script node's update): rcx unused, rdx the result,
// r8 the Npc, r9 the OptionalNpc (or null); returns the result.
using Participants = void*(__fastcall*)(void* self, void* result, void* npc, void* other);

// The result: a vector of 8-byte participant ids (begin + 0, end + 8, capacity + 16), then
// ContainsPlayer (+ 24) and ContainsPlayersDog (+ 25).
constexpr std::size_t kBegin = 0;
constexpr std::size_t kEnd = 8;
constexpr std::size_t kContainsPlayer = 24;
// A skirmish never has this many fighters; a larger count is not a vector this hook knows.
constexpr std::size_t kMostParticipants = 256;

Participants original_participants = nullptr;
IsPlayerId player_id = nullptr;
Logger log_line = nullptr;
std::atomic<unsigned> cleared_logged{0};
std::atomic<unsigned> kept_logged{0};
constexpr unsigned kMostLogged = 8;

template <typename T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

// Whether every participant is a player. Fills `first_other` with the first id that is not.
bool only_players(const std::uint64_t* begin, std::size_t count, std::uint64_t& first_other) {
  for (std::size_t index = 0; index < count; ++index) {
    if (!player_id(begin[index])) {
      first_other = begin[index];
      return false;
    }
  }
  return true;
}

void* __fastcall hooked_participants(void* self, void* result, void* npc, void* other) {
  void* answer = original_participants(self, result, npc, other);
  if (!result || !player_id || field<std::uint8_t>(result, kContainsPlayer) == 0) {
    return answer;
  }
  const auto* begin = field<const std::uint64_t*>(result, kBegin);
  const auto* end = field<const std::uint64_t*>(result, kEnd);
  if (!begin || end < begin) {
    return answer;
  }
  const auto count = static_cast<std::size_t>(end - begin);
  if (count < 2 || count > kMostParticipants) {
    return answer;
  }
  std::uint64_t first_other = 0;
  if (only_players(begin, count, first_other)) {
    static_cast<std::uint8_t*>(result)[kContainsPlayer] = 0;
    if (log_line && cleared_logged.fetch_add(1) < kMostLogged) {
      log_line("CRIME fight of %zu players only: no crime report (first=%llx second=%llx)", count,
               static_cast<unsigned long long>(begin[0]),
               static_cast<unsigned long long>(begin[1]));
    }
    return answer;
  }
  if (log_line && kept_logged.fetch_add(1) < kMostLogged) {
    log_line("CRIME fight of %zu with a player and an NPC (%llx): reported as the game does", count,
             static_cast<unsigned long long>(first_other));
  }
  return answer;
}

}  // namespace

bool install(void* participants, IsPlayerId is_player_id, Logger logger) {
  log_line = logger;
  player_id = is_player_id;
  if (!participants || !is_player_id) {
    return false;
  }
  return MH_CreateHook(participants, reinterpret_cast<void*>(hooked_participants),
                       reinterpret_cast<void**>(&original_participants)) == MH_OK &&
         MH_EnableHook(participants) == MH_OK;
}

}  // namespace kcds::runtime::crime::pvp_crime
