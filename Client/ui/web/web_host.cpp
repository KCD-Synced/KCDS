// Resource web views on CEF; see include/kcds/web/web.hpp.
#include "include/kcds/web/web.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windowsx.h>
// windowsx.h macros that collide with CEF's DOM API.
#undef GetFirstChild
#undef GetNextSibling
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "include/wrapper/cef_helpers.h"
#include "page_keyboard.hpp"
#include "web_protocol.hpp"

namespace kcds::web {
namespace {

using Microsoft::WRL::ComPtr;
namespace protocol = kcds::web::protocol;

// ---- shared state ---------------------------------------------------------------------------

// The latest frame of a view, written by the CEF thread and read by the present thread: a texture
// the game's device opens, or, when Chromium and the game draw on different adapters (laptops
// with two GPUs), the pixels themselves.
struct ViewFrame {
  HANDLE shared_handle = nullptr;  // NT handle of the inter-thread texture (keyed mutex, key 0)
  // BGRA, premultiplied, width * 4 bytes a row; only when the frame cannot be shared on the GPU.
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
  int width = 0;
  int height = 0;
  unsigned generation = 0;  // changes whenever the texture is replaced or new pixels arrive
};

// A view anchored to a world point (anchor_view).
struct Anchor {
  bool active = false;
  float x = 0;
  float y = 0;
  float z = 0;
  int offset_x = 0;
  int offset_y = 0;
};

struct View {
  int id = 0;
  Anchor anchor;
  std::string owner;
  std::string url;
  std::string origin;  // the locked origin, "null" if the URL lacks one
  ViewOptions options;
  bool offscreen = false;
  ViewFrame frame;
  CefRefPtr<CefBrowser> browser;  // CEF UI thread
  HCURSOR cursor = nullptr;       // the shape the page asked for
  // Whether the page's document finished loading, and since when it has been loading.
  bool document_ready = false;
  std::chrono::steady_clock::time_point load_started = std::chrono::steady_clock::now();
  bool load_overdue = false;  // told so once per load
};

Options settings;
std::mutex state_mutex;
std::map<int, std::shared_ptr<View>> views;
std::vector<std::string> messages;
std::map<std::string, std::vector<InternalMessage>> internal_messages;

bool internal_owner(const std::string& owner) {
  return !owner.empty() && owner[0] == '@';
}
int screen_width = 0;
int screen_height = 0;
// The view camera for anchored views (set_view_camera), under state_mutex.
bool camera_valid = false;
float camera_matrix[12] = {};
float camera_fov = 0;
// Points nearer the camera than this, or behind it, have no place on screen.
constexpr float kNearPlane = 0.01f;
int input_view = 0;
// How long a focused page may take to load before the player gets the mouse and keyboard back
// (a page that never loaded held them, and the player had to quit the game).
constexpr std::chrono::seconds kFocusedLoadGrace{6};
std::atomic<bool> is_ready{false};
std::atomic<bool> has_failed{false};
std::atomic<bool> started{false};
LUID game_adapter{};
std::atomic<bool> game_adapter_known{false};
// The game's device could not open a view's texture (another adapter): frames go through memory.
std::atomic<bool> present_cannot_share{false};

template <typename... Args>
void note(const char* format, Args... args) {
  if (settings.log) {
    settings.log(format, args...);
  }
}

std::shared_ptr<View> find_view(int id) {
  auto found = views.find(id);
  return found == views.end() ? nullptr : found->second;
}

// ---- JSON -----------------------------------------------------------------------------------

std::string json_string(const std::string& text) {
  static constexpr char kPlain[] = "\"\\\n\r\t";
  static constexpr char kLetter[] = "\"\\nrt";
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    int named = -1;
    for (int index = 0; index < 5 && named < 0; ++index) {
      if (kPlain[index] == character) {
        named = index;
      }
    }
    if (named >= 0) {
      out.push_back('\\');
      out.push_back(kLetter[named]);
    } else if (byte < 0x20) {
      out += "\\u00";
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 15]);
    } else {
      out.push_back(character);
    }
  }
  return out + "\"";
}

// A JSON object from already encoded "name":value fields.
class JsonObject {
 public:
  JsonObject& field(const char* name, const std::string& text) {
    return raw(name, json_string(text));
  }
  JsonObject& field(const char* name, int number) {
    return raw(name, std::to_string(number));
  }
  JsonObject& flag(const char* name, bool value) {
    return raw(name, value ? "true" : "false");
  }
  JsonObject& raw(const char* name, const std::string& encoded) {
    text_ += text_.empty() ? "{" : ",";
    text_ += json_string(name) + ":" + encoded;
    return *this;
  }
  std::string str() const {
    return text_.empty() ? "{}" : text_ + "}";
  }

 private:
  std::string text_;
};

// Queues a browser* event for the view's owner (state_mutex held).
void queue_event(const View& view, const char* event, JsonObject payload) {
  if (internal_owner(view.owner)) {
    internal_messages[view.owner].push_back({event, std::string(), std::string()});
    return;
  }
  JsonObject message;
  message.field("type", "web-event").field("owner", view.owner).field("event", event);
  message.raw("args", "[" + payload.field("viewId", view.id).str() + "]");
  messages.push_back(message.str());
}

void queue_event(int id, const char* event, JsonObject payload) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto view = find_view(id);
  if (view) {
    queue_event(*view, event, std::move(payload));
  }
}

// The view that holds the player's input now (state_mutex held): the focused one, unless its page
// has been loading for too long. It gets the input again if its page still finishes.
int view_holding_input() {
  auto view = input_view ? find_view(input_view) : nullptr;
  if (!view || view->document_ready ||
      std::chrono::steady_clock::now() - view->load_started < kFocusedLoadGrace) {
    return input_view;
  }
  if (!view->load_overdue) {
    view->load_overdue = true;
    note("WEB view %d: the focused page has not loaded; the player gets the input back", view->id);
    queue_event(*view, "webViewFailed",
                JsonObject()
                    .field("url", view->url)
                    .field("description", "ERR_TIMED_OUT")
                    .field("errorCode", -7)
                    .flag("isMainFrame", true));
  }
  return 0;
}

// ---- URLs -----------------------------------------------------------------------------------

// scheme://host[:port], or "null" for a URL without an origin.
std::string origin_of(const std::string& url) {
  std::string origin = "null";
  CefURLParts parts;
  if (CefParseURL(url, parts)) {
    const std::string scheme = CefString(&parts.scheme).ToString();
    const std::string host = CefString(&parts.host).ToString();
    if (!scheme.empty() && !host.empty()) {
      const std::string port = CefString(&parts.port).ToString();
      origin = scheme + "://" + host;
      if (!port.empty()) {
        origin.push_back(':');
        origin += port;
      }
    }
  }
  return origin;
}

std::string host_of(const std::string& url) {
  CefURLParts parts;
  if (!CefParseURL(url, parts)) {
    return std::string();
  }
  return CefString(&parts.host).ToString();
}

bool allowed_url(const std::string& url) {
  std::string origin = origin_of(url);
  return origin.rfind("kcds://", 0) == 0 || origin.rfind("https://", 0) == 0 ||
         origin.rfind("http://", 0) == 0;
}

// ---- kcds://resources/<resource>/<path> ---------------------------------------------------------

class FileHandler;

std::mutex file_mutex;
int next_file_request = 1;
std::vector<FileRequest> file_requests;
std::map<int, CefRefPtr<FileHandler>> waiting_files;
// A file the resource runtime has not answered in this time fails its load, so the page reports
// webViewFailed instead of waiting without any event: a runtime that restarts
// or stops drops the requests it was sent.
constexpr int kFileAnswerMs = 30000;
void time_out_file_later(int request_id);

// One kcds:// load: asks the resource runtime for the file and answers once it replied.
class FileHandler : public CefResourceHandler {
 public:
  FileHandler(std::string resource, std::string path)
      : resource_(std::move(resource)), path_(std::move(path)) {
  }

  bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    (void)request;
    handle_request = false;
    if (resource_.empty() || path_.empty() || path_.find("..") != std::string::npos) {
      handle_request = true;  // malformed: answered at once as a 404
      return true;
    }
    std::lock_guard<std::mutex> lock(file_mutex);
    callback_ = callback;
    request_id_ = next_file_request++;
    waiting_files[request_id_] = this;
    file_requests.push_back({request_id_, resource_, path_});
    time_out_file_later(request_id_);
    return true;
  }

  // The runtime's answer (any thread).
  void complete(bool found, std::string bytes) {
    CefRefPtr<CefCallback> callback;
    {
      std::lock_guard<std::mutex> lock(file_mutex);
      found_ = found;
      bytes_ = std::move(bytes);
      callback = callback_;
      callback_ = nullptr;
    }
    if (callback) {
      callback->Continue();
    }
  }

  // No answer in time: the load ends as a 504.
  void time_out() {
    {
      std::lock_guard<std::mutex> lock(file_mutex);
      timed_out_ = true;
    }
    complete(false, std::string());
  }

  const std::string& resource() const {
    return resource_;
  }
  const std::string& path() const {
    return path_;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length,
                          CefString& redirect_url) override {
    (void)redirect_url;
    std::lock_guard<std::mutex> lock(file_mutex);
    if (timed_out_) {
      response->SetStatus(504);
      response->SetStatusText("Gateway Timeout");
      response->SetMimeType("text/plain");
      response_length = 0;
      return;
    }
    if (!found_) {
      response->SetStatus(404);
      response->SetStatusText("Not Found");
      response->SetMimeType("text/plain");
      response_length = 0;
      return;
    }
    std::string extension;
    std::size_t dot = path_.rfind('.');
    if (dot != std::string::npos) {
      extension = path_.substr(dot + 1);
    }
    std::string mime = CefGetMimeType(extension).ToString();
    response->SetStatus(200);
    response->SetStatusText("OK");
    response->SetMimeType(mime.empty() ? "application/octet-stream" : mime);
    response_length = static_cast<int64_t>(bytes_.size());
  }

  bool Read(void* data_out, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    (void)callback;
    std::lock_guard<std::mutex> lock(file_mutex);
    std::size_t left = bytes_.size() - offset_;
    if (!found_ || left == 0) {
      bytes_read = 0;
      return false;
    }
    std::size_t count = std::min(left, static_cast<std::size_t>(bytes_to_read));
    std::memcpy(data_out, bytes_.data() + offset_, count);
    offset_ += count;
    bytes_read = static_cast<int>(count);
    return true;
  }

  void Cancel() override {
    std::lock_guard<std::mutex> lock(file_mutex);
    waiting_files.erase(request_id_);
    callback_ = nullptr;
  }

 private:
  std::string resource_;
  std::string path_;
  int request_id_ = 0;
  CefRefPtr<CefCallback> callback_;
  bool found_ = false;
  bool timed_out_ = false;
  std::string bytes_;
  std::size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(FileHandler);
};

