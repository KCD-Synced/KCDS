// What the studio's gizmos share (Asset/Animation Studio): the free camera
// as the editor's view sees it, lines clipped to the editor's view of the world (so nothing is
// drawn over its panels), a press's line into the world, and the turning rings.
//
// The editor tells where its view of the world is on the screen ([left, bottom, right, top],
// -1 to 1) with studioBones and studioPropGizmo; set_viewport keeps it for every gizmo.
//
// Frame thread, except set_viewport and the message readers (any thread).
#pragma once

#include <cstddef>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::view {

// Nothing nearer the camera than this is drawn or picked (metres).
constexpr double kNear = 0.05;

struct Rect {
  double left = -1;
  double bottom = -1;
  double right = 1;
  double top = 1;
};

// A press in the editor's view as a line into the world: from the camera, a unit direction.
struct Ray {
  double from[3] = {0, 0, 0};
  double direction[3] = {0, 0, 1};
};

// The camera's axes and where the editor's view is, for clipping.
struct View {
  double eye[3] = {0, 0, 0};
  double forward[3] = {0, 1, 0};
  double right[3] = {1, 0, 0};
  double up[3] = {0, 0, 1};
  double tan_h = 1;
  double tan_v = 1;
  Rect rect;
};

void set_viewport(const Rect& rect);

// The free camera now (noclip camera_view), with the editor's view kept by set_viewport. False
// while there is no free camera.
bool current(View& view);

// Whether the editor's view shows the point.
bool inside(const View& view, const float* point);

// Draws the part of the line from a to b the editor's view shows: over the scene, `thickness`
// pixels; or (over_scene false) one pixel, hidden behind what stands in front of it.
void clipped_line(const View& view, const float* a, const float* b, const float* colour,
                  float thickness, bool over_scene = true);

// A number field's value (an int or a double); false for anything else.
bool number_of(const kcd2s::sdk::Value& value, double& out);

// `size` numbers of an array field; false when it is not one.
bool numbers_of(const kcd2s::sdk::Value& message, const char* name, double* out, std::size_t size);

// A press's line from a message's `from` and `direction` (made a unit); false without them.
bool read_ray(const kcd2s::sdk::Value& message, Ray& ray);

// The vector turned by the quaternion (x, y, z, w).
void turn_vector(const float q[4], const float v[3], float out[3]);

// The turning gizmo: a ring about each of three axes, round a centre.
struct Rings {
  float centre[3] = {0, 0, 0};
  float radius = 0.15f;
  // Per ring its normal (the axis) and two directions in its plane.
  float normal[3][3] = {};
  float across[3][3] = {};
  float up[3][3] = {};
};

// Rings round `centre` about the axes of the turn (x, y, z, w; the world's for none).
Rings rings_about(const float centre[3], const float turn[4], float radius);

// Draws the rings, `active` (0 X, 1 Y, 2 Z) thick; -1 for none.
void draw_rings(const View& view, const Rings& rings, int active);

// The ring a press's line meets nearest the camera (0 X, 1 Y, 2 Z), or -1.
int ring_hit(const Rings& rings, const Ray& ray);

// The colour of each axis: X red, Y green, Z blue.
extern const float kAxisColours[3][4];

}  // namespace kcds::runtime::studio::view
