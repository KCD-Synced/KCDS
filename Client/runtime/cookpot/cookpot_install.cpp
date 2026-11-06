// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "cookpot_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../vendors/vendors_tick.hpp"
#include "kettle_state.hpp"
#include "pot_looks.hpp"

namespace kcds::game_client {

void install_cookpot_looks() {
  kcds::runtime::cookpot::Natives natives;
  // The script's integer-phase SetPrefabPhaseById; a float-phase twin
  // differs in its prologue.
  natives.binding = resolve_unique(
      "SetPrefabPhaseById",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 56 41 57 48 83 EC 40 48 8B 0D ?? ?? ?? "
      "?? 48 8B F2 41 8B D0 4D 8B F9 49 8B D8 48 8B 01 FF 50 70 48 8B 0D ?? ?? ?? ?? 4C 8B F0 48 "
      "8B A9 50 01 00 00 48 85 C0 74");
  natives.log = log;
  log("POTS %s", kcds::runtime::cookpot::install(natives) ? "installed" : "unavailable");
  // A pot's fill told to its fireplace's kettle behaviour (kettle_state.hpp): the message send
  // behind XGenAIModule.SendMessageToEntity and the game's string made and freed.
  kcds::runtime::cookpot::kettle::Natives kettle;
  kettle.send = resolve_unique("AiSendMessageToEntity",
                               "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 48 83 "
                               "EC 30 48 8B CA");
  kettle.make_string = resolve_unique("GameStringFromText",
                                      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC "
                                      "20 44 8B 05 ?? ?? ?? ?? 48 8B F1");
  kettle.free_string = resolve_unique("GameStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8");
  kettle.wuid_of = &entity_wuid;
  kettle.log = log;
  log("POTS kettle states %s",
      kcds::runtime::cookpot::kettle::install(kettle) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
