#include "../landings.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

using namespace kcds::runtime::ranged;

// The game's objects as the landings read them: an item's class at + 0x48, its GUID at + 8.
struct ItemClass {
  std::array<std::byte, 8> padding{};
  std::array<std::uint8_t, 16> guid{};
};

struct Item {
  std::array<std::byte, 0x48> padding{};
  ItemClass* item_class = nullptr;
};

static_assert(offsetof(ItemClass, guid) == 8);
static_assert(offsetof(Item, item_class) == 0x48);

constexpr std::uint32_t kStaticArrow = 1;
constexpr std::uint32_t kRigidArrow = 2;
constexpr std::uint32_t kBodilessArrow = 3;

ItemClass arrow_class;
Item arrow_item;
int removed = 0;

bool transform(std::uint32_t entity, float* matrix) {
  const float identity[12] = {1, 0, 0, 10.0f + float(entity), 0, 1, 0, 20, 0, 0, 1, 2};
  std::memcpy(matrix, identity, sizeof(identity));
  return true;
}

void* item_of(std::uint32_t) {
  return &arrow_item;
}

void remove(std::uint32_t) {
  ++removed;
}

int physics(std::uint32_t entity) {
  if (entity == kStaticArrow) {
    return 1;
  }
  if (entity == kRigidArrow) {
    return 2;
  }
  return 0;
}

// Fires one shot of the arrow class, lets the pickable come to rest and returns the landing.
std::vector<kcd2s::integration::ShotLanded> land(std::uint32_t entity) {
  reset_landings();
  const float origin[3] = {0, 20, 2};
  expect_landing(true, arrow_class.guid, origin);
  pickable_spawned(entity);
  // Old enough to be looked at, then still between two looks.
  for (int look = 0; look < 4; ++look) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tick_landings();
  }
  return take_landings();
}

}  // namespace

KCD2S_TEST(ranged_landings, an_arrow_without_a_moving_body_is_reported_stuck) {
  arrow_class.guid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  arrow_item.item_class = &arrow_class;
  LandingNatives natives;
  natives.entity_transform = transform;
  natives.level_item_of = item_of;
  natives.remove_entity = remove;
  natives.physics_type = physics;
  KCD2S_CHECK(install_landings(natives, nullptr));

  // Stuck in a target with a static body: it stays there.
  auto landed = land(kStaticArrow);
  KCD2S_CHECK(landed.size() == 1);
  KCD2S_CHECK(landed[0].stuck);
  KCD2S_CHECK(landed[0].position[0] == 11.0f);

  // No body at all: stuck too.
  landed = land(kBodilessArrow);
  KCD2S_CHECK(landed.size() == 1);
  KCD2S_CHECK(landed[0].stuck);

  // Fell or bounced off, a rigid body: it lies where it came to rest.
  landed = land(kRigidArrow);
  KCD2S_CHECK(landed.size() == 1);
  KCD2S_CHECK(!landed[0].stuck);
  KCD2S_CHECK(removed == 3);
}