// kcds://kcds/<path>: the game client's own pages from the ui folder next to the DLL.
class InternalSchemeFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;
    CefURLParts parts;
    std::string path;
    if (CefParseURL(request->GetURL(), parts)) {
      path = CefURIDecode(CefString(&parts.path), true, UU_SPACES).ToString();
    }
    if (path.size() < 2 || path.find("..") != std::string::npos) {
      return not_found();
    }
    std::wstring file = settings.runtime_folder + L"ui";
    for (char character : path) {
      file +=
          character == '/' ? L'\\' : static_cast<wchar_t>(static_cast<unsigned char>(character));
    }
    CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForFile(file);
    if (!stream) {
      return not_found();
    }
    std::string extension;
    std::size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
      extension = path.substr(dot + 1);
    }
    std::string mime = CefGetMimeType(extension).ToString();
    return new CefStreamResourceHandler(mime.empty() ? "application/octet-stream" : mime, stream);
  }

 private:
  static CefRefPtr<CefResourceHandler> not_found() {
    CefRefPtr<CefStreamReader> empty = CefStreamReader::CreateForData(nullptr, 0);
    return new CefStreamResourceHandler(404, "Not Found", "text/plain", CefResponse::HeaderMap(),
                                        empty);
  }

  IMPLEMENT_REFCOUNTING(InternalSchemeFactory);
};

class ResourceSchemeFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;
    std::string resource;
    std::string file;
    CefURLParts parts;
    if (CefParseURL(request->GetURL(), parts)) {
      // path is "/<resource>/<file path>"
      std::string path = CefURIDecode(CefString(&parts.path), true, UU_SPACES).ToString();
      std::size_t slash = path.size() > 1 && path[0] == '/' ? path.find('/', 1) : std::string::npos;
      if (slash != std::string::npos && slash + 1 < path.size()) {
        resource = path.substr(1, slash - 1);
        file = path.substr(slash + 1);
      }
    }
    return new FileHandler(resource, file);
  }

  IMPLEMENT_REFCOUNTING(ResourceSchemeFactory);
};

// ---- GPU frames (CEF UI thread) ----------------------------------------------------------------

ComPtr<ID3D11Device1> cef_device;
ComPtr<ID3D11DeviceContext> cef_context;

// The view's inter-thread texture on the CEF side.
struct CefTexture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<IDXGIKeyedMutex> mutex;
  HANDLE handle = nullptr;
  int width = 0;
  int height = 0;
};

bool ensure_cef_device() {
  if (cef_device) {
    return true;
  }
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIAdapter1> chosen;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && game_adapter_known.load()) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
      DXGI_ADAPTER_DESC1 description;
      adapter->GetDesc1(&description);
      if (description.AdapterLuid.LowPart == game_adapter.LowPart &&
          description.AdapterLuid.HighPart == game_adapter.HighPart) {
        chosen = adapter;
        break;
      }
    }
  }
  ComPtr<ID3D11Device> device;
  D3D_FEATURE_LEVEL level;
  HRESULT result =
      D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                        &device, &level, &cef_context);
  if (FAILED(result) || FAILED(device.As(&cef_device))) {
    note("WEB no D3D11 device for view frames (0x%08lx)", static_cast<unsigned long>(result));
    return false;
  }
  note("WEB view frames on %s adapter", chosen ? "the game's" : "the default");
  return true;
}

bool make_texture(CefTexture& target, int width, int height) {
  target = CefTexture{};
  D3D11_TEXTURE2D_DESC description{};
  description.Width = static_cast<UINT>(width);
  description.Height = static_cast<UINT>(height);
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  description.MiscFlags =
      D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (FAILED(cef_device->CreateTexture2D(&description, nullptr, &target.texture))) {
    return false;
  }
  target.texture.As(&target.mutex);
  ComPtr<IDXGIResource1> resource;
  target.texture.As(&resource);
  if (!resource || FAILED(resource->CreateSharedHandle(
                       nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                       &target.handle))) {
    return false;
  }
  target.width = width;
  target.height = height;
  return true;
}

// A device on the adapter whose texture a shared handle is, with the texture opened; nullptr when
// no adapter opens it.
ComPtr<ID3D11Device1> device_opening(HANDLE handle, ComPtr<ID3D11Texture2D>& opened) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    HRESULT result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(result) || FAILED(device.As(&device1))) {
      continue;
    }
    if (SUCCEEDED(device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened)))) {
      return device1;
    }
  }
  return nullptr;
}

// ---- one view ---------------------------------------------------------------------------------

const char* cursor_name(cef_cursor_type_t type) {
  struct CssCursor {
    cef_cursor_type_t type;
    const char* name;
  };
  // Grouped by what the cursor means; a shape CSS has no name for reports as "custom".
  static constexpr CssCursor kCursors[] = {
      {CT_POINTER, "default"},
      {CT_HAND, "pointer"},
      {CT_IBEAM, "text"},
      {CT_VERTICALTEXT, "vertical-text"},
      {CT_CROSS, "crosshair"},
      {CT_CELL, "cell"},
      {CT_MOVE, "move"},
      {CT_GRAB, "grab"},
      {CT_GRABBING, "grabbing"},
      {CT_WAIT, "wait"},
      {CT_PROGRESS, "progress"},
      {CT_HELP, "help"},
      {CT_CONTEXTMENU, "context-menu"},
      {CT_ALIAS, "alias"},
      {CT_COPY, "copy"},
      {CT_NODROP, "no-drop"},
      {CT_NOTALLOWED, "not-allowed"},
      {CT_NONE, "none"},
      {CT_ZOOMIN, "zoom-in"},
      {CT_ZOOMOUT, "zoom-out"},
      {CT_NORTHRESIZE, "n-resize"},
      {CT_EASTRESIZE, "e-resize"},
      {CT_SOUTHRESIZE, "s-resize"},
      {CT_WESTRESIZE, "w-resize"},
      {CT_NORTHEASTRESIZE, "ne-resize"},
      {CT_NORTHWESTRESIZE, "nw-resize"},
      {CT_SOUTHEASTRESIZE, "se-resize"},
      {CT_SOUTHWESTRESIZE, "sw-resize"},
      {CT_NORTHSOUTHRESIZE, "ns-resize"},
      {CT_EASTWESTRESIZE, "ew-resize"},
      {CT_COLUMNRESIZE, "col-resize"},
      {CT_ROWRESIZE, "row-resize"},
  };
  for (const CssCursor& cursor : kCursors) {
    if (cursor.type == type) {
      return cursor.name;
    }
  }
  return "custom";
}

const char* severity_name(cef_log_severity_t level) {
  switch (level) {
    case LOGSEVERITY_VERBOSE:
      return "debug";
    case LOGSEVERITY_WARNING:
      return "warning";
    case LOGSEVERITY_ERROR:
      return "error";
    case LOGSEVERITY_FATAL:
      return "fatal";
    default:
      return "info";
  }
}

// Runs work on the CEF UI thread after delay_ms (defined with the UI thread tasks below).
void post_to_cef_later(std::function<void()> work, std::int64_t delay_ms);

// How long a view frame waits before it is offered to the game's frame again when the game held
// that frame.
constexpr std::int64_t kFrameRetryMs = 4;

// How long a click or key took to show in a view's frame (the view's next paint after it): the
// player's log hears the average and the longest every kInputReportMs while pages are used.
constexpr ULONGLONG kInputReportMs = 60000;
std::atomic<ULONGLONG> input_waiting_since{0};
std::mutex input_stats_mutex;
struct InputStats {
  ULONGLONG since = 0;
  int count = 0;
  ULONGLONG total = 0;
  ULONGLONG most = 0;
} input_stats;

// A click or key went to a view (the window's thread): the next paint shows it.
void input_sent() {
  ULONGLONG none = 0;
  input_waiting_since.compare_exchange_strong(none, GetTickCount64());
}

