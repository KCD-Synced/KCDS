#include "../world_screen.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

namespace screen = kcds::web::world_screen;

namespace {

// A camera at the origin looking north (+y), up +z: right x, forward y, up z.
const float kLookingNorth[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

bool near(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

// Where the transform puts page point (x, y).
void apply(const float m[16], float x, float y, float& out_x, float& out_y) {
  const float sx = x * m[0] + y * m[4] + m[12];
  const float sy = x * m[1] + y * m[5] + m[13];
  const float w = x * m[3] + y * m[7] + m[15];
  out_x = sx / w;
  out_y = sy / w;
}

}  // namespace

KCD2S_TEST(world_screen, a_point_ahead_lands_mid_screen_and_one_behind_does_not) {
  const screen::Camera camera{kLookingNorth, 1.0f};
  const float ahead[3] = {0, 10, 0};
  float x = 0, y = 0;
  KCD2S_CHECK(screen::project(camera, ahead, 1600, 900, x, y));
  KCD2S_CHECK(near(x, 800) && near(y, 450));
  const float behind[3] = {0, -10, 0};
  KCD2S_CHECK(!screen::project(camera, behind, 1600, 900, x, y));
}

KCD2S_TEST(world_screen, the_page_corners_land_on_the_quad_corners) {
  // A tilted quad, as a screen seen from the side.
  const float quad[8] = {100, 80, 500, 140, 480, 400, 120, 360};
  float m[16] = {};
  KCD2S_CHECK(screen::page_to_quad(quad, 1280, 720, m));
  const float page[8] = {0, 0, 1280, 0, 1280, 720, 0, 720};
  for (int corner = 0; corner < 4; ++corner) {
    float x = 0, y = 0;
    apply(m, page[corner * 2], page[corner * 2 + 1], x, y);
    KCD2S_CHECK(near(x, quad[corner * 2]) && near(y, quad[corner * 2 + 1]));
  }
  // And back: a click on the screen lands on the page point it shows.
  for (int corner = 0; corner < 4; ++corner) {
    float page_x = 0, page_y = 0;
    KCD2S_CHECK(screen::screen_to_page(m, quad[corner * 2], quad[corner * 2 + 1], page_x, page_y));
    KCD2S_CHECK(near(page_x, page[corner * 2]) && near(page_y, page[corner * 2 + 1]));
  }
  float middle_x = 0, middle_y = 0;
  apply(m, 640, 360, middle_x, middle_y);
  float back_x = 0, back_y = 0;
  KCD2S_CHECK(screen::screen_to_page(m, middle_x, middle_y, back_x, back_y));
  KCD2S_CHECK(near(back_x, 640) && near(back_y, 360));
  // A quad squashed to a line has no transform.
  const float flat[8] = {0, 0, 10, 0, 20, 0, 30, 0};
  KCD2S_CHECK(!screen::page_to_quad(flat, 1280, 720, m));
}

KCD2S_TEST(world_screen, it_shows_only_from_the_side_its_page_faces) {
  const screen::Camera camera{kLookingNorth, 1.0f};
  // A screen 10 m north, its page facing south (towards the camera): top-left is west as the
  // camera sees it.
  screen::Corners facing;
  facing.points = {-2, 10, 1, 2, 10, 1, 2, 10, -1, -2, 10, -1};
  KCD2S_CHECK(screen::facing_camera(camera, facing));
  // The same screen turned round: its back to the camera.
  screen::Corners away;
  away.points = {2, 10, 1, -2, 10, 1, -2, 10, -1, 2, 10, -1};
  KCD2S_CHECK(!screen::facing_camera(camera, away));
}
