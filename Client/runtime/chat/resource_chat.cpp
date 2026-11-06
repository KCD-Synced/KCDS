#include "resource_chat.hpp"

#include <mutex>
#include <string>
#include <vector>

#include "chat_overlay.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::chat {
namespace {

namespace sdk = kcd2s::sdk;
namespace native_chat = kcd2s::native_chat;

std::mutex outgoing_mutex;
std::vector<std::string> outgoing;
// As many lines as the overlay itself keeps waiting; more from a script are dropped.
constexpr std::size_t kMaxOutgoing = 16;

const void* reported_session = nullptr;
bool reported = false;
bool reported_open = false;
bool reported_visible = true;

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "chatOut") {
    const sdk::Value* text = kcd2s::integration::script_field(message, "text");
    if (text && text->tag() == sdk::Tag::Str &&
        kcd2s::integration::valid_chat_text(text->as_str())) {
      std::lock_guard<std::mutex> lock(outgoing_mutex);
      if (outgoing.size() < kMaxOutgoing) {
        outgoing.push_back(text->as_str());
      }
    }
    return true;
  }
  if (operation == "chatUI") {
    const sdk::Value* visible = kcd2s::integration::script_field(message, "visible");
    if (visible && visible->tag() == sdk::Tag::Bool) {
      native_chat::set_ui_visible(visible->as_bool());
    }
    return true;
  }
  if (operation == "chatOpen") {
    native_chat::open_input();
    return true;
  }
  if (operation == "chatClose") {
    native_chat::close_input_line();
    return true;
  }
  return false;
}

bool forward_typed(const RuntimeSender& send, const std::string& text) {
  sdk::Value::Map typed{{"op", sdk::Value(std::string("chatTyped"))}, {"text", sdk::Value(text)}};
  return send(sdk::Value(std::move(typed)));
}

void forward_received(const RuntimeSender& send, const kcd2s::integration::ChatMessage& message) {
  sdk::Value::Map line{{"op", sdk::Value(std::string("chatMessage"))},
                       {"author", sdk::Value(message.author)},
                       {"text", sdk::Value(message.text)},
                       {"color", sdk::Value(static_cast<std::int64_t>(message.color))}};
  send(sdk::Value(std::move(line)));
}

std::vector<std::string> take_outgoing() {
  std::lock_guard<std::mutex> lock(outgoing_mutex);
  std::vector<std::string> taken;
  taken.swap(outgoing);
  return taken;
}

void pump(const void* session, const RuntimeSender& send) {
  if (session != reported_session) {
    // Chat.setUIVisible holds for one session.
    reported_session = session;
    reported = false;
    native_chat::set_ui_visible(true);
    std::lock_guard<std::mutex> lock(outgoing_mutex);
    outgoing.clear();
  }
  // The chat key while a resource hid the chat: one chatKey for each press.
  for (unsigned presses = native_chat::take_hidden_chat_keys(); presses > 0; --presses) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("chatKey"))}}));
  }
  const bool open = native_chat::editing();
  const bool visible = native_chat::ui_visible();
  if (reported && open == reported_open && visible == reported_visible) {
    return;
  }
  sdk::Value::Map host{{"op", sdk::Value(std::string("chatHost"))},
                       {"open", sdk::Value(open)},
                       {"visible", sdk::Value(visible)}};
  if (send(sdk::Value(std::move(host)))) {
    reported = true;
    reported_open = open;
    reported_visible = visible;
  }
}

}  // namespace kcds::runtime::chat
