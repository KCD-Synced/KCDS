// NUI — in-game HTML UI. A resource ships an HTML `ui_page`; the client
// composites it (via CEF/Chromium) over the game and exchanges messages with it. This header is the
// host- agnostic, headless-testable half:
//
//   resource -> UI : NuiBus::send_message(resource, Value)  (delivered as window.postMessage(json))
//   UI -> resource : the page calls a named NUI callback;
//                    NuiBus::invoke_callback(resource, name, body) runs the resource's handler and
//                    returns its Value result.
//   focus/cursor   : NuiBus::set_focus(hasFocus, hasCursor) toggles input routing to the UI.
//
// Values cross the boundary as the canonical sdk::Value; value_to_json() renders one to the JSON
// the browser receives. The actual CEF browser surface + engine compositing is PREPARE-ONLY:
// INuiRenderer is the interface a Windows/engine build implements (not built here).
#ifndef KCD2S_SDK_NUI_HPP
#define KCD2S_SDK_NUI_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "kcd2s/sdk/value.hpp"

namespace kcd2s::sdk {

// Render a Value as JSON (what a NUI message becomes for the browser). Deterministic; map order is
// preserved. Bytes -> {"__bytes":[...]}, EntityRef -> {"__entity":id}, FuncRef -> {"__func":[r,i]}.
std::string value_to_json(const Value& v);

// Parse JSON into a Value (the inverse of value_to_json — a NUI page POSTs JSON, not marshalled
// bytes). Numbers without a fraction/exponent that fit become Int, else Double. The reserved
// wrappers {"__entity":id}, {"__func":[r,i]}, {"__bytes":[...]} are recognised so a
// value_to_json -> json_to_value round-trip is identity for those. Untrusted-input safe: a depth
// limit, no crash/UB/hang on arbitrary bytes, and strict trailing-garbage rejection.
struct JsonResult {
  bool ok = false;
  Value value;
};
JsonResult json_to_value(const std::string& text);
JsonResult json_to_value(const char* data, std::size_t len);

// The engine/CEF side a real client implements (PREPARE-ONLY — no CEF here). A NuiBus can be given
// one so send_message/set_focus drive the browser; headless code leaves it null and uses callbacks.
struct INuiRenderer {
  virtual ~INuiRenderer() = default;
  virtual bool CreateSurface(const std::string& resource, const std::string& url) = 0;
  virtual void DestroySurface(const std::string& resource) = 0;
  virtual void PostMessage(const std::string& resource, const std::string& json) = 0;
  virtual void SetFocused(bool has_focus, bool has_cursor) = 0;
  virtual void Resize(const std::string& resource, int w, int h) = 0;
  // TODO(env): a concrete renderer needs CEF/Chromium off-screen rendering + compositing the
  // surface onto the game via the client bridge (Present hook), and input injection when focused.
  // See and Client/bridge.
};

class NuiBus {
 public:
  using CallbackFn = std::function<Value(const Value& body)>;
  using OutgoingFn = std::function<void(const std::string& resource, const Value& msg)>;

  // Register a resource's HTML UI page (from manifest `ui_page`); optionally asks the renderer to
  // create the surface.
  void register_ui(const std::string& resource, const std::string& page);
  void unregister_ui(const std::string& resource);
  const std::string* ui_page(const std::string& resource) const;

  // resource -> UI. Delivered to the OutgoingFn (tests capture it) and, if a renderer is set,
  // posted to the browser as JSON.
  void send_message(const std::string& resource, const Value& msg);
  void set_outgoing(OutgoingFn fn) {
    outgoing_ = std::move(fn);
  }
  void set_renderer(INuiRenderer* r) {
    renderer_ = r;
  }

  // UI -> resource: a resource exposes a named callback its page can invoke.
  void register_callback(const std::string& resource, const std::string& name, CallbackFn fn);
  void clear_callbacks(const std::string& resource);  // on resource stop (crash isolation)

  // Invoke a callback with a decoded body. nullopt if there is no such callback.
  std::optional<Value> invoke_callback(const std::string& resource, const std::string& name,
                                       const Value& body);

  // Untrusted entry: the body arrives as marshalled sdk::Value bytes from the page. Decodes then
  // invokes. ok=false on a decode failure or a missing callback (never crashes).
  struct CallResult {
    bool ok = false;
    Value result;
  };
  CallResult invoke_callback_raw(const std::string& resource, const std::string& name,
                                 const std::uint8_t* data, std::size_t len);

  // As above but the body is JSON text (what a real browser `fetch` posts). Parsed with
  // json_to_value; ok=false on a JSON parse error or a missing callback.
  CallResult invoke_callback_json(const std::string& resource, const std::string& name,
                                  const std::string& json);

  // Focus/cursor: when focused, the client routes mouse/keyboard to the UI instead of the game.
  void set_focus(bool has_focus, bool has_cursor);
  bool has_focus() const {
    return has_focus_;
  }
  bool has_cursor() const {
    return has_cursor_;
  }

 private:
  static std::string key(const std::string& resource, const std::string& name);

  std::unordered_map<std::string, std::string> ui_pages_;  // resource -> page url
  std::unordered_map<std::string, CallbackFn> callbacks_;  // "resource\0name" -> handler
  OutgoingFn outgoing_;
  INuiRenderer* renderer_ = nullptr;
  bool has_focus_ = false;
  bool has_cursor_ = false;
};

}  // namespace kcd2s::sdk

#endif  // KCD2S_SDK_NUI_HPP
