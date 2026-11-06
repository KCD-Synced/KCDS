#include "dialogue_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/natives.hpp"
#include "dialogue_list.hpp"

#include <vector>

namespace kcds::game_client {

// The local player's conversation, read as IsInDialog does: services + 0x150 -> + 0x10 is the
// dialogue manager, the player's soul at C_Actor + 1640, its id through the soul's first virtual.
bool read_game_dialogue() {
  void* services = get_services ? get_services() : nullptr;
  if (!dialogue_lookup || !services) {
    return false;
  }
  auto bytes = static_cast<std::uint8_t*>(services);
  auto dialogues = *reinterpret_cast<std::uint8_t**>(bytes + 0x150);
  auto game_interface = *reinterpret_cast<void**>(bytes + 8);
  if (!dialogues || !game_interface) {
    return false;
  }
  void* manager = *reinterpret_cast<void**>(dialogues + 0x10);
  auto actor = static_cast<std::uint8_t*>(
      slot<void*(__fastcall*)(void*)>(game_interface, 64)(game_interface));
  void* soul = actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  if (!manager || !soul) {
    return false;
  }
  auto id = slot<const std::uint64_t*(__fastcall*)(void*)>(soul, 0)(soul);
  return id && dialogue_lookup(manager, *id, true, true) != nullptr;
}
// The server's conversations (Dialogue): its pages and closes in the game's dialogue
// list, and the local player's picks and backing out for the server.
void tick_dialogue(bool in_world) {
  std::vector<kcd2s::integration::DialogueOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_dialogue_orders();
    }
  }
  for (const auto& order : orders) {
    kcds::runtime::dialogue::apply(order);
  }
  DWORD foreground_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
  const bool keys = foreground_pid == GetCurrentProcessId() && !ui_owns_input() &&
                    !page_over_world.load() && !game_menu_open();
  kcds::runtime::dialogue::frame(in_world, game_in_dialogue.load(), keys);
  const auto choices = kcds::runtime::dialogue::take_choices();
  const auto cancels = kcds::runtime::dialogue::take_cancels();
  if (choices.empty() && cancels.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client) {
    return;
  }
  for (const auto& choice : choices) {
    client->peer().send_dialogue_choice(choice);
  }
  for (const auto& cancel : cancels) {
    client->peer().send_dialogue_cancel(cancel);
  }
}

}  // namespace kcds::game_client
