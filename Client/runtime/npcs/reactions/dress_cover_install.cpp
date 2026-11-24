// Winsock before Windows.h (see client_internal.hpp).
#include "../../core/client_internal.hpp"

#include "dress_cover_install.hpp"

#include "../../core/log.hpp"
#include "../../core/natives.hpp"
#include "dress_cover.hpp"

namespace kcds::game_client {

void install_dress_cover() {
  namespace cover = kcds::runtime::npcs::reactions::dress_cover;
  cover::Natives natives;
  // The context manager's singleton (made on first use).
  natives.manager = resolve_unique("ContextManager",
                                   "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 90 48 85 DB 75 7F 48 8D "
                                   "0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                                   "48 8B 1D ?? ?? ?? ?? 48 85");
  // A context's id by its name (the registry the game's rules and checks share).
  natives.context_id = resolve_unique(
      "ContextIdByName",
      "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 48 83 EC 30 49 8B F0 48 8B FA "
      "FF 15 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 48 8D 2D ?? ?? ?? ?? 8B D8 75 08");
  // An entity's count of a context raised and lowered (the manager's map of them at + 0x180).
  natives.add = resolve_unique("EntityContextRaise",
                               "48 8B C4 48 89 58 08 44 89 40 18 48 89 50 10 57 48 83 EC 40 48 8B "
                               "D9 4C 8D 40 10 48 81 C1 80 01 00 00 48 8D 50 E8 41 8A F9 E8");
  natives.remove = resolve_unique(
      "EntityContextLower",
      "48 8B C4 48 89 58 18 48 89 68 20 48 89 50 10 56 57 41 56 48 83 EC 30 41 8B D8 48 8B F2 48 "
      "8B E9 4C 8D 40 10 48 81 C1 80 01 00 00 48 8D 50 08 45");
  natives.flush = resolve_unique("ContextManagerFlush",
                                 "40 53 48 83 EC 20 48 8B D9 48 8B 83 68 01 00 00 48 3B 83 70 01 "
                                 "00 00 74 1A 48 8B 00 48 8B CB F6 40 14 02 74 07 E8");
  natives.has = resolve_unique(
      "EntityHasContext",
      "48 89 5C 24 18 48 89 54 24 10 57 48 83 EC 30 48 8D 54 24 40 48 8B D9 E8 ?? ?? ?? ?? 48 8D "
      "8B 80 01 00 00 48 8B F8 4C 8D 44 24 48 48 8D 54 24 20");
  natives.string_from_text =
      resolve_unique("CryStringFromText",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                     "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00");
  natives.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  if (!cover::install(natives, log)) {
    log("DRESS COVER natives missing; NPCs remark on a woman in a dress as the game does");
  }
}

}  // namespace kcds::game_client