// A view painted (CEF UI thread): the input waiting since then is shown.
void input_painted(int id) {
  const ULONGLONG since = input_waiting_since.exchange(0);
  if (since == 0) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> lock(input_stats_mutex);
  InputStats& stats = input_stats;
  if (stats.since == 0) {
    stats.since = now;
  }
  stats.count += 1;
  stats.total += now - since;
  stats.most = std::max(stats.most, now - since);
  if (now - stats.since < kInputReportMs) {
    return;
  }
  const ULONGLONG average = stats.total / static_cast<ULONGLONG>(stats.count);
  note("WEB view %d: %d click(s) and key(s) in the page's frame after %llu ms, %llu ms at most", id,
       stats.count, average, stats.most);
  stats = InputStats{};
}

// The rate a view paints at: the main display's refresh rate (a 144 Hz display shows a key typed
// into a page up to a frame sooner than at 60), from 60 to 240 frames a second.
int view_frame_rate() {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  int rate = 60;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
    rate = static_cast<int>(mode.dmDisplayFrequency);
  }
  return std::clamp(rate, 60, 240);
}

class ViewClient : public CefClient,
                   public CefRenderHandler,
                   public CefLifeSpanHandler,
                   public CefLoadHandler,
                   public CefDisplayHandler,
                   public CefRequestHandler {
 public:
  explicit ViewClient(int id) : id_(id) {
  }

  CefRefPtr<CefRenderHandler> GetRenderHandler() override {
    return this;
  }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override {
    return this;
  }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override {
    return this;
  }

  // -- life span
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    std::shared_ptr<View> view;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      view = find_view(id_);
      if (view) {
        view->browser = browser;
        queue_event(*view, "webViewOpened", JsonObject().field("url", view->url));
        queue_event(*view, "webViewOrigin",
                    JsonObject().field("origin", view->origin).field("url", view->url));
      }
    }
    if (!view) {
      browser->GetHost()->CloseBrowser(true);  // destroyed before CEF finished creating it
      return;
    }
    apply_visibility(*view);
    // A focus asked for while CEF was still creating the browser (focus right after
    // open) is given now.
    if (view->options.focus) {
      browser->GetHost()->SetFocus(true);
    }
  }

  ~ViewClient() override {
    if (texture_.handle) {
      CloseHandle(texture_.handle);
    }
    if (retired_handle_) {
      CloseHandle(retired_handle_);
    }
  }

  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                     const CefString& target_url, const CefString& target_frame_name,
                     CefLifeSpanHandler::WindowOpenDisposition target_disposition,
                     bool user_gesture, const CefPopupFeatures& features,
                     CefWindowInfo& window_info, CefRefPtr<CefClient>& client,
                     CefBrowserSettings& browser_settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override {
    (void)browser;
    (void)popup_id;
    (void)target_frame_name;
    (void)target_disposition;
    (void)user_gesture;
    (void)features;
    (void)window_info;
    (void)client;
    (void)browser_settings;
    (void)extra_info;
    (void)no_javascript_access;
    queue_event(id_, "webViewPopup",
                JsonObject()
                    .field("url", target_url.ToString())
                    .field("openerUrl", frame ? frame->GetURL().ToString() : std::string()));
    return true;  // windowless views host no popups
  }

  // -- navigation lock
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool user_gesture, bool is_redirect) override {
    (void)browser;
    (void)user_gesture;
    (void)is_redirect;
    std::string url = request->GetURL().ToString();
    std::string origin = origin_of(url);
    bool main = frame->IsMain();
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id_);
    if (!view) {
      return true;
    }
    bool blocked = main && origin != view->origin;
    queue_event(*view, "webViewNavigated",
                JsonObject().field("url", url).flag("isMainFrame", main).flag("blocked", blocked));
    if (blocked) {
      queue_event(*view, "webViewBlocked",
                  JsonObject()
                      .field("url", url)
                      .field("domain", host_of(url))
                      .field("reason", origin == "null" ? "invalid-url" : "cross-origin"));
    }
    return blocked;
  }

  // -- loading
  void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   TransitionType transition_type) override {
    (void)browser;
    (void)transition_type;
    if (frame->IsMain()) {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (auto view = find_view(id_)) {
        view->document_ready = false;
        view->load_started = std::chrono::steady_clock::now();
        view->load_overdue = false;
      }
    }
    queue_event(
        id_, "webViewLoading",
        JsonObject().field("url", frame->GetURL().ToString()).flag("isMainFrame", frame->IsMain()));
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                 int http_status_code) override {
    (void)browser;
    std::string url = frame->GetURL().ToString();
    if (http_status_code >= 400) {
      // A file the resource does not ship is a real 404: report it like a failed load.
      queue_event(id_, "webViewFailed",
                  JsonObject()
                      .field("url", url)
                      .field("description", "ERR_HTTP_RESPONSE_CODE_FAILURE")
                      .field("errorCode", -379)
                      .flag("isMainFrame", frame->IsMain()));
      return;
    }
    if (frame->IsMain()) {
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (auto view = find_view(id_)) {
          view->document_ready = true;
        }
      }
      queue_event(id_, "webViewReady", JsonObject().field("url", url));
    }
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code,
                   const CefString& error_text, const CefString& failed_url) override {
    (void)browser;
    if (error_code == ERR_ABORTED) {
      return;  // a refused or replaced navigation, already reported by webViewNavigated
    }
    queue_event(id_, "webViewFailed",
                JsonObject()
                    .field("url", failed_url.ToString())
                    .field("description", error_text.ToString())
                    .field("errorCode", static_cast<int>(error_code))
                    .flag("isMainFrame", frame->IsMain()));
  }

  // -- display
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                        const CefString& message, const CefString& source, int line) override {
    (void)browser;
    std::string text = message.ToString();
    note("WEB view %d console: %s (%s:%d)", id_, text.c_str(), source.ToString().c_str(), line);
    queue_event(id_, "webViewConsole",
                JsonObject()
                    .field("message", text)
                    .field("source", source.ToString())
                    .field("line", line)
                    .field("severity", severity_name(level)));
    return true;
  }

  bool OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor, cef_cursor_type_t type,
                      const CefCursorInfo& custom_cursor_info) override {
    (void)browser;
    (void)custom_cursor_info;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto view = find_view(id_);
      if (view) {
        view->cursor = cursor;
      }
    }
    queue_event(id_, "webViewCursor",
                JsonObject().field("cursor", cursor_name(type)).field("cursorType", type));
    return true;
  }

  bool OnTooltip(CefRefPtr<CefBrowser> browser, CefString& text) override {
    (void)browser;
    queue_event(id_, "webViewTooltip", JsonObject().field("text", text.ToString()));
    return true;
  }

  // -- page bridge
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    (void)browser;
    (void)source;
    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> values = message->GetArgumentList();
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id_);
    if (!view) {
      return true;
    }
    if (name == protocol::kInputFocus) {
      queue_event(*view, "webViewFocus", JsonObject().flag("focused", values->GetBool(0)));
      return true;
    }
    if (name != protocol::kCallEvent) {
      return false;
    }
    std::string frame_url = values->GetString(2).ToString();
    if (origin_of(frame_url) != view->origin) {
      queue_event(*view, "webViewBlocked",
                  JsonObject()
                      .field("url", frame_url)
                      .field("domain", host_of(frame_url))
                      .field("reason", "foreign-event"));
      return true;
    }
    (void)frame;
    if (internal_owner(view->owner)) {
      std::string payload;
      if (values->GetType(1) == VTYPE_STRING) {
        payload = values->GetString(1).ToString();
      }
      internal_messages[view->owner].push_back(
          {std::string(), values->GetString(0).ToString(), payload});
      return true;
    }
    JsonObject call;
    call.field("type", "web-call").field("owner", view->owner).field("viewId", view->id);
    call.field("name", values->GetString(0).ToString());
    if (values->GetType(1) == VTYPE_STRING) {
      call.field("payload", values->GetString(1).ToString());
    } else {
      call.raw("payload", "null");
    }
    messages.push_back(call.str());
    return true;
  }

  // -- rendering
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
    (void)browser;
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id_);
    int width = 0;
    int height = 0;
    if (view) {
      width = view->options.width;
      height = view->options.height;
      if (width <= 0 || height <= 0) {
        width = screen_width;
        height = screen_height;
      }
    }
    rect.Set(0, 0, std::max(width, 1), std::max(height, 1));
  }

  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty_rects,
               const void* buffer, int width, int height) override {
    (void)browser;
    (void)type;
    (void)dirty_rects;
    (void)buffer;
    (void)width;
    (void)height;
  }

  void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                          const RectList& dirty_rects,
                          const CefAcceleratedPaintInfo& info) override {
    (void)browser;
    (void)dirty_rects;
    if (!info.shared_texture_handle || !ensure_cef_device()) {
      return;
    }
    if (type == PET_POPUP) {
      keep_popup(info.shared_texture_handle);
      return;
    }
    if (type != PET_VIEW) {
      return;
    }
    input_painted(id_);
    ComPtr<ID3D11Texture2D> source;  // CEF's handle is valid only during this callback
    const bool opened = SUCCEEDED(
        cef_device->OpenSharedResource1(info.shared_texture_handle, IID_PPV_ARGS(&source)));
    if (opened && !present_cannot_share.load()) {
      share_frame(source);
      return;
    }
    if (!memory_frames_) {
      memory_frames_ = true;
      note(opened ? "WEB view %d: the game's device cannot open the frames; they go through memory"
                  : "WEB view %d: the frames are on another adapter; they go through memory",
           id_);
    }
    read_back_frame(info.shared_texture_handle, source);
  }

  // -- popups: a <select>'s dropdown and other widgets Chromium paints apart from the view
  // Their frame is kept and copied into the view's frame at their rectangle.
  void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override {
    popup_visible_ = show;
    if (!show) {
      popup_.Reset();
      // The next view frame no longer carries the popup.
      browser->GetHost()->Invalidate(PET_VIEW);
    }
  }

  void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override {
    (void)browser;
    popup_rect_ = rect;
  }

  // Shows, hides or keeps painting the view (CEF UI thread, state_mutex not held).
  static void apply_visibility(const View& view) {
    if (view.browser) {
      bool painting = view.options.visible || view.offscreen;
      view.browser->GetHost()->WasHidden(!painting);
    }
  }

 private:
  // The frame on the GPU: copied into the inter-thread texture the game's device opens.
  void share_frame(const ComPtr<ID3D11Texture2D>& source) {
    D3D11_TEXTURE2D_DESC description;
    source->GetDesc(&description);
    int width = static_cast<int>(description.Width);
    int height = static_cast<int>(description.Height);
    if (width != texture_.width || height != texture_.height) {
      HANDLE previous = texture_.handle;
      if (!make_texture(texture_, width, height)) {
        note("WEB view %d: no %dx%d frame texture", id_, width, height);
        return;
      }
      std::lock_guard<std::mutex> lock(state_mutex);
      auto view = find_view(id_);
      if (view) {
        view->frame.shared_handle = texture_.handle;
        view->frame.width = width;
        view->frame.height = height;
        view->frame.generation++;
      }
      // The present thread may still hold the previous texture for a frame; it is closed when the
      // one after it is replaced.
      if (retired_handle_) {
        CloseHandle(retired_handle_);
      }
      retired_handle_ = previous;
    }
    if (!keep_latest(source)) {
      return;
    }
    publish_latest();
  }

  // The view's last frame, kept on the CEF device: CEF's texture is valid only during the paint
  // callback, and a frame the game held at that moment was lost until the page painted again (a
  // key typed into a page showed only at the next paint, Alt+Tab).
  bool keep_latest(const ComPtr<ID3D11Texture2D>& source) {
    D3D11_TEXTURE2D_DESC description;
    source->GetDesc(&description);
    D3D11_TEXTURE2D_DESC kept{};
    if (latest_) {
      latest_->GetDesc(&kept);
    }
    if (!latest_ || kept.Width != description.Width || kept.Height != description.Height ||
        kept.Format != description.Format) {
      description.MiscFlags = 0;
      description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      description.CPUAccessFlags = 0;
      description.Usage = D3D11_USAGE_DEFAULT;
      latest_.Reset();
      if (FAILED(cef_device->CreateTexture2D(&description, nullptr, &latest_))) {
        note("WEB view %d: no %ux%u frame copy", id_, description.Width, description.Height);
        return false;
      }
    }
    cef_context->CopyResource(latest_.Get(), source.Get());
    latest_waiting_ = true;
    return true;
  }

  // Copies the kept frame (with the popup) into the frame the game reads (CEF UI thread). When the
  // game holds that frame, it is offered again a moment later, so the last frame always arrives.
  void publish_latest() {
    if (!latest_waiting_ || !latest_ || !texture_.texture) {
      return;
    }
    if (texture_.mutex->AcquireSync(0, 8) != S_OK) {
      retry_publish();
      return;
    }
    cef_context->CopyResource(texture_.texture.Get(), latest_.Get());
    paste_popup();
    cef_context->Flush();
    texture_.mutex->ReleaseSync(0);
    latest_waiting_ = false;
  }

  void retry_publish() {
    if (retry_pending_) {
      return;
    }
    retry_pending_ = true;
    CefRefPtr<ViewClient> self(this);
    post_to_cef_later(
        [self]() {
          self->retry_pending_ = false;
          self->publish_latest();
        },
        kFrameRetryMs);
  }

  // A popup's frame (CEF UI thread): kept for the view's later frames and pasted into the shared
  // frame now.
  void keep_popup(HANDLE handle) {
    ComPtr<ID3D11Texture2D> source;  // CEF's handle is valid only during the paint callback
    if (!popup_visible_ || !texture_.texture ||
        FAILED(cef_device->OpenSharedResource1(handle, IID_PPV_ARGS(&source)))) {
      return;
    }
    D3D11_TEXTURE2D_DESC description;
    source->GetDesc(&description);
    D3D11_TEXTURE2D_DESC kept{};
    if (popup_) {
      popup_->GetDesc(&kept);
    }
    if (!popup_ || kept.Width != description.Width || kept.Height != description.Height) {
      description.MiscFlags = 0;
      description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      description.CPUAccessFlags = 0;
      description.Usage = D3D11_USAGE_DEFAULT;
      popup_.Reset();
      if (FAILED(cef_device->CreateTexture2D(&description, nullptr, &popup_))) {
        return;
      }
    }
    cef_context->CopyResource(popup_.Get(), source.Get());
    // The view's kept frame goes out again with the popup on it.
    latest_waiting_ = latest_ != nullptr;
    publish_latest();
  }

  // Copies the kept popup into the shared frame, clipped to it (the frame's mutex is held).
  void paste_popup() {
    if (!popup_visible_ || !popup_) {
      return;
    }
    D3D11_TEXTURE2D_DESC popup;
    popup_->GetDesc(&popup);
    const int left = std::max(popup_rect_.x, 0);
    const int top = std::max(popup_rect_.y, 0);
    const int right = std::min(popup_rect_.x + static_cast<int>(popup.Width), texture_.width);
    const int bottom = std::min(popup_rect_.y + static_cast<int>(popup.Height), texture_.height);
    if (right <= left || bottom <= top) {
      return;
    }
    D3D11_BOX box{};
    box.left = static_cast<UINT>(left - popup_rect_.x);
    box.top = static_cast<UINT>(top - popup_rect_.y);
    box.right = static_cast<UINT>(right - popup_rect_.x);
    box.bottom = static_cast<UINT>(bottom - popup_rect_.y);
    box.front = 0;
    box.back = 1;
    cef_context->CopySubresourceRegion(texture_.texture.Get(), 0, static_cast<UINT>(left),
                                       static_cast<UINT>(top), 0, popup_.Get(), 0, &box);
  }

  // The frame through memory: read back on a device that opens it (the CEF device, or one on the
  // adapter Chromium drew it on) and published as pixels the game's device uploads.
  void read_back_frame(HANDLE handle, ComPtr<ID3D11Texture2D> source) {
    if (!source && (!readback_device_ ||
                    FAILED(readback_device_->OpenSharedResource1(handle, IID_PPV_ARGS(&source))))) {
      readback_device_ = device_opening(handle, source);
      if (!readback_device_) {
        if (!reported_open_failure_) {
          reported_open_failure_ = true;
          note("WEB view %d: no adapter opens the frames", id_);
        }
        return;
      }
    }
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC description;
    source->GetDesc(&description);
    if (!make_staging(device, description)) {
      return;
    }
    context->CopyResource(staging_.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    const int width = static_cast<int>(description.Width);
    const int height = static_cast<int>(description.Height);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(row_bytes * height);
    for (int row = 0; row < height; ++row) {
      std::memcpy(pixels->data() + row_bytes * row,
                  static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch * row,
                  row_bytes);
    }
    context->Unmap(staging_.Get(), 0);
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id_);
    if (view) {
      view->frame.shared_handle = nullptr;
      view->frame.pixels = std::move(pixels);
      view->frame.width = width;
      view->frame.height = height;
      view->frame.generation++;
    }
  }

  // The CPU-readable copy of the frame on the frame's device.
  bool make_staging(const ComPtr<ID3D11Device>& device, const D3D11_TEXTURE2D_DESC& frame) {
    if (staging_ && staging_device_ == device && staging_width_ == frame.Width &&
        staging_height_ == frame.Height) {
      return true;
    }
    staging_.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = frame.Width;
    description.Height = frame.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = frame.Format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging_))) {
      note("WEB view %d: no %ux%u readback texture", id_, frame.Width, frame.Height);
      return false;
    }
    staging_device_ = device;
    staging_width_ = frame.Width;
    staging_height_ = frame.Height;
    return true;
  }

  int id_;
  CefTexture texture_;
  // The view's last frame (CEF UI thread), whether the game's frame still lacks it, and whether a
  // retry is queued.
  ComPtr<ID3D11Texture2D> latest_;
  bool latest_waiting_ = false;
  bool retry_pending_ = false;
  HANDLE retired_handle_ = nullptr;
  // The open popup (CEF UI thread): whether it shows, where and its last frame.
  bool popup_visible_ = false;
  CefRect popup_rect_;
  ComPtr<ID3D11Texture2D> popup_;
  bool reported_open_failure_ = false;
  // Frames through memory (see ViewFrame): the device on Chromium's adapter, when it is not the
  // CEF device, and the readback texture.
  bool memory_frames_ = false;
  ComPtr<ID3D11Device1> readback_device_;
  ComPtr<ID3D11Device> staging_device_;
  ComPtr<ID3D11Texture2D> staging_;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;

  IMPLEMENT_REFCOUNTING(ViewClient);
};

