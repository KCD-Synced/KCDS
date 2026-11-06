// World Architect's editor tells the client it opened or closed (the runtime's
// { op: 'editorChrome', open }, Client/resources/runtime.cjs showGameUi): while it is open the
// watermark is drawn on the editor's menu bar (watermark.hpp set_editor).
// Any thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::branding::editor_chrome {

// True when the message was an editorChrome one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The world went: the editor went with it.
void reset();

}  // namespace kcds::runtime::branding::editor_chrome
