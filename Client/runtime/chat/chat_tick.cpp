#include "chat_tick.hpp"

#include "../core/client_internal.hpp"
#include "chat_overlay.hpp"
#include "../core/log.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/smithing.hpp"
#include "resource_chat.hpp"

#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;
namespace native_chat = kcd2s::native_chat;

// WHGame 1.5.5 CXConsole::ShowConsole (IConsole slot 14; called by
// CScriptBind_System::ShowConsole through slot 0x70) stores the open state at IConsole + 0x1DC.
constexpr std::size_t kConsoleActiveOffset = 0x1DC;
ShowConsoleFn original_show_console = nullptr;
void __fastcall hooked_show_console(void* console, bool show, int scroll) {
  if (show && ui_owns_input()) {
    return;
  }
  original_show_console(console, show, scroll);
}
bool engine_console_open() {
  void* console = console_slot ? *console_slot : nullptr;
  if (!console)
    return false;
  return *(static_cast<const std::uint8_t*>(console) + kConsoleActiveOffset) != 0;
}
// Sends to the current client's resource runtime (network_mutex held).
kcds::runtime::chat::RuntimeSender runtime_sender() {
  local::Client* current = client.get();
  return [current](const kcd2s::sdk::Value& message) {
    return current && current->send_to_resource_runtime(message);
  };
}
void tick_chat() {
  std::vector<integration::ChatMessage> received;
  bool ready = false;
  auto outgoing = native_chat::take_submissions();
  if (std::string line = take_test_chat_line(); !line.empty()) {
    outgoing.push_back(std::move(line));
  }
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    auto& peer = client->peer();
    const auto own = client->own_avatar();
    ready = peer.world_ready() && own != core::kInvalidEntity;
    received = peer.take_chat();
    // Typed lines go through the resources' chatSend handlers first (Chat global);
    // without a running resource runtime they are sent as they are.
    const bool resources = client->resource_runtime_ready();
    for (const auto& text : outgoing) {
      if (resources && kcds::runtime::chat::forward_typed(runtime_sender(), text)) {
        continue;
      }
      if (!peer.send_chat(own, text))
        log("CHAT submission rejected locally");
    }
    for (const auto& text : kcds::runtime::chat::take_outgoing()) {
      if (!peer.send_chat(own, text))
        log("CHAT resource line rejected locally");
    }
    if (resources) {
      for (const auto& message : received) {
        kcds::runtime::chat::forward_received(runtime_sender(), message);
      }
    }
  }
  native_chat::set_ready(ready);
  native_chat::set_engine_console_open(engine_console_open());
  native_chat::set_minigame_open(kcds::runtime::crafting::smithing::own_running() ||
                                 kcds::runtime::crafting::alchemy::own_running());
  for (auto& message : received)
    native_chat::receive(std::move(message));
}
}  // namespace kcds::game_client