// ---- application ------------------------------------------------------------------------------

class WebApp : public CefApp, public CefBrowserProcessHandler {
 public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    protocol::register_schemes(registrar);
  }

  // The views of one site share a renderer process: every resource page is on
  // kcds://resources/, and Chromium otherwise starts a renderer for each view, so a server that
  // keeps many pages open ran dozens of them and slowed the game's input.
  //
  // Test hook (KCDS_TEST_HOOKS only): Chromium switches from KCDS_TEST_WEB_SWITCHES, "name" or
  // "name=value" separated by spaces, such as use-angle=d3d11-warp to draw the views on another
  // adapter than the game's.
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty()) {
      return;
    }
    command_line->AppendSwitch("process-per-site");
    if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) == 0) {
      return;
    }
    char value[512] = {};
    DWORD length = GetEnvironmentVariableA("KCDS_TEST_WEB_SWITCHES", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
      return;
    }
    std::string switches(value, length);
    std::size_t start = 0;
    while (start < switches.size()) {
      std::size_t end = switches.find(' ', start);
      if (end == std::string::npos) {
        end = switches.size();
      }
      std::string entry = switches.substr(start, end - start);
      std::size_t equals = entry.find('=');
      if (equals == std::string::npos) {
        command_line->AppendSwitch(entry);
      } else {
        command_line->AppendSwitchWithValue(entry.substr(0, equals), entry.substr(equals + 1));
      }
      start = end + 1;
    }
    note("WEB test switches %s", switches.c_str());
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnContextInitialized() override {
    CefRegisterSchemeHandlerFactory(protocol::kScheme, protocol::kResourcesHost,
                                    new ResourceSchemeFactory());
    CefRegisterSchemeHandlerFactory(protocol::kScheme, protocol::kInternalHost,
                                    new InternalSchemeFactory());
    is_ready.store(true);
    note("WEB CEF ready");
  }

  IMPLEMENT_REFCOUNTING(WebApp);
};

