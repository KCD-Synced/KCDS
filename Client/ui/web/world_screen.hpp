// A web view drawn on a screen in the world (Web.setViewWorldScreen): its four corners are world
// points, projected every frame with that frame's camera, and the page is drawn onto them with a
// perspective transform, as a cinema screen or a notice board would show it. Pure: web_host.cpp
// draws with it.
#pragma once

#include <array>
#include <cmath>

namespace kcds::web::world_screen {

// The screen's corners in the world: top-left, top-right, bottom-right, bottom-left, as the page
// shows them to someone in front of it.
struct Corners {
  std::array<float, 12> points{};
};

// The camera: its 3x4 matrix by rows (right, forward and up columns, the position in the fourth)
// and its vertical field of view in radians.
struct Camera {
  const float* matrix = nullptr;
  float fov = 0;
};

// Points closer to the camera than this are behind it.
inline constexpr float kNearPlane = 0.05f;

// A world point on the screen of `width` x `height` pixels; false behind the camera.
inline bool project(const Camera& camera, const float* point, float width, float height, float& x,
                    float& y) {
  const float* m = camera.matrix;
  const float dx = point[0] - m[3];
  const float dy = point[1] - m[7];
  const float dz = point[2] - m[11];
  const float right = dx * m[0] + dy * m[4] + dz * m[8];
  const float forward = dx * m[1] + dy * m[5] + dz * m[9];
  const float up = dx * m[2] + dy * m[6] + dz * m[10];
  if (forward <= kNearPlane) {
    return false;
  }
  const float tan_half = std::tan(camera.fov / 2);
  const float aspect = width / height;
  x = (1 + right / (forward * tan_half * aspect)) * 0.5f * width;
  y = (1 - up / (forward * tan_half)) * 0.5f * height;
  return true;
}

// Whether the camera is in front of the screen (the side its page faces).
inline bool facing_camera(const Camera& camera, const Corners& corners) {
  const float* p = corners.points.data();
  const float across[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]};
  const float down[3] = {p[9] - p[0], p[10] - p[1], p[11] - p[2]};
  // The page's front: down x across.
  const float normal[3] = {down[1] * across[2] - down[2] * across[1],
                           down[2] * across[0] - down[0] * across[2],
                           down[0] * across[1] - down[1] * across[0]};
  const float* m = camera.matrix;
  const float to_camera[3] = {m[3] - p[0], m[7] - p[1], m[11] - p[2]};
  return normal[0] * to_camera[0] + normal[1] * to_camera[1] + normal[2] * to_camera[2] > 0;
}

// The perspective transform that takes the page (0, 0)-(page_width, page_height) onto the four
// screen points (x0, y0 top-left, then clockwise), as Direct2D's 4x4 matrix takes a row vector
// [x y 0 1]: rows 11-14, 21-24, 31-34, 41-44. False when the quad is degenerate.
inline bool page_to_quad(const float quad[8], float page_width, float page_height, float out[16]) {
  const float x0 = quad[0], y0 = quad[1], x1 = quad[2], y1 = quad[3];
  const float x2 = quad[4], y2 = quad[5], x3 = quad[6], y3 = quad[7];
  // The unit square onto the quad (u right, v down), as a projective map:
  // X = (a u + b v + c) / (g u + h v + 1), Y = (d u + e v + f) / (g u + h v + 1).
  const float dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
  const float dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
  float g = 0, h = 0;
  if (dx3 != 0 || dy3 != 0) {
    const float det = dx1 * dy2 - dx2 * dy1;
    if (std::fabs(det) < 1e-6f) {
      return false;
    }
    g = (dx3 * dy2 - dx2 * dy3) / det;
    h = (dx1 * dy3 - dx3 * dy1) / det;
  }
  const float a = x1 - x0 + g * x1, b = x3 - x0 + h * x3, c = x0;
  const float d = y1 - y0 + g * y1, e = y3 - y0 + h * y3, f = y0;
  if (page_width <= 0 || page_height <= 0) {
    return false;
  }
  // u = x / page_width, v = y / page_height.
  const float matrix[16] = {
      a / page_width,
      d / page_width,
      0,
      g / page_width,
      b / page_height,
      e / page_height,
      0,
      h / page_height,
      0,
      0,
      1,
      0,
      c,
      f,
      0,
      1,
  };
  for (int index = 0; index < 16; ++index) {
    out[index] = matrix[index];
  }
  return true;
}

// The page point under a screen point (x, y), through the inverse of `transform`
// (page_to_quad's): where a click on the screen lands on the page. False when it has no inverse
// or the point lies beyond the quad's horizon.
inline bool screen_to_page(const float transform[16], float x, float y, float& page_x,
                           float& page_y) {
  // The 3x3 homography the row-vector matrix holds: screen = H [page_x page_y 1].
  const float h[9] = {transform[0], transform[4], transform[12],  //
                      transform[1], transform[5], transform[13],  //
                      transform[3], transform[7], transform[15]};
  const float inverse[9] = {
      h[4] * h[8] - h[5] * h[7], h[2] * h[7] - h[1] * h[8], h[1] * h[5] - h[2] * h[4],
      h[5] * h[6] - h[3] * h[8], h[0] * h[8] - h[2] * h[6], h[2] * h[3] - h[0] * h[5],
      h[3] * h[7] - h[4] * h[6], h[1] * h[6] - h[0] * h[7], h[0] * h[4] - h[1] * h[3],
  };
  const float determinant = h[0] * inverse[0] + h[1] * inverse[3] + h[2] * inverse[6];
  if (std::fabs(determinant) < 1e-12f) {
    return false;
  }
  const float w = inverse[6] * x + inverse[7] * y + inverse[8];
  if (std::fabs(w) < 1e-12f || w / determinant <= 0) {
    return false;
  }
  page_x = (inverse[0] * x + inverse[1] * y + inverse[2]) / w;
  page_y = (inverse[3] * x + inverse[4] * y + inverse[5]) / w;
  return true;
}

}  // namespace kcds::web::world_screen
