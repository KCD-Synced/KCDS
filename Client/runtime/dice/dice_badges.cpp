#include "dice_badges.hpp"

#include <windows.h>

#include <cstring>
#include <optional>

#include "MinHook.h"

namespace kcds::runtime::dice::badges {
namespace {

namespace wire = kcd2s::integration;

// C_Dice: the players' badge tag points; a badge: its item and the other player's badge item.
constexpr std::size_t kPlayerTagPoint = 868;
constexpr std::size_t kOpponentTagPoint = 872;
constexpr std::size_t kBadgeItem = 24;
constexpr std::size_t kRivalItem = 32;

using Bind = void(__fastcall*)(void* minigame, void* own, void* theirs);
using Place = double(__fastcall*)(void* badge, const std::uint32_t* tag_point);

Natives natives;
Bind original_bind = nullptr;

// The badges of the match about to start or being played: the items made for them, null for a
// player without one.
struct Prepared {
  std::uint32_t match = 0;
  void* own = nullptr;
  void* theirs = nullptr;
};
std::optional<Prepared> prepared;

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class T>
void write(void* object, std::size_t offset, T value) {
  std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

void* make(std::uint32_t opponent, const wire::DiceBadge& badge) {
  if (!badge.present() || !natives.create_item) {
    return nullptr;
  }
  return natives.create_item(opponent, badge.item);
}

// The match's badges in place of the game's choice: each player's item (none without), the
// other's item for its Defence badge, and each badge on the table at its tag point.
void __fastcall hooked_bind(void* minigame, void* own, void* theirs) {
  void* ours = natives.match_minigame ? natives.match_minigame() : nullptr;
  if (!prepared || !own || !theirs || minigame != ours) {
    original_bind(minigame, own, theirs);
    return;
  }
  write<void*>(own, kBadgeItem, prepared->own);
  write<void*>(theirs, kBadgeItem, prepared->theirs);
  write<void*>(own, kRivalItem, prepared->theirs);
  write<void*>(theirs, kRivalItem, prepared->own);
  const auto place = reinterpret_cast<Place>(natives.place);
  if (prepared->own) {
    place(own, reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(minigame) +
                                                      kPlayerTagPoint));
  }
  if (prepared->theirs) {
    place(theirs, reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(minigame) +
                                                         kOpponentTagPoint));
  }
  log_line("DICE match %u: badges bound (own %p, the other player's %p)", prepared->match,
           prepared->own, prepared->theirs);
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  if (!natives.bind || !natives.place) {
    log_line("DICE badge natives are missing");
    return false;
  }
  if (MH_CreateHook(natives.bind, reinterpret_cast<void*>(&hooked_bind),
                    reinterpret_cast<void**>(&original_bind)) != MH_OK ||
      MH_EnableHook(natives.bind) != MH_OK) {
    original_bind = nullptr;
    log_line("DICE could not hook the badge binding %p", natives.bind);
    return false;
  }
  return true;
}

void prepare(std::uint32_t match, std::uint32_t opponent, const wire::DiceBadge& own,
             const wire::DiceBadge& theirs) {
  Prepared badges;
  badges.match = match;
  badges.own = make(opponent, own);
  badges.theirs = make(opponent, theirs);
  if ((own.present() && !badges.own) || (theirs.present() && !badges.theirs)) {
    log_line("DICE match %u: a badge item could not be made", match);
  }
  prepared = badges;
}

void forget(std::uint32_t match) {
  if (prepared && prepared->match == match) {
    prepared.reset();
  }
}

}  // namespace kcds::runtime::dice::badges