DWORD WINAPI cef_thread(void*) {
  SetDllDirectoryW(settings.runtime_folder.c_str());
  std::wstring library = settings.runtime_folder + L"libcef.dll";
  if (!LoadLibraryW(library.c_str())) {
    note("WEB libcef.dll not loaded (error %lu); no web views", GetLastError());
    has_failed.store(true);
    return 1;
  }
  CefMainArgs arguments(GetModuleHandleW(nullptr));
  CefSettings cef_settings;
  cef_settings.no_sandbox = true;
  cef_settings.windowless_rendering_enabled = true;
  cef_settings.multi_threaded_message_loop = true;
  cef_settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  cef_settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&cef_settings.browser_subprocess_path) =
      settings.runtime_folder + L"kcds-web-helper.exe";
  CefString(&cef_settings.resources_dir_path) = settings.runtime_folder;
  CefString(&cef_settings.locales_dir_path) = settings.runtime_folder + L"locales";
  CefString(&cef_settings.locale) = "en-US";  // the only locale the client ships
  CefString(&cef_settings.root_cache_path) = settings.cache_folder;
  CefString(&cef_settings.cache_path) = settings.cache_folder + L"\\profile";
  CefString(&cef_settings.log_file) = settings.cache_folder + L"\\cef.log";
  if (!CefInitialize(arguments, cef_settings, new WebApp(), nullptr)) {
    note("WEB CefInitialize failed; no web views");
    has_failed.store(true);
    return 1;
  }
  note("WEB CEF initialized");
  return 0;  // the multi-threaded message loop keeps CEF running on its own UI thread
}

// ---- CEF UI thread tasks ----------------------------------------------------------------------

class FunctionTask : public CefTask {
 public:
  explicit FunctionTask(std::function<void()> work) : work_(std::move(work)) {
  }
  void Execute() override {
    work_();
  }

 private:
  std::function<void()> work_;

  IMPLEMENT_REFCOUNTING(FunctionTask);
};

void post_to_cef(std::function<void()> work) {
  CefPostTask(TID_UI, new FunctionTask(std::move(work)));
}

void post_to_cef_later(std::function<void()> work, std::int64_t delay_ms) {
  CefPostDelayedTask(TID_UI, new FunctionTask(std::move(work)), delay_ms);
}

void time_out_file_later(int request_id) {
  CefPostDelayedTask(
      TID_UI, new FunctionTask([request_id]() {
        CefRefPtr<FileHandler> handler;
        {
          std::lock_guard<std::mutex> lock(file_mutex);
          auto waiting = waiting_files.find(request_id);
          if (waiting == waiting_files.end()) {
            return;  // answered or cancelled
          }
          handler = waiting->second;
          waiting_files.erase(waiting);
        }
        note("WEB kcds://resources/%s/%s: no answer from the resource runtime in %d ms",
             handler->resource().c_str(), handler->path().c_str(), kFileAnswerMs);
        handler->time_out();
      }),
      kFileAnswerMs);
}

void create_browser_task(int id, std::string url) {
  CefWindowInfo window_info;
  window_info.SetAsWindowless(nullptr);
  window_info.shared_texture_enabled = true;
  CefBrowserSettings browser_settings;
  browser_settings.windowless_frame_rate = view_frame_rate();
  note("WEB view %d paints up to %d frames a second", id, browser_settings.windowless_frame_rate);
  browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  CefBrowserHost::CreateBrowser(window_info, new ViewClient(id), url, browser_settings, nullptr,
                                nullptr);
}

// Runs action on the CEF UI thread for a view whose browser exists. CEF may call back into the
// handlers (which take state_mutex), so the action runs without the lock.
void run_on_view(int view_id, std::function<void(View&)> action) {
  std::shared_ptr<View> view;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    view = find_view(view_id);
  }
  if (view && view->browser) {
    action(*view);
  }
}

void with_browser(int id, std::function<void(View&)> action) {
  post_to_cef([id, action]() { run_on_view(id, action); });
}

void close_browser_task(CefRefPtr<CefBrowser> browser) {
  browser->GetHost()->CloseBrowser(true);
}

// ---- present thread ---------------------------------------------------------------------------

struct PresentView {
  unsigned generation = 0;
  HANDLE handle = nullptr;
  ComPtr<ID3D11Texture2D> shared;
  ComPtr<IDXGIKeyedMutex> mutex;
  ComPtr<ID3D11Texture2D> copy;
  ComPtr<ID2D1Bitmap1> bitmap;
};

ID3D11Device* present_device_key = nullptr;
ComPtr<ID3D11Device1> present_device;
ComPtr<ID3D11DeviceContext> present_context;
std::map<int, PresentView> present_views;

bool ensure_present_device(ID2D1DeviceContext* canvas) {
  ComPtr<ID2D1Image> target;
  canvas->GetTarget(&target);
  ComPtr<ID2D1Bitmap1> bitmap;
  if (!target || FAILED(target.As(&bitmap))) {
    return false;
  }
  ComPtr<IDXGISurface> surface;
  if (FAILED(bitmap->GetSurface(&surface)) || !surface) {
    return false;
  }
  ComPtr<ID3D11Device> device;
  if (FAILED(surface->GetDevice(IID_PPV_ARGS(&device)))) {
    return false;
  }
  if (device.Get() == present_device_key) {
    return true;
  }
  present_views.clear();
  present_device_key = device.Get();
  device.As(&present_device);
  device->GetImmediateContext(&present_context);
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC description;
    adapter->GetDesc(&description);
    game_adapter = description.AdapterLuid;
    game_adapter_known.store(true);
  }
  return present_device != nullptr;
}

// A view frame that came through memory, on a bitmap of the game's canvas.
bool show_memory_frame(ID2D1DeviceContext* canvas, PresentView& target, const ViewFrame& frame) {
  if (target.bitmap && !target.shared && target.generation == frame.generation) {
    return true;
  }
  const D2D1_SIZE_U size =
      D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height));
  const UINT32 pitch = static_cast<UINT32>(frame.width) * 4;
  const bool same_bitmap = target.bitmap && !target.shared &&
                           target.bitmap->GetPixelSize().width == size.width &&
                           target.bitmap->GetPixelSize().height == size.height;
  if (same_bitmap) {
    if (FAILED(target.bitmap->CopyFromMemory(nullptr, frame.pixels->data(), pitch))) {
      return false;
    }
  } else {
    target = PresentView{};
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(
            canvas->CreateBitmap(size, frame.pixels->data(), pitch, &properties, &target.bitmap))) {
      return false;
    }
  }
  target.generation = frame.generation;
  target.handle = nullptr;
  return true;
}

// The views asked to paint again after frames went to memory (present thread).
std::set<int> repaint_asked;

// The game's device cannot open a view's texture: from now on every view sends its frames
// through memory, and this one paints again, so a page that does not change still arrives.
void stop_sharing(int view_id) {
  if (!present_cannot_share.exchange(true)) {
    note(
        "WEB the game's device cannot open view frames (another adapter?); they go through "
        "memory");
  }
  if (repaint_asked.insert(view_id).second) {
    with_browser(view_id, [](View& view) { view.browser->GetHost()->Invalidate(PET_VIEW); });
  }
}

