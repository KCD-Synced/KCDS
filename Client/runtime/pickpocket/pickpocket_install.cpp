// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "pickpocket_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "pickpocket.hpp"

namespace kcds::game_client {

void install_pickpocket() {
  kcds::runtime::pickpocket::Natives pickpocket;
  pickpocket.start =
      resolve_unique("RequestPickpocketing",
                     "48 89 5C 24 08 57 48 83 EC 30 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? "
                     "48 8B 88 28 01 00 00 48 8B 01 FF 50 38 8B 53 30 41 B1 01");
  pickpocket.result = resolve_unique(
      "PickpocketingResult",
      "40 53 48 83 EC 20 48 8B D9 0F B6 C2 48 81 C1 70 01 00 00 48 8D 14 C1 48 8D 4C 24 30 E8");
  pickpocket.notifications =
      resolve_unique("UiNotificationService",
                     "48 83 EC 28 8B 0D 3A FE AA 04 65 48 8B 04 25 58 00 00 00 BA 18 00 "
                     "00 00 48 8B 04 C8 8B 04 02 39 05 6B 6E FD 04");
  pickpocket.string_from_text =
      resolve_unique("CryStringFromText",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                     "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00");
  pickpocket.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  pickpocket.is_stolen = resolve_unique(
      "ItemIsStolen", "48 89 5C 24 18 56 57 41 56 48 83 EC 30 48 83 79 78 00 48 8B F9 0F 85");
  // human:CanBeRobbed and its two calls: the victim actor (+ 14) and the script return (+ 60).
  constexpr const char* kCanBeRobbed =
      "48 89 5C 24 08 57 48 83 EC 20 48 8B FA E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D "
      "98 08 0A 00 00 48 8B CB E8 ?? ?? ?? ?? 80 7B 20 02";
  pickpocket.can_be_robbed = resolve_unique("CanBeRobbed", kCanBeRobbed);
  pickpocket.robbed_actor = resolve_unique("CanBeRobbedActor", kCanBeRobbed, 14, 18);
  pickpocket.return_bool = resolve_unique("ScriptReturnBool", kCanBeRobbed, 60, 64);
  pickpocket.send_report = [](const kcd2s::integration::PickpocketReport& report) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_pickpocket_report(report);
  };
  log("PICKPOCKET %s",
      kcds::runtime::pickpocket::install(pickpocket, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
