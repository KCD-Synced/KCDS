// Voice is heard from the world, not from the inventory screen's studio.
#include "listener_place.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::voice::listen_from;
using kcds::runtime::voice::ListenerPlace;

namespace {

void view_at(float matrix[12], float x, float y, float z) {
  for (int index = 0; index < 12; ++index) {
    matrix[index] = 0.0f;
  }
  matrix[0] = matrix[5] = matrix[10] = 1.0f;
  matrix[3] = x;
  matrix[7] = y;
  matrix[11] = z;
}

}  // namespace

KCD2S_TEST(listener_place, the_inventory_studio_keeps_the_world_place) {
  ListenerPlace place;
  float world[12];
  float studio[12];
  view_at(world, 754.8f, 3348.8f, 143.6f);
  view_at(studio, -3909.4f, -4001.7f, 51.3f);
  const float body[3] = {754.7f, 3348.8f, 142.0f};
  KCD2S_CHECK_EQ(listen_from(place, world, body)[3], 754.8f);
  // The inventory open: its studio camera kilometres from the body is not heard from.
  KCD2S_CHECK_EQ(listen_from(place, studio, body)[3], 754.8f);
  KCD2S_CHECK_EQ(listen_from(place, studio, body)[7], 3348.8f);
  // Closed again: the world camera.
  float moved[12];
  view_at(moved, 760.0f, 3350.0f, 143.6f);
  KCD2S_CHECK_EQ(listen_from(place, moved, body)[3], 760.0f);
}

KCD2S_TEST(listener_place, the_camera_is_followed_near_the_body) {
  ListenerPlace place;
  float first[12];
  float near[12];
  float far[12];
  view_at(first, 0.0f, 0.0f, 0.0f);
  view_at(near, 30.0f, 0.0f, 0.0f);
  view_at(far, 5000.0f, 0.0f, 0.0f);
  const float body[3] = {0.0f, 0.0f, 0.0f};
  listen_from(place, first, body);
  // A camera behind or above the body (third person, a death camera): followed.
  KCD2S_CHECK_EQ(listen_from(place, near, body)[3], 30.0f);
  // The body teleported with its camera: followed.
  const float moved_body[3] = {5000.0f, 0.0f, 0.0f};
  KCD2S_CHECK_EQ(listen_from(place, far, moved_body)[3], 5000.0f);
  // No body known (outside a world): the camera.
  ListenerPlace fresh;
  KCD2S_CHECK_EQ(listen_from(fresh, far, nullptr)[3], 5000.0f);
  KCD2S_CHECK_EQ(listen_from(fresh, first, nullptr)[3], 0.0f);
}
