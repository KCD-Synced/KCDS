#include "kcd2s/replication/interest.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;

static bool has(const std::vector<EntityId>& v, EntityId e) {
  return std::find(v.begin(), v.end(), e) != v.end();
}

KCD2S_TEST(interest, query_by_distance) {
  InterestGrid g(InterestConfig{32.0f});
  g.set_position(1, Vec3{0, 0, 0});
  g.set_position(2, Vec3{10, 0, 0});   // near
  g.set_position(3, Vec3{500, 0, 0});  // far
  g.set_position(4, Vec3{0, 0, 40});   // just outside a 32-radius but within 50

  auto within50 = g.query(Vec3{0, 0, 0}, 50.0f);
  KCD2S_CHECK(has(within50, 1));
  KCD2S_CHECK(has(within50, 2));
  KCD2S_CHECK(has(within50, 4));
  KCD2S_CHECK(!has(within50, 3));  // 500 away, excluded
}

KCD2S_TEST(interest, y_axis_ignored_horizontal_plane) {
  InterestGrid g;
  g.set_position(1, Vec3{0, 1000, 0});  // far in Y only
  auto q = g.query(Vec3{0, 0, 0}, 10.0f);
  KCD2S_CHECK(has(q, 1));  // horizontal distance is 0
}

KCD2S_TEST(interest, incremental_move_updates_cells) {
  InterestGrid g(InterestConfig{32.0f});
  g.set_position(1, Vec3{0, 0, 0});
  KCD2S_CHECK(has(g.query(Vec3{0, 0, 0}, 16.0f), 1));

  g.set_position(1, Vec3{1000, 0, 1000});                    // move far (no full rebuild)
  KCD2S_CHECK(!has(g.query(Vec3{0, 0, 0}, 16.0f), 1));       // gone from origin
  KCD2S_CHECK(has(g.query(Vec3{1000, 0, 1000}, 16.0f), 1));  // present at new spot
  KCD2S_CHECK_EQ(g.entity_count(), std::size_t{1});          // not duplicated
}

KCD2S_TEST(interest, remove) {
  InterestGrid g;
  g.set_position(7, Vec3{0, 0, 0});
  g.remove(7);
  KCD2S_CHECK(!g.contains(7));
  KCD2S_CHECK(g.query(Vec3{0, 0, 0}, 100.0f).empty());
}

KCD2S_TEST(interest, velocity_lookahead_pulls_entity_ahead) {
  InterestGrid g(InterestConfig{32.0f});
  // Entity 80 units ahead on +x; range 50 misses it, but a +x velocity lookahead reaches it.
  g.set_position(1, Vec3{80, 0, 0});
  KCD2S_CHECK(!has(g.query(Vec3{0, 0, 0}, 50.0f), 1));
  KCD2S_CHECK(has(g.query(Vec3{0, 0, 0}, 50.0f, Vec3{60, 0, 0}), 1));
}

KCD2S_TEST(interest, scorer_orders_sensibly) {
  // A near high-priority entity should outrank a far low-priority one.
  const float near_high = score_entity(Priority::High, 100, 100, 5.0f, false);
  const float far_low = score_entity(Priority::Low, 100, 100, 500.0f, false);
  KCD2S_CHECK(near_high > far_low);

  // Starvation: an equal entity not sent for a long time rises above a just-sent one.
  const float fresh = score_entity(Priority::Med, 100, 100, 50.0f, false);
  const float starved = score_entity(Priority::Med, 100, 10, 50.0f, false);
  KCD2S_CHECK(starved > fresh);

  // Importance boost dominates (viewers/avatars).
  const float important_far = score_entity(Priority::Low, 100, 100, 400.0f, true);
  const float normal_near = score_entity(Priority::Med, 100, 100, 5.0f, false);
  KCD2S_CHECK(important_far > normal_near);
}

