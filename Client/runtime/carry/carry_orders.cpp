#include "carry_orders.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../ground/ground_items.hpp"
#include "carry_items.hpp"

#include <cstring>
#include <vector>

namespace kcds::game_client {

// A new item of a class laid in the world at a point, for a carry.
void* lay_carried_item(const std::array<std::uint8_t, 16>& item_class, const float* position) {
  kcds::runtime::ground::Stack stack;
  stack.item = item_class;
  std::memcpy(stack.position, position, sizeof(stack.position));
  return kcds::runtime::ground::lay(stack);
}

// Player.holdItem and dropHeldItem: what the server told the local player to carry
// or put down. A new item is laid at its feet and taken up; a stack in view is taken up.
void tick_carry_orders(void* local_actor, const core::Vec3& position) {
  namespace integration = kcd2s::integration;
  std::vector<integration::CarryOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    orders = client->peer().take_carry_orders();
  }
  for (const integration::CarryOrder& order : orders) {
    bool done = false;
    if (order.kind == integration::CarryOrderKind::Carry) {
      const float feet[3] = {position.x, position.y, position.z};
      done = kcds::runtime::carry::carry_new(local_actor, order.item, feet);
    } else if (order.kind == integration::CarryOrderKind::CarryStack) {
      void* item = kcds::runtime::ground::claim(order.stack);
      done = kcds::runtime::carry::carry_laid(local_actor, item);
      if (!done) {
        kcds::runtime::ground::unclaim(order.stack, item);
      }
    } else {
      done = kcds::runtime::carry::put_down(local_actor,
                                            order.kind == integration::CarryOrderKind::LetGo);
    }
    log("CARRY order %u: %s", unsigned(order.kind), done ? "done" : "refused");
  }
}

}  // namespace kcds::game_client
