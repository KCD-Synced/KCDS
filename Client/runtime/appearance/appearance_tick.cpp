#include "appearance_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../inventory/inventory_tick.hpp"
#include "../noclip/free_camera.hpp"
#include "components.hpp"
#include "disguise.hpp"
#include "player_skeleton.hpp"
#include "visibility.hpp"
#include "worn_only.hpp"

#include <vector>

namespace kcds::game_client {

// Puts on what the resources asked the local player to look like (Player.setAppearance), oldest
// request first.
void tick_appearance_requests(void* local_actor) {
  std::vector<kcd2s::integration::AppearanceRequest> requests;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    requests = client->peer().take_appearance_requests();
  }
  for (const auto& request : requests) {
    // A gender change puts the body on that gender's skeleton first.
    if (request.gender && kcds::runtime::appearance::player_skeleton::keep(
                              local_actor, *request.gender == "female")) {
      hold_local_inventory();
    }
    kcds::runtime::appearance::components::put_on_request(local_actor, request);
  }
  if (kcds::runtime::appearance::player_skeleton::settle(local_actor)) {
    redress_local_body();
  }
}

// Keeps the local player's body drawn as the resources last asked (Player.setVisible); returns
// whether they want it drawn, which every other client follows. During a NoClip flight the player
// does not see their own body: it is hidden in this game only,
// so other players still see a body that flies with the camera. A flight started with showBody
// keeps it drawn.
bool tick_body_visible(void* local_entity) {
  static bool logged = true;
  bool visible = true;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    visible = client->peer().body_visible();
  }
  if (visible != logged) {
    logged = visible;
    log("APPEARANCE body %s", visible ? "drawn" : "hidden");
  }
  const bool drawn_here = visible && !kcds::runtime::noclip::hides_body();
  kcds::runtime::appearance::visibility::keep_local(local_entity, drawn_here);
  return visible;
}

// Puts on and takes off what the resources want the local player to wear outside their
// inventory (Player.wearItem).
void tick_worn_only(void* local_actor, void* local_entity) {
  std::vector<kcd2s::integration::ItemClassId> wanted;
  std::uint64_t revision = 0;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    wanted = client->peer().worn();
    revision = client->peer().worn_revision();
  }
  kcds::runtime::appearance::worn_only::keep(local_actor, local_entity, wanted, revision);
}

// Keeps the local player's body drawn as the mesh the resources last asked for
// (Player.setDisguise); returns the mesh it wears, "" for its own body.
std::string tick_disguise(void* local_entity) {
  std::string model;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    model = client->peer().disguise();
  }
  return kcds::runtime::appearance::disguise::keep_local(local_entity, model);
}

}  // namespace kcds::game_client