// KCD2 world coordinates are Z-up, so the game host measures interest in the (x, y)
// plane and ignores height.
KCD2S_TEST(interest, z_up_grid_uses_the_xy_plane) {
  InterestGrid g(InterestConfig{32.0f, UpAxis::Z});
  g.set_position(1, Vec3{0, 0, 1000});  // far above the viewer only
  g.set_position(2, Vec3{0, 60, 0});    // 60 m away along y
  g.set_position(3, Vec3{30, 30, 0});   // about 42 m away diagonally
  auto q = g.query(Vec3{0, 0, 0}, 50.0f);
  KCD2S_CHECK(has(q, 1));
  KCD2S_CHECK(!has(q, 2));
  KCD2S_CHECK(has(q, 3));
  KCD2S_CHECK_EQ(g.horizontal_distance_sq(Vec3{3, 4, 100}, Vec3{0, 0, 0}), 25.0f);
  KCD2S_CHECK(has(g.query(Vec3{0, 0, 0}, 50.0f, Vec3{0, 20, 0}), 2));  // lookahead along +y
}

KCD2S_TEST(interest, overlapping_lookahead_preserves_first_encounter_order_without_duplicates) {
  for (const auto up : {UpAxis::Y, UpAxis::Z}) {
    InterestGrid grid(InterestConfig{1.0f, up});
    std::vector<std::pair<EntityId, Vec3>> positions;
    for (int x = -7; x <= 7; ++x) {
      for (int depth = -7; depth <= 7; ++depth) {
        const Vec3 position = up == UpAxis::Y ? Vec3{float(x) + 0.25f, 0, float(depth) + 0.25f}
                                              : Vec3{float(x) + 0.25f, float(depth) + 0.25f, 0};
        const auto id = EntityId(positions.size() + 1);
        grid.set_position(id, position);
        positions.emplace_back(id, position);
      }
    }
    const Vec3 viewer{};
    constexpr float range = 1.25f;
    // 225 occupied cells exceeds both 5x5 query boxes, so these take the box path.
    for (const auto shift : {0.0f, 0.25f, 1.0f, -1.0f, 2.0f, -4.0f}) {
      const Vec3 velocity = up == UpAxis::Y ? Vec3{shift, 0, shift / 2} : Vec3{shift, shift / 2, 0};
      std::vector<EntityId> expected;
      std::unordered_set<EntityId> seen;
      // Independently enumerate the old first-encounter contract: viewer box,
      // then lookahead box, with a set only in this test oracle.
      for (const Vec3 center : {viewer, velocity}) {
        const int center_x = int(std::floor(center.x));
        const int center_depth = int(std::floor(up == UpAxis::Y ? center.z : center.y));
        for (int x = center_x - 2; x <= center_x + 2; ++x) {
          for (int depth = center_depth - 2; depth <= center_depth + 2; ++depth) {
            for (const auto& [id, position] : positions) {
              const float position_depth = up == UpAxis::Y ? position.z : position.y;
              if (int(std::floor(position.x)) != x || int(std::floor(position_depth)) != depth) {
                continue;
              }
              if ((grid.horizontal_distance_sq(position, viewer) <= range * range ||
                   grid.horizontal_distance_sq(position, velocity) <= range * range) &&
                  seen.insert(id).second) {
                expected.push_back(id);
              }
            }
          }
        }
      }
      const auto actual = grid.query(viewer, range, velocity);
      KCD2S_CHECK(actual == expected);
      KCD2S_CHECK_EQ(std::unordered_set<EntityId>(actual.begin(), actual.end()).size(),
                     actual.size());
    }
  }
}

KCD2S_TEST(interest, occupied_cell_fallback_stays_unique_after_moves_and_removals) {
  InterestGrid grid(InterestConfig{32.0f, UpAxis::Z});
  grid.set_position(1, {0, 0, 0});
  grid.set_position(2, {1, 1, 0});
  grid.set_position(3, {2, 2, 0});
  grid.set_position(4, {300, 300, 0});
  grid.set_position(2, {100, 0, 0});
  grid.set_position(2, {1, 1, 0});
  grid.remove(3);
  grid.set_position(3, {3, 3, 0});
  const auto near = grid.query({}, 50, {10, 10, 0});
  KCD2S_CHECK_EQ(near.size(), std::size_t{3});
  KCD2S_CHECK((std::unordered_set<EntityId>(near.begin(), near.end()) ==
               std::unordered_set<EntityId>{1, 2, 3}));
  // A broad query visits every occupied cell once and accepts each member once.
  const auto broad = grid.query({}, 1000, {10, 10, 0});
  KCD2S_CHECK_EQ(broad.size(), std::size_t{4});
  KCD2S_CHECK_EQ(std::unordered_set<EntityId>(broad.begin(), broad.end()).size(), broad.size());
}
