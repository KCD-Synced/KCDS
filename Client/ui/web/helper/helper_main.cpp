// kcds-web-helper.exe: CEF's renderer, GPU and utility processes for resource web views.
//
// In the renderer it gives every page the bridge function sendToGame(name, json): the name must be
// a string and the payload a string or null, as the page bridge contract says. The call goes to
// the browser process (the game client DLL), which checks the frame's origin against the view's
// lock and hands it to the owning resource. It also reports when an editable element gains or
// loses the focus, so the game can stop routing keys while the player types.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "web_protocol.hpp"

namespace {

namespace protocol = kcds::web::protocol;

// window.sendToGame(name, json)
class PageEventCall : public CefV8Handler {
 public:
  bool Execute(const CefString& /*name*/, CefRefPtr<CefV8Value> /*object*/,
               const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& result,
               CefString& exception) override {
    if (well_formed(arguments)) {
      forward_to_browser(arguments);
      result = CefV8Value::CreateUndefined();
    } else {
      exception = "sendToGame(name, json) takes an event name and a string or null";
    }
    return true;
  }

 private:
  static bool well_formed(const CefV8ValueList& arguments) {
    return arguments.size() == 2 && arguments[0]->IsString() &&
           (arguments[1]->IsNull() || arguments[1]->IsString());
  }

  // Name, payload and the sending page's address go to the browser process.
  static void forward_to_browser(const CefV8ValueList& arguments) {
    CefRefPtr<CefFrame> page = CefV8Context::GetCurrentContext()->GetFrame();
    CefRefPtr<CefProcessMessage> call = CefProcessMessage::Create(protocol::kCallEvent);
    CefRefPtr<CefListValue> fields = call->GetArgumentList();
    fields->SetString(0, arguments[0]->GetStringValue());
    fields->SetString(2, page->GetURL());
    if (arguments[1]->IsNull()) {
      fields->SetNull(1);
    } else {
      fields->SetString(1, arguments[1]->GetStringValue());
    }
    page->SendProcessMessage(PID_BROWSER, call);
  }

  IMPLEMENT_REFCOUNTING(PageEventCall);
};

class HelperApp : public CefApp, public CefRenderProcessHandler {
 public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    protocol::register_schemes(registrar);
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return this;
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    (void)browser;
    (void)frame;
    CefRefPtr<CefV8Value> function = CefV8Value::CreateFunction("sendToGame", new PageEventCall());
    context->GetGlobal()->SetValue("sendToGame", function, V8_PROPERTY_ATTRIBUTE_READONLY);
  }

  void OnFocusedNodeChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefDOMNode> node) override {
    bool editable = node && node->IsEditable();
    bool& editing = editing_[browser ? browser->GetIdentifier() : 0];
    if (editable == editing) {
      return;
    }
    editing = editable;
    if (!frame) {
      return;
    }
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(protocol::kInputFocus);
    message->GetArgumentList()->SetBool(0, editable);
    frame->SendProcessMessage(PID_BROWSER, message);
  }

 private:
  std::map<int, bool> editing_;  // per browser: an editable element has the focus

  IMPLEMENT_REFCOUNTING(HelperApp);
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  CefMainArgs arguments(instance);
  CefRefPtr<HelperApp> application = new HelperApp();
  return CefExecuteProcess(arguments, application, nullptr);
}
