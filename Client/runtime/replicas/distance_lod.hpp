#pragma once

namespace kcds::game_client {
struct Frame;

// Optional native render LOD for idle distant remote players. Engine thread only;
// no animation calls, controller ticks, collision, or gameplay attachments are skipped.
void tick_remote_distance_lod(const Frame& frame);

// Whether a remote player's body needs no full update this frame: standing idle on foot, settled
// where its owner is, with nothing new from its owner. It still gets one every few frames, which
// keeps it right against anything the game did to it. A crowd is mostly this on most frames.
// Off screen a calm body also updates only every few frames while it moves. `body` is its position.
bool replica_can_rest(core::EntityId id, Remote& remote,
                      const kcd2s::replication::Mirror::Entity& latest, bool shown_settled,
                      const float body[3]);
// A calm body's frame between its full updates: it only takes its position and heading. Its full
// update comes when its owner's state changed, at once close to the camera and in its turn further
// away, and every few frames anyway.
bool replica_light_frame(core::EntityId id, Remote& remote,
                         const kcd2s::replication::Mirror::Entity& latest, const float body[3]);
// A calm body's turn for the engine's less frequent updates: true once in `every` frames, by id.
bool replica_turn(core::EntityId id, unsigned every);
// How far the body is from the camera, squared; 0 without a camera.
float replica_camera_distance_sq(const float body[3]);
// Whether the body is out of the camera's view and not close to it.
bool replica_off_screen(const float body[3]);
// Once a frame, before the bodies.
void replica_rest_new_frame();
}  // namespace kcds::game_client
