#include "body_fit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace kcds::runtime::appearance::refit {
namespace {

using Vec3 = std::array<float, 3>;

constexpr Matrix34 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

Matrix34 multiply(const Matrix34& a, const Matrix34& b) {
  Matrix34 out{};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 4; ++column) {
      float value = 0.0f;
      for (int k = 0; k < 3; ++k) {
        value += a[row * 4 + k] * b[k * 4 + column];
      }
      if (column == 3) {
        value += a[row * 4 + 3];
      }
      out[row * 4 + column] = value;
    }
  }
  return out;
}

// The 3x3 part's inverse; false when it has none.
bool invert_linear(const Matrix34& m, std::array<float, 9>& out) {
  const float a = m[0], b = m[1], c = m[2];
  const float d = m[4], e = m[5], f = m[6];
  const float g = m[8], h = m[9], i = m[10];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12f) {
    return false;
  }
  const float s = 1.0f / det;
  out = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s,
         (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s,
         (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
  return true;
}

Matrix34 inverse(const Matrix34& m) {
  std::array<float, 9> linear{};
  if (!invert_linear(m, linear)) {
    return kIdentity;
  }
  Matrix34 out{};
  for (int row = 0; row < 3; ++row) {
    for (int k = 0; k < 3; ++k) {
      out[row * 4 + k] = linear[row * 3 + k];
    }
    out[row * 4 + 3] = -(linear[row * 3] * m[3] + linear[row * 3 + 1] * m[7] +
                         linear[row * 3 + 2] * m[11]);
  }
  return out;
}

Vec3 apply(const Matrix34& m, const float* p) {
  return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
          m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
          m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

Vec3 apply_linear(const Matrix34& m, const float* v) {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
          m[8] * v[0] + m[9] * v[1] + m[10] * v[2]};
}

float dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float length(const Vec3& v) {
  return std::sqrt(dot(v, v));
}

Vec3 minus(const Vec3& a, const Vec3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

// A vertex's joints blended by its weights (the weights made to sum to 1); the identity for a
// vertex without weights.
Matrix34 blended(const Influence& influence, const std::vector<Matrix34>& carried) {
  Matrix34 out{};
  float total = 0.0f;
  for (std::size_t k = 0; k < 4; ++k) {
    const float weight = influence.weights[k];
    if (weight <= 0.0f || influence.joints[k] >= carried.size()) {
      continue;
    }
    const Matrix34& joint = carried[influence.joints[k]];
    for (std::size_t n = 0; n < 12; ++n) {
      out[n] += weight * joint[n];
    }
    total += weight;
  }
  if (total <= 0.0f) {
    return kIdentity;
  }
  for (float& value : out) {
    value /= total;
  }
  return out;
}

}  // namespace

std::vector<Matrix34> carry(const std::vector<Joint>& piece_joints,
                            const std::vector<Joint>& her_joints) {
  std::unordered_map<std::string, const Joint*> hers;
  for (const Joint& joint : her_joints) {
    hers.emplace(joint.name, &joint);
  }
  std::vector<Matrix34> out(piece_joints.size(), kIdentity);
  std::vector<bool> done(piece_joints.size(), false);
  // Parents come before their children in the file, so an ancestor is ready when a child asks.
  for (std::size_t index = 0; index < piece_joints.size(); ++index) {
    const Joint& joint = piece_joints[index];
    const auto found = hers.find(joint.name);
    if (found != hers.end()) {
      out[index] = multiply(found->second->bind, inverse(joint.bind));
    } else if (joint.parent >= 0 && std::size_t(joint.parent) < index && done[joint.parent]) {
      out[index] = out[joint.parent];
    }
    done[index] = true;
  }
  return out;
}

std::size_t BodyField::CellHash::operator()(const Cell& cell) const {
  return std::size_t(cell.x) * 73856093u ^ std::size_t(cell.y) * 19349663u ^
         std::size_t(cell.z) * 83492791u;
}

BodyField::Cell BodyField::cell_of(const Vec3& point) const {
  return {int(std::floor(point[0] / cell_size_)), int(std::floor(point[1] / cell_size_)),
          int(std::floor(point[2] / cell_size_))};
}

bool BodyField::build(const SkinFile& male_body, const std::vector<Joint>& her_joints,
                      const std::vector<float>& her_body, const FieldSettings& settings) {
  settings_ = settings;
  points_.clear();
  moves_.clear();
  grid_.clear();
  const std::size_t count = male_body.positions.size() / 3;
  if (count == 0 || male_body.normals.size() != male_body.positions.size() ||
      male_body.influences.size() != count || her_body.size() < 3) {
    return false;
  }
  const std::vector<Matrix34> carried = carry(male_body.joints, her_joints);
  std::vector<Vec3> normals(count);
  points_.resize(count);
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    const Matrix34 joint = blended(male_body.influences[vertex], carried);
    points_[vertex] = apply(joint, &male_body.positions[vertex * 3]);
    Vec3 normal = apply_linear(joint, &male_body.normals[vertex * 3]);
    const float size = length(normal);
    normals[vertex] = size > 0.0f ? Vec3{normal[0] / size, normal[1] / size, normal[2] / size}
                                  : Vec3{0.0f, 0.0f, 0.0f};
  }

  // Her body in cells as wide as the tube.
  const float her_cell = settings.tube_radius;
  std::unordered_map<Cell, std::vector<std::size_t>, CellHash> her_grid;
  const std::size_t her_count = her_body.size() / 3;
  auto her_cell_of = [her_cell](const Vec3& p) {
    return Cell{int(std::floor(p[0] / her_cell)), int(std::floor(p[1] / her_cell)),
                int(std::floor(p[2] / her_cell))};
  };
  for (std::size_t vertex = 0; vertex < her_count; ++vertex) {
    const Vec3 p{her_body[vertex * 3], her_body[vertex * 3 + 1], her_body[vertex * 3 + 2]};
    her_grid[her_cell_of(p)].push_back(vertex);
  }

  // Each carried male vertex: the move along its normal onto her nearest surface in the tube.
  moves_.assign(count, Vec3{0.0f, 0.0f, 0.0f});
  std::vector<bool> found(count, false);
  const int steps = int(std::ceil(settings.reach / her_cell));
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    const Vec3& origin = points_[vertex];
    const Vec3& normal = normals[vertex];
    if (normal[0] == 0.0f && normal[1] == 0.0f && normal[2] == 0.0f) {
      continue;
    }
    float best_score = std::numeric_limits<float>::max();
    float best_along = 0.0f;
    std::vector<std::size_t> seen;
    for (int step = -steps; step <= steps; ++step) {
      const float t = float(step) * her_cell;
      const Vec3 at{origin[0] + normal[0] * t, origin[1] + normal[1] * t, origin[2] + normal[2] * t};
      const Cell centre = her_cell_of(at);
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dz = -1; dz <= 1; ++dz) {
            const auto cell = her_grid.find({centre.x + dx, centre.y + dy, centre.z + dz});
            if (cell == her_grid.end()) {
              continue;
            }
            for (std::size_t her : cell->second) {
              const Vec3 p{her_body[her * 3], her_body[her * 3 + 1], her_body[her * 3 + 2]};
              const Vec3 offset = minus(p, origin);
              const float along = dot(offset, normal);
              const Vec3 side{offset[0] - along * normal[0], offset[1] - along * normal[1],
                              offset[2] - along * normal[2]};
              const float aside = length(side);
              if (aside >= settings.tube_radius || std::fabs(along) >= settings.reach) {
                continue;
              }
              const float score = std::fabs(along) + aside;
              if (score < best_score) {
                best_score = score;
                best_along = along;
              }
            }
          }
        }
      }
    }
    if (best_score < std::numeric_limits<float>::max()) {
      moves_[vertex] = {normal[0] * best_along, normal[1] * best_along, normal[2] * best_along};
      found[vertex] = true;
    }
  }
  if (std::none_of(found.begin(), found.end(), [](bool value) { return value; })) {
    points_.clear();
    moves_.clear();
    return false;
  }

  // The carried male body in cells for the smoothing and the queries.
  cell_size_ = std::max(settings.smooth_radius, 0.01f);
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    grid_[cell_of(points_[vertex])].push_back(vertex);
  }
  // Smoothing: each move becomes the mean of the moves found around it; a vertex that found no
  // surface takes its neighbours' mean and counts as found from then on.
  for (int round = 0; round < settings.smooth_rounds; ++round) {
    std::vector<Vec3> next = moves_;
    std::vector<bool> next_found = found;
    for (std::size_t vertex = 0; vertex < count; ++vertex) {
      const Cell centre = cell_of(points_[vertex]);
      Vec3 sum{0.0f, 0.0f, 0.0f};
      float total = 0.0f;
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dz = -1; dz <= 1; ++dz) {
            const auto cell = grid_.find({centre.x + dx, centre.y + dy, centre.z + dz});
            if (cell == grid_.end()) {
              continue;
            }
            for (std::size_t other : cell->second) {
              if (!found[other] ||
                  length(minus(points_[other], points_[vertex])) > settings.smooth_radius) {
                continue;
              }
              for (int k = 0; k < 3; ++k) {
                sum[k] += moves_[other][k];
              }
              total += 1.0f;
            }
          }
        }
      }
      if (total > 0.0f) {
        next[vertex] = {sum[0] / total, sum[1] / total, sum[2] / total};
        next_found[vertex] = true;
      }
    }
    moves_ = std::move(next);
    found = std::move(next_found);
  }
  return true;
}

