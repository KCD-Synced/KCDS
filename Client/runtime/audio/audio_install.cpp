// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "audio_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "resource_audio.hpp"

namespace kcds::game_client {

void install_audio() {
  // The audio system itself is the stations' (crafting/sounds.hpp); a free proxy's trigger takes
  // the empty callback infos (SAudioCallBackInfos::GetEmptyObject), as the whistle's.
  kcds::runtime::audio::Natives natives;
  natives.get_entity = get_entity;
  natives.empty_callback = resolve_unique(
      "AudioEmptyCallback",
      "48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 "
      "02 39 05 ?? ?? ?? ?? 7F ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8D 0D ?? ?? ?? ?? E8 ?? "
      "?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 0F 57 C0 48");
  natives.log = log;
  kcds::runtime::audio::install(natives);
  log("AUDIO resource sounds %s", natives.empty_callback ? "installed" : "without positions");
}

}  // namespace kcds::game_client
