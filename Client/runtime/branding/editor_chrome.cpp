#include "editor_chrome.hpp"

#include "kcd2s/integration/script_channel.hpp"
#include "watermark.hpp"

namespace kcds::runtime::branding::editor_chrome {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "editorChrome") {
    return false;
  }
  const sdk::Value* open = integration::script_field(message, "open");
  watermark::set_editor(open && open->tag() == sdk::Tag::Bool && open->as_bool());
  return true;
}

void reset() {
  watermark::set_editor(false);
}

}  // namespace kcds::runtime::branding::editor_chrome
