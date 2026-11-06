// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "follow_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "follow.hpp"

namespace kcds::game_client {

void install_follow() {
  // The game's follow of a conversation: the action actor's follow start
  // and C_ActorAction's finish.
  kcds::runtime::follow::Natives natives;
  natives.start = resolve_unique(
      "FollowStart",
      "48 89 5C 24 10 57 48 83 EC 20 48 8B 01 48 8B DA 48 8B F9 FF 50 10 B2 1A 48 8B 88 90 09 00 "
      "00 48 81 C1 70 03 00 00 48 8B 01 FF 50 08 84 C0 75");
  natives.finish = resolve_unique(
      "ActorActionFinish",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 8B FA 48 8B 59 40 FF 90 80 00 00 00 44 8B C7 48 8B "
      "CB 8B D0 48 8B 5C 24 30 48 83");
  natives.may_follow = resolve_unique(
      "FollowAllowed",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 "
      "8B 43 18 48 8B 80 68 0D 00 00 80 78 31 00 75");
  natives.may_keep_following =
      resolve_unique("FollowGoesOn",
                     "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 88 50 01 00 00 48 8B 49 10 "
                     "E8 ?? ?? ?? ?? 48 "
                     "85 C0 74 18 48 8B 10 48 8B C8 FF 52 58 8B D0 48 8B CB 48 83 C4 20 5B E9");
  // The follow's movement request (C_ActorActionFollow): its walking pace.
  natives.move = resolve_unique(
      "FollowMove",
      "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00 4C "
      "8B EA 0F 29 70 B8 4C 8B F1 0F 29 78 A8 49 8B CD 49 8B D1 4D 8B F8 E8 ?? ?? ?? ?? 4D 8B 4D "
      "00 49 8D 76 58 48 B8 FF FF FF FF FF FF FF FD");
  // The follow's tuning for its target: its distances (Follow.start distance).
  natives.tuning = resolve_unique("FollowTuning",
                                  "40 53 48 83 EC 20 48 8B 49 18 8B DA E8 ?? ?? ?? ?? 84 C0 74 0D "
                                  "E8 ?? ?? ?? ?? 48 05 68 04 00 00 EB 63 E8");
  natives.get_entity = get_entity;
  natives.log = log;
  kcds::runtime::follow::install(natives);
}

}  // namespace kcds::game_client