bool open_present_view(ID2D1DeviceContext* canvas, PresentView& target, const ViewFrame& frame) {
  target = PresentView{};
  if (FAILED(
          present_device->OpenSharedResource1(frame.shared_handle, IID_PPV_ARGS(&target.shared)))) {
    return false;
  }
  target.shared.As(&target.mutex);
  D3D11_TEXTURE2D_DESC description;
  target.shared->GetDesc(&description);
  description.MiscFlags = 0;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(present_device->CreateTexture2D(&description, nullptr, &target.copy))) {
    return false;
  }
  ComPtr<IDXGISurface> surface;
  target.copy.As(&surface);
  D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (!surface ||
      FAILED(canvas->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target.bitmap))) {
    return false;
  }
  target.generation = frame.generation;
  target.handle = frame.shared_handle;
  return true;
}

}  // namespace

// ---- public API -----------------------------------------------------------------------------

namespace {
void page_key(const page_keyboard::Key& key);
void page_mouse(const page_keyboard::Mouse& mouse);
bool page_wants_keys();
}  // namespace

bool start(const Options& options) {
  if (started.exchange(true)) {
    return true;
  }
  page_keyboard::start(page_key, page_mouse, page_wants_keys);
  settings = options;
  if (!settings.runtime_folder.empty() && settings.runtime_folder.back() != L'\\') {
    settings.runtime_folder += L'\\';
  }
  CreateDirectoryW(settings.cache_folder.c_str(), nullptr);
  HANDLE thread = CreateThread(nullptr, 0, cef_thread, nullptr, 0, nullptr);
  if (!thread) {
    has_failed.store(true);
    return false;
  }
  CloseHandle(thread);
  return true;
}

bool ready() {
  return is_ready.load() && !has_failed.load();
}

bool failed() {
  return has_failed.load();
}

void shutdown() {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (const auto& entry : views) {
      ids.push_back(entry.first);
    }
  }
  for (int id : ids) {
    destroy_view(id);
  }
}

bool create_view(int id, const std::string& owner, const std::string& url,
                 const ViewOptions& options) {
  if (!ready() || id <= 0 || !allowed_url(url)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (views.count(id) != 0) {
      return false;
    }
    auto view = std::make_shared<View>();
    view->id = id;
    view->owner = owner;
    view->url = url;
    view->origin = origin_of(url);
    view->options = options;
    views[id] = view;
    if (options.focus && options.visible) {
      input_view = id;
    }
  }
  post_to_cef([id, url]() { create_browser_task(id, url); });
  return true;
}

bool destroy_view(int id) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    browser = view->browser;
    views.erase(id);
    if (input_view == id) {
      input_view = 0;
    }
  }
  if (browser) {
    post_to_cef([browser]() { close_browser_task(browser); });
  }
  return true;
}

bool set_visible(int id, bool visible) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    view->options.visible = visible;
  }
  with_browser(id, [](View& view) { ViewClient::apply_visibility(view); });
  return true;
}

bool set_focus(int id, bool focused) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    view->options.focus = focused;
    if (focused) {
      input_view = id;
    } else if (input_view == id) {
      input_view = 0;
    }
  }
  with_browser(id, [focused](View& view) { view.browser->GetHost()->SetFocus(focused); });
  return true;
}

bool set_offscreen(int id, bool offscreen) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    view->offscreen = offscreen;
  }
  with_browser(id, [](View& view) { ViewClient::apply_visibility(view); });
  return true;
}

bool load_url(int id, const std::string& url) {
  if (!allowed_url(url)) {
    queue_event(id, "webViewBlocked",
                JsonObject()
                    .field("url", url)
                    .field("domain", host_of(url))
                    .field("reason", "invalid-url"));
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    std::string origin = origin_of(url);
    view->url = url;
    if (origin != view->origin) {
      view->origin = origin;
      queue_event(*view, "webViewOrigin", JsonObject().field("origin", origin).field("url", url));
    }
  }
  with_browser(id, [url](View& view) { view.browser->GetMainFrame()->LoadURL(url); });
  return true;
}

bool resize_view(int id, int width, int height) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    view->options.width = width;
    view->options.height = height;
  }
  with_browser(id, [](View& view) { view.browser->GetHost()->WasResized(); });
  return true;
}

bool move_view(int id, int x, int y) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto view = find_view(id);
  if (!view) {
    return false;
  }
  view->options.x = x;
  view->options.y = y;
  return true;
}

bool anchor_view(int id, bool anchored, float x, float y, float z, int offset_x, int offset_y) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto view = find_view(id);
  if (!view) {
    return false;
  }
  view->anchor.active = anchored;
  view->anchor.x = x;
  view->anchor.y = y;
  view->anchor.z = z;
  view->anchor.offset_x = offset_x;
  view->anchor.offset_y = offset_y;
  return true;
}

void set_view_camera(bool valid, const float* matrix, float fov) {
  std::lock_guard<std::mutex> lock(state_mutex);
  camera_valid = valid && matrix && fov > 0;
  if (camera_valid) {
    std::copy(matrix, matrix + 12, camera_matrix);
    camera_fov = fov;
  }
}

bool emit(int id, const std::string& event_name, const std::string& payload_json) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!find_view(id)) {
      return false;
    }
  }
  std::string script = "window.dispatchEvent(new CustomEvent(" + json_string(event_name) +
                       ",{detail:" + (payload_json.empty() ? std::string("null") : payload_json) +
                       "}));";
  with_browser(id, [script](View& view) {
    CefRefPtr<CefFrame> frame = view.browser->GetMainFrame();
    frame->ExecuteJavaScript(script, frame->GetURL(), 0);
  });
  return true;
}

void destroy_owned(const std::string& owner) {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (const auto& entry : views) {
      if (entry.second->owner == owner) {
        ids.push_back(entry.first);
      }
    }
  }
  for (int id : ids) {
    destroy_view(id);
  }
}

void destroy_resource_views() {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (const auto& entry : views) {
      if (entry.second->owner.rfind('@', 0) != 0) {
        ids.push_back(entry.first);
      }
    }
  }
  for (int id : ids) {
    destroy_view(id);
  }
}

std::vector<std::string> take_messages() {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::vector<std::string> taken;
  taken.swap(messages);
  return taken;
}

std::vector<InternalMessage> take_internal_messages(const std::string& owner) {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::vector<InternalMessage> taken;
  auto found = internal_messages.find(owner);
  if (found != internal_messages.end()) {
    taken.swap(found->second);
  }
  return taken;
}

std::vector<FileRequest> take_file_requests() {
  std::lock_guard<std::mutex> lock(file_mutex);
  std::vector<FileRequest> taken;
  taken.swap(file_requests);
  return taken;
}

void complete_file(int id, bool found, std::string bytes) {
  CefRefPtr<FileHandler> handler;
  {
    std::lock_guard<std::mutex> lock(file_mutex);
    auto waiting = waiting_files.find(id);
    if (waiting == waiting_files.end()) {
      return;
    }
    handler = waiting->second;
    waiting_files.erase(waiting);
  }
  handler->complete(found, std::move(bytes));
}

int focused_view() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return view_holding_input();
}

namespace {

// The keys a player types into a page with: watched for presses that bring no key message.
constexpr int kWatchedKeys[] = {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_BACK, VK_TAB};
// A press is lost when no key message came this long after it (ms).
constexpr ULONGLONG kKeyMessageWait = 400;
// Asks the window's own thread to take the keyboard focus back (keep_keys).
const UINT kRetakeKeys = RegisterWindowMessageW(L"KCDS.RetakeKeys");
// The last key message the window got, a press seen on the keyboard since and whether it was
// already answered (present thread and UI thread).
std::atomic<ULONGLONG> last_key_message{0};
// The game window the views are drawn in (set every frame by keep_keys).
std::atomic<HWND> game_window{nullptr};
ULONGLONG unanswered_press = 0;
bool keys_down[256] = {};

bool physical_press() {
  bool pressed = false;
  auto watch = [&pressed](int key) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !keys_down[key]) {
      pressed = true;
    }
    keys_down[key] = down;
  };
  for (int key : kWatchedKeys) {
    watch(key);
  }
  for (int key = 'A'; key <= 'Z'; ++key) {
    watch(key);
  }
  for (int key = '0'; key <= '9'; ++key) {
    watch(key);
  }
  return pressed;
}

// Gives the focused view its browser focus again (CEF's own client does so on WM_SETFOCUS).
void refocus(int id) {
  with_browser(id, [](View& view) { view.browser->GetHost()->SetFocus(true); });
}

}  // namespace

void keep_keys(void* window_handle) {
  HWND window = static_cast<HWND>(window_handle);
  game_window.store(window);
  const bool watching =
      focused_view() != 0 && window && GetForegroundWindow() == window && !IsIconic(window);
  // While a page has the keyboard its keys come from the page keyboard's own thread: the game's
  // main thread no longer delays them, and no key message needs to arrive.
  const bool pressed = physical_press();
  if (!watching || page_keyboard::active()) {
    unanswered_press = 0;
    return;
  }
  const ULONGLONG now = GetTickCount64();
  if (pressed && unanswered_press == 0) {
    unanswered_press = now;
  }
  if (unanswered_press == 0) {
    return;
  }
  if (last_key_message.load() >= unanswered_press) {
    unanswered_press = 0;
    return;
  }
  if (now - unanswered_press >= kKeyMessageWait) {
    unanswered_press = 0;
    PostMessageW(window, kRetakeKeys, 0, 0);
  }
}

