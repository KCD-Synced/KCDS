#include "previews_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../gizmos/gizmos.hpp"
#include "../nameplates/nameplates.hpp"
#include "../markers/world_markers.hpp"
#include "ghosts.hpp"
#include "local_previews.hpp"
#include "placer.hpp"
#include "prop_entity.hpp"

#include <vector>

namespace kcds::game_client {

// The local player's prop previews and placement and the server's gizmos:
// the orders the server sent, the placement's aim and keys, its reports, and this frame's lines.
void tick_previews(void* local_entity, double now) {
  std::vector<kcd2s::integration::GhostOrder> ghosts;
  std::vector<kcd2s::integration::PlacerOrder> placements;
  std::vector<kcd2s::integration::GizmoOrder> gizmos;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    ghosts = client->peer().take_ghost_orders();
    placements = client->peer().take_placer_orders();
    gizmos = client->peer().take_gizmo_orders();
  }
  // The client scripts' own previews after the server's.
  kcds::runtime::props::local_previews::take(ghosts, placements);
  for (const auto& order : ghosts) {
    kcds::runtime::props::ghosts::apply(order);
  }
  for (const auto& order : placements) {
    kcds::runtime::props::placer::apply(order);
  }
  for (const auto& order : gizmos) {
    kcds::runtime::gizmos::apply(order, now);
  }
  if (kcds::runtime::props::placer::active()) {
    float player[3] = {};
    const bool has_player = local_entity != nullptr;
    if (has_player) {
      kcds::runtime::props::entity::world_position(local_entity, player);
    }
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    kcds::runtime::props::placer::Frame placing;
    placing.camera = view_matrix_valid ? view_matrix.data() : nullptr;
    placing.player = has_player ? player : nullptr;
    placing.keys =
        foreground_pid == GetCurrentProcessId() && !ui_owns_input() && !page_over_world.load();
    placing.now = now;
    std::vector<kcd2s::integration::PlacerReport> reports;
    kcds::runtime::props::placer::frame(placing, reports);
    namespace local = kcds::runtime::props::local_previews;
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& report : reports) {
      // A client script's session reports to its runtime, the server's to the server.
      if (local::local(report.session)) {
        local::report(report);
      } else {
        client->peer().send_placer_report(report);
      }
    }
  }
  kcds::runtime::props::ghosts::frame();
  // The client scripts' ghosts' boxes, for PropGhost.getBounds.
  for (const auto& change : kcds::runtime::props::ghosts::take_local_bounds()) {
    kcds::runtime::props::local_previews::report_bounds(change.ghost, change.present, change.min,
                                                        change.max);
  }
  kcds::runtime::gizmos::frame(now);
  // The nameplates above players: none while the game's loading screen is up (its
  // "loading" action map), where the bodies are not posed yet, nor over a game menu (inventory,
  // a container's transfer screen, the map, pause).
  const bool shown = view_matrix_valid && multiplayer_world_ready.load() &&
                     loading_down_since_request.load() && !game_menu_open();
  kcds::runtime::nameplates::frame(now, shown ? view_matrix.data() : nullptr, view_fov);
  // The server's mesh markers, drawn each frame.
  kcds::runtime::markers::frame(now);
}
}  // namespace kcds::game_client
