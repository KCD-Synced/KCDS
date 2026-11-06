// The view camera as read each frame, for Hud.worldToScreen and the web views anchored to world
// points.
#pragma once

namespace kcds::game_client {

void clear_view_camera();
void read_view_camera(bool in_world);

}  // namespace kcds::game_client