namespace {

int key_modifiers() {
  int modifiers = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) {
    modifiers |= EVENTFLAG_SHIFT_DOWN;
  }
  if (GetKeyState(VK_CONTROL) & 0x8000) {
    modifiers |= EVENTFLAG_CONTROL_DOWN;
  }
  if (GetKeyState(VK_MENU) & 0x8000) {
    modifiers |= EVENTFLAG_ALT_DOWN;
  }
  if (GetKeyState(VK_LBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  }
  if (GetKeyState(VK_RBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  }
  if (GetKeyState(VK_MBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  }
  return modifiers;
}

// A point in the game window's client area, in the frame's pixels and relative to the view.
CefMouseEvent view_point(HWND window, POINT client, int view_x, int view_y) {
  RECT area{};
  GetClientRect(window, &area);
  int client_width = std::max(1, static_cast<int>(area.right - area.left));
  int client_height = std::max(1, static_cast<int>(area.bottom - area.top));
  int frame_width = 0;
  int frame_height = 0;
  screen_size(frame_width, frame_height);
  if (frame_width <= 0 || frame_height <= 0) {
    frame_width = client_width;
    frame_height = client_height;
  }
  CefMouseEvent event;
  event.x = client.x * frame_width / client_width - view_x;
  event.y = client.y * frame_height / client_height - view_y;
  event.modifiers = static_cast<uint32_t>(key_modifiers());
  return event;
}

// Text typed into a focused view. KingdomCome.exe's window is an ANSI window, so its WM_CHAR
// carries the system code page and loses every character outside it; the text is taken from the
// key press instead, in the player's keyboard layout (ToUnicodeEx flag 4 keeps Windows' own
// keyboard state, so the game's TranslateMessage is not disturbed), and dead keys are composed
// here.
std::wstring dead_key;

wchar_t combining_mark(wchar_t accent) {
  switch (accent) {
    case L'\u00b4':
      return L'\u0301';  // acute
    case L'`':
      return L'\u0300';  // grave
    case L'^':
      return L'\u0302';  // circumflex
    case L'~':
      return L'\u0303';  // tilde
    case L'\u02c7':
      return L'\u030c';  // caron
    case L'\u00a8':
      return L'\u0308';  // diaeresis
    case L'\u02da':
      return L'\u030a';  // ring
    case L'\u02dd':
      return L'\u030b';  // double acute
    default:
      return 0;
  }
}

// The key's scan code as Chromium's key converter reads a Windows native key code:
// the scan code (lParam bits 16-23), with 0xE000 for an extended key (bit 24: right Ctrl and Alt,
// the arrows, Insert, Delete, Home, End, Page Up and Down, the keypad's Enter and slash). The page
// gets KeyboardEvent.code from it ('KeyA', 'ArrowLeft', ...); with the whole lParam it was empty.
int scan_code_of(LPARAM key_data) {
  int scan_code = static_cast<int>((key_data >> 16) & 0xFF);
  if ((key_data & (LPARAM(1) << 24)) != 0) {
    scan_code |= 0xE000;
  }
  return scan_code;
}

// The text a key press types with the keyboard `state` in the game window's layout.
std::wstring typed_with(const BYTE* state, HWND window, UINT key, UINT scan) {
  if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) {
    return std::wstring();
  }
  bool control = (state[VK_CONTROL] & 0x80) != 0;
  bool alt = (state[VK_MENU] & 0x80) != 0;
  if (control != alt) {
    return std::wstring();  // shortcuts (AltGr is Ctrl+Alt and still types)
  }
  wchar_t characters[8]{};
  HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(window, nullptr));
  int count = ToUnicodeEx(key, scan, state, characters, 8, 4, layout);
  if (count < 0) {
    dead_key.assign(1, characters[0]);
    return std::wstring();
  }
  if (count == 0) {
    return std::wstring();
  }
  std::wstring typed(characters, characters + std::min(count, 8));
  if (dead_key.empty()) {
    return typed;
  }
  wchar_t accent = dead_key[0];
  dead_key.clear();
  if (typed == L" ") {
    return std::wstring(1, accent);
  }
  wchar_t mark = combining_mark(accent);
  if (!mark) {
    return std::wstring(1, accent) + typed;
  }
  std::wstring composed = typed + mark;
  wchar_t precomposed[16]{};
  int length = FoldStringW(MAP_PRECOMPOSED, composed.data(), static_cast<int>(composed.size()),
                           precomposed, 16);
  return length > 0 ? std::wstring(precomposed, precomposed + length) : composed;
}

// The text of a key message, with the window thread's own keyboard state.
std::wstring typed_text(HWND window, WPARAM key, LPARAM key_data) {
  BYTE state[256]{};
  if (!GetKeyboardState(state)) {
    return std::wstring();
  }
  return typed_with(state, window, static_cast<UINT>(key),
                    static_cast<UINT>((key_data >> 16) & 0xFF));
}

void send_mouse(int id, std::function<void(CefRefPtr<CefBrowserHost>)> action) {
  with_browser(id, [action](View& view) { action(view.browser->GetHost()); });
}

// A key press or release and the text it types, to the focused page.
void send_key(int id, CefKeyEvent event, std::wstring typed) {
  send_mouse(id, [event, typed](CefRefPtr<CefBrowserHost> host) {
    host->SendKeyEvent(event);
    for (wchar_t unit : typed) {
      CefKeyEvent character = event;
      character.type = KEYEVENT_CHAR;
      character.windows_key_code = unit;
      character.character = static_cast<char16_t>(unit);
      character.unmodified_character = static_cast<char16_t>(unit);
      host->SendKeyEvent(character);
    }
  });
}

// The modifier keys as the keyboard has them now (the hook's thread has no window state).
int async_modifiers() {
  int modifiers = 0;
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    modifiers |= EVENTFLAG_SHIFT_DOWN;
  }
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
    modifiers |= EVENTFLAG_CONTROL_DOWN;
  }
  if (GetAsyncKeyState(VK_MENU) & 0x8000) {
    modifiers |= EVENTFLAG_ALT_DOWN;
  }
  return modifiers;
}

// The mouse from the page keyboard's thread: to the page that holds the input, as the window's
// mouse messages would have brought it.
void page_mouse(const page_keyboard::Mouse& mouse) {
  HWND window = game_window.load();
  int id = 0;
  int view_x = 0;
  int view_y = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    id = view_holding_input();
    auto view = find_view(id);
    if (!view || !window) {
      return;
    }
    view_x = view->options.x;
    view_y = view->options.y;
  }
  POINT client = mouse.at;
  ScreenToClient(window, &client);
  RECT area{};
  GetClientRect(window, &area);
  const bool inside = client.x >= area.left && client.x < area.right && client.y >= area.top &&
                      client.y < area.bottom;
  if (!inside && mouse.button != 0 && !mouse.released) {
    return;  // a click outside the game's window is not the page's
  }
  CefMouseEvent event = view_point(window, client, view_x, view_y);
  // The buttons held go with every move too: a move with the left button held selects text and
  // drags, one without it only hovers.
  const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  int modifiers = async_modifiers();
  if (GetAsyncKeyState(swapped ? VK_RBUTTON : VK_LBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  }
  if (GetAsyncKeyState(swapped ? VK_LBUTTON : VK_RBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  }
  if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
    modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  }
  event.modifiers = static_cast<uint32_t>(modifiers);
  if (mouse.button == 0) {
    send_mouse(id,
               [event](CefRefPtr<CefBrowserHost> host) { host->SendMouseMoveEvent(event, false); });
    return;
  }
  // Windows reads the buttons as they are on the mouse; a left-handed player swapped them.
  cef_mouse_button_type_t button = MBT_MIDDLE;
  if (mouse.button == VK_LBUTTON) {
    button = swapped ? MBT_RIGHT : MBT_LEFT;
  } else if (mouse.button == VK_RBUTTON) {
    button = swapped ? MBT_LEFT : MBT_RIGHT;
  }
  if (!mouse.released) {
    input_sent();
  }
  const bool released = mouse.released;
  const int clicks = mouse.clicks;
  send_mouse(id, [event, button, released, clicks](CefRefPtr<CefBrowserHost> host) {
    host->SendMouseClickEvent(event, button, released, clicks);
  });
}

// Whether a page has the keyboard and the game's window is in front (the page keyboard's thread).
bool page_wants_keys() {
  HWND window = game_window.load();
  return window && GetForegroundWindow() == window && !IsIconic(window) && focused_view() != 0;
}

// A key from the page keyboard's thread: to the focused page, as the window's key messages would
// have brought it.
void page_key(const page_keyboard::Key& key) {
  HWND window = game_window.load();
  const int id = focused_view();
  if (id == 0 || !window) {
    return;
  }
  const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool system = key.alt && !control;
  if (system && (key.virtual_key == VK_F4 || key.virtual_key == VK_TAB)) {
    return;  // Alt+F4 and Alt+Tab belong to Windows
  }
  if (key.virtual_key == VK_LWIN || key.virtual_key == VK_RWIN) {
    return;
  }
  if (!key.released) {
    last_key_message.store(GetTickCount64());
    input_sent();
  }
  CefKeyEvent event;
  event.windows_key_code = static_cast<int>(key.virtual_key);
  event.native_key_code = static_cast<int>(key.scan_code | (key.extended ? 0xE000u : 0u));
  event.is_system_key = system;
  event.modifiers = static_cast<uint32_t>(async_modifiers());
  event.type = key.released ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
  std::wstring typed;
  if (!key.released) {
    // The keyboard as Windows has it now, toggles (Caps Lock) included: GetKeyState(0) brings this
    // thread's copy up to date first.
    BYTE state[256]{};
    GetKeyState(0);
    if (GetKeyboardState(state)) {
      for (int modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LSHIFT, VK_RSHIFT, VK_LCONTROL,
                           VK_RCONTROL, VK_LMENU, VK_RMENU}) {
        state[modifier] = (GetAsyncKeyState(modifier) & 0x8000) ? 0x80 : 0;
      }
      typed = typed_with(state, window, key.virtual_key, key.scan_code);
    }
  }
  send_key(id, event, std::move(typed));
}

}  // namespace