std::array<float, 3> BodyField::move_at(const std::array<float, 3>& point) const {
  if (points_.empty()) {
    return {0.0f, 0.0f, 0.0f};
  }
  const Cell centre = cell_of(point);
  const int most_rings = int(std::ceil(settings_.fade / cell_size_)) + 1;
  std::vector<std::pair<float, std::size_t>> near;
  for (int ring = 0; ring <= most_rings; ++ring) {
    for (int dx = -ring; dx <= ring; ++dx) {
      for (int dy = -ring; dy <= ring; ++dy) {
        for (int dz = -ring; dz <= ring; ++dz) {
          if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring) {
            continue;
          }
          const auto cell = grid_.find({centre.x + dx, centre.y + dy, centre.z + dz});
          if (cell == grid_.end()) {
            continue;
          }
          for (std::size_t vertex : cell->second) {
            near.emplace_back(length(minus(points_[vertex], point)), vertex);
          }
        }
      }
    }
    // Every point closer than `ring` cells has been seen now.
    if (near.size() >= settings_.nearest) {
      std::nth_element(near.begin(), near.begin() + (settings_.nearest - 1), near.end());
      if (near[settings_.nearest - 1].first <= float(ring) * cell_size_) {
        break;
      }
    }
  }
  if (near.empty()) {
    return {0.0f, 0.0f, 0.0f};
  }
  std::sort(near.begin(), near.end());
  if (near.size() > settings_.nearest) {
    near.resize(settings_.nearest);
  }
  const float keep = std::clamp(1.0f - near.front().first / settings_.fade, 0.0f, 1.0f);
  if (keep <= 0.0f) {
    return {0.0f, 0.0f, 0.0f};
  }
  Vec3 sum{0.0f, 0.0f, 0.0f};
  float total = 0.0f;
  for (const auto& [distance, vertex] : near) {
    const float weight = 1.0f / (distance * distance + 1e-6f);
    for (int k = 0; k < 3; ++k) {
      sum[k] += moves_[vertex][k] * weight;
    }
    total += weight;
  }
  return {sum[0] / total * keep, sum[1] / total * keep, sum[2] / total * keep};
}

std::vector<float> fit(const SkinFile& piece, const std::vector<Matrix34>& carried,
                       const BodyField& field) {
  const std::size_t count = piece.positions.size() / 3;
  if (count == 0 || piece.influences.size() != count) {
    return {};
  }
  std::vector<float> out = piece.positions;
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    const Matrix34 joint = blended(piece.influences[vertex], carried);
    const Vec3 on_her = apply(joint, &piece.positions[vertex * 3]);
    const Vec3 move = field.move_at(on_her);
    if (move[0] == 0.0f && move[1] == 0.0f && move[2] == 0.0f) {
      continue;
    }
    // The move in the piece's own bind space: the engine's skinning turns it back into `move`.
    std::array<float, 9> back{};
    if (!invert_linear(joint, back)) {
      continue;
    }
    for (int row = 0; row < 3; ++row) {
      out[vertex * 3 + row] +=
          back[row * 3] * move[0] + back[row * 3 + 1] * move[1] + back[row * 3 + 2] * move[2];
    }
  }
  return out;
}

}  // namespace kcds::runtime::appearance::refit