bool handle_window_message(void* window_handle, unsigned message, std::uintptr_t wparam,
                           std::intptr_t lparam, std::intptr_t& result) {
  HWND window = static_cast<HWND>(window_handle);
  int id = 0;
  int view_x = 0;
  int view_y = 0;
  HCURSOR cursor = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    id = view_holding_input();
    auto view = find_view(id);
    if (!view) {
      return false;
    }
    view_x = view->options.x;
    view_y = view->options.y;
    cursor = view->cursor;
  }
  WPARAM w = static_cast<WPARAM>(wparam);
  LPARAM l = static_cast<LPARAM>(lparam);
  result = 0;
  if (message == kRetakeKeys) {
    // keep_keys saw keys typed that no key message brought: the window takes the
    // keyboard focus on its own thread and the view its browser focus.
    HWND before = GetFocus();
    SetFocus(window);
    refocus(id);
    note("WEB view %d: keys typed but none reached the window (focus %p); keyboard taken back", id,
         static_cast<void*>(before));
    return true;
  }
  switch (message) {
    case WM_SETFOCUS:
      // The window has the keyboard again (back from an Alt+Tab): so has the focused view.
      refocus(id);
      return false;
    case WM_ACTIVATE:
      if (LOWORD(w) != WA_INACTIVE) {
        refocus(id);
      }
      return false;
    case WM_SETCURSOR:
      if (LOWORD(l) != HTCLIENT || !cursor) {
        return false;
      }
      SetCursor(cursor);
      result = TRUE;
      return true;
    case WM_MOUSEMOVE: {
      if (page_keyboard::active()) {
        return true;  // the page keyboard's thread moves the page's pointer
      }
      CefMouseEvent event = view_point(window, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}, view_x, view_y);
      send_mouse(
          id, [event](CefRefPtr<CefBrowserHost> host) { host->SendMouseMoveEvent(event, false); });
      return true;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK: {
      if (page_keyboard::active()) {
        return true;  // the page keyboard's thread already gave the page this click
      }
      CefMouseEvent event = view_point(window, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}, view_x, view_y);
      cef_mouse_button_type_t button = MBT_LEFT;
      if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK) {
        button = MBT_RIGHT;
      } else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                 message == WM_MBUTTONDBLCLK) {
        button = MBT_MIDDLE;
      }
      bool released = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
      bool twice =
          message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK;
      if (released) {
        ReleaseCapture();
      } else {
        SetCapture(window);
        input_sent();
      }
      send_mouse(id, [event, button, released, twice](CefRefPtr<CefBrowserHost> host) {
        host->SendMouseClickEvent(event, button, released, twice ? 2 : 1);
      });
      return true;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // screen coordinates
      ScreenToClient(window, &point);
      CefMouseEvent event = view_point(window, point, view_x, view_y);
      int delta = GET_WHEEL_DELTA_WPARAM(w);
      bool horizontal = message == WM_MOUSEHWHEEL;
      send_mouse(id, [event, delta, horizontal](CefRefPtr<CefBrowserHost> host) {
        host->SendMouseWheelEvent(event, horizontal ? delta : 0, horizontal ? 0 : delta);
      });
      return true;
    }
    case WM_SYSKEYDOWN:
      if (w == VK_F4 || w == VK_TAB) {
        return false;  // Alt+F4 and Alt+Tab belong to Windows
      }
      [[fallthrough]];
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (page_keyboard::active()) {
        return true;  // the page keyboard's thread already gave the page this key
      }
      if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        last_key_message.store(GetTickCount64());
        input_sent();
      }
      CefKeyEvent event;
      event.windows_key_code = static_cast<int>(w);
      event.native_key_code = scan_code_of(l);
      event.is_system_key = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
      event.modifiers = static_cast<uint32_t>(key_modifiers());
      bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
      event.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
      std::wstring typed = down ? typed_text(window, w, l) : std::wstring();
      send_key(id, event, std::move(typed));
      return true;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
      return true;  // the text was taken from the key press
    default:
      return false;
  }
}

void set_screen_size(int width, int height) {
  std::vector<int> filling;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (width == screen_width && height == screen_height) {
      return;
    }
    int old_width = screen_width;
    int old_height = screen_height;
    screen_width = width;
    screen_height = height;
    for (const auto& entry : views) {
      ViewOptions& options = entry.second->options;
      if (options.width <= 0 || options.height <= 0) {
        filling.push_back(entry.first);
        continue;
      }
      // A view made as large as the screen (open with Web.screenSize()) covers the
      // whole screen: it keeps covering it when the resolution changes, so its page lays out
      // for the new size instead of staying cut off or small.
      bool covers_screen = options.x == 0 && options.y == 0 && old_width > 0 && old_height > 0 &&
                           options.width == old_width && options.height == old_height;
      if (covers_screen && width > 0 && height > 0) {
        options.width = width;
        options.height = height;
        filling.push_back(entry.first);
      }
    }
  }
  for (int id : filling) {
    with_browser(id, [](View& view) { view.browser->GetHost()->WasResized(); });
  }
}

void screen_size(int& width, int& height) {
  std::lock_guard<std::mutex> lock(state_mutex);
  width = screen_width;
  height = screen_height;
}

bool any_visible() {
  std::lock_guard<std::mutex> lock(state_mutex);
  for (const auto& entry : views) {
    const ViewFrame& frame = entry.second->frame;
    if (entry.second->options.visible && (frame.shared_handle || frame.pixels)) {
      return true;
    }
  }
  return false;
}

namespace {

// The anchor's place on screen with the current camera (state_mutex held); false when it has none.
bool anchor_position(const Anchor& anchor, float width, float height, int& x, int& y) {
  if (!camera_valid || width <= 0 || height <= 0) {
    return false;
  }
  const float* m = camera_matrix;
  const float dx = anchor.x - m[3];
  const float dy = anchor.y - m[7];
  const float dz = anchor.z - m[11];
  const float right = dx * m[0] + dy * m[4] + dz * m[8];
  const float forward = dx * m[1] + dy * m[5] + dz * m[9];
  const float up = dx * m[2] + dy * m[6] + dz * m[10];
  if (forward <= kNearPlane) {
    return false;
  }
  const float tan_half = std::tan(camera_fov / 2);
  const float aspect = width / height;
  const float screen_x = (1 + right / (forward * tan_half * aspect)) * 0.5f * width;
  const float screen_y = (1 - up / (forward * tan_half)) * 0.5f * height;
  if (screen_x < 0 || screen_x > width || screen_y < 0 || screen_y > height) {
    return false;
  }
  x = static_cast<int>(std::lround(screen_x)) + anchor.offset_x;
  y = static_cast<int>(std::lround(screen_y)) + anchor.offset_y;
  return true;
}

}  // namespace

void paint(ID2D1DeviceContext* canvas, float width, float height) {
  if (!canvas || !ensure_present_device(canvas)) {
    return;
  }
  struct Drawn {
    int id;
    int z;
    int x;
    int y;
    ViewFrame frame;
  };
  std::vector<Drawn> drawn;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (const auto& entry : views) {
      const View& view = *entry.second;
      if (!view.options.visible || (!view.frame.shared_handle && !view.frame.pixels)) {
        continue;
      }
      int x = view.options.x;
      int y = view.options.y;
      // An anchored view is placed with this frame's camera, so it stays on its point while the
      // camera turns (placed by a script it trailed the camera, live).
      if (view.anchor.active && !anchor_position(view.anchor, width, height, x, y)) {
        continue;
      }
      drawn.push_back({view.id, view.options.z_index, x, y, view.frame});
    }
  }
  // Views that are gone release their textures.
  for (auto it = present_views.begin(); it != present_views.end();) {
    bool alive = std::any_of(drawn.begin(), drawn.end(),
                             [&](const Drawn& item) { return item.id == it->first; });
    it = alive ? std::next(it) : present_views.erase(it);
  }
  std::sort(drawn.begin(), drawn.end(), [](const Drawn& left, const Drawn& right) {
    return left.z != right.z ? left.z < right.z : left.id < right.id;
  });
  for (const Drawn& item : drawn) {
    PresentView& target = present_views[item.id];
    if (item.frame.pixels) {
      if (!show_memory_frame(canvas, target, item.frame)) {
        continue;
      }
    } else {
      if (target.generation != item.frame.generation || target.handle != item.frame.shared_handle) {
        if (!open_present_view(canvas, target, item.frame)) {
          stop_sharing(item.id);
          continue;
        }
      }
      if (target.mutex && target.mutex->AcquireSync(0, 0) == S_OK) {
        present_context->CopyResource(target.copy.Get(), target.shared.Get());
        target.mutex->ReleaseSync(0);
      }
    }
    float left = static_cast<float>(item.x);
    float top = static_cast<float>(item.y);
    canvas->DrawBitmap(target.bitmap.Get(),
                       D2D1::RectF(left, top, left + static_cast<float>(item.frame.width),
                                   top + static_cast<float>(item.frame.height)));
  }
}

}  // namespace kcds::web
