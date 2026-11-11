#include "resource_web.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../chat/chat_overlay.hpp"
#include "../render/world_textures.hpp"
#include "../session/load_progress.hpp"
#include "../session/resource_cache.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcds/menu/compositor.hpp"
#include "kcds/web/web.hpp"

namespace kcds::runtime::web {
namespace {

namespace sdk = kcd2s::sdk;
namespace views = kcds::web;

Logger log_line = nullptr;
std::atomic<bool> started{false};

// What the current runtime last heard about the browser.
// The parts of kcds:// files still arriving: the runtime sends a file in parts under
// its 64 kB message limit, every part but the last with more: true.
constexpr std::size_t kMaxFileBytes = 64 * 1024 * 1024;
// A page's file read from the download cache: the catalog's largest (bundles.cjs).
constexpr std::uintmax_t kMaxCachedFileBytes = 512ull * 1024 * 1024;
std::mutex file_parts_mutex;
std::map<int, std::string> file_parts;

const void* reported_session = nullptr;
bool reported_ready = false;
int reported_width = -1;
int reported_height = -1;

std::string text_field(const sdk::Value& message, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (field && field->tag() == sdk::Tag::Str) {
    return field->as_str();
  }
  return std::string();
}

int number_field(const sdk::Value& message, const char* name, int fallback = 0) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Int) {
    return static_cast<int>(field->as_int());
  }
  if (field->tag() == sdk::Tag::Double) {
    return static_cast<int>(field->as_double());
  }
  return fallback;
}

float float_field(const sdk::Value& message, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (!field) {
    return 0;
  }
  if (field->tag() == sdk::Tag::Double) {
    return static_cast<float>(field->as_double());
  }
  if (field->tag() == sdk::Tag::Int) {
    return static_cast<float>(field->as_int());
  }
  return 0;
}

bool flag_field(const sdk::Value& message, const char* name, bool fallback = false) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (field && field->tag() == sdk::Tag::Bool) {
    return field->as_bool();
  }
  return fallback;
}

std::string decode_base64(const std::string& text) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string bytes;
  bytes.reserve(text.size() * 3 / 4);
  unsigned buffer = 0;
  int bits = 0;
  for (char character : text) {
    if (character == '=') {
      break;
    }
    std::size_t value = alphabet.find(character);
    if (value == std::string::npos) {
      continue;
    }
    buffer = (buffer << 6) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return bytes;
}

void run_call(const sdk::Value& message) {
  std::string call = text_field(message, "call");
  int id = number_field(message, "id");
  if (call == "create") {
    views::ViewOptions options;
    options.x = number_field(message, "x");
    options.y = number_field(message, "y");
    options.width = number_field(message, "width");
    options.height = number_field(message, "height");
    options.z_index = number_field(message, "zIndex");
    options.visible = flag_field(message, "visible", true);
    options.focus = flag_field(message, "focus", false);
    if (!views::create_view(id, text_field(message, "owner"), text_field(message, "url"),
                            options)) {
      log_line("WEB view %d could not be created", id);
    }
  } else if (call == "destroy") {
    views::destroy_view(id);
  } else if (call == "visible") {
    views::set_visible(id, flag_field(message, "value"));
  } else if (call == "focus") {
    views::set_focus(id, flag_field(message, "value"));
  } else if (call == "offscreen") {
    views::set_offscreen(id, flag_field(message, "value"));
  } else if (call == "load") {
    views::load_url(id, text_field(message, "url"));
  } else if (call == "resize") {
    views::resize_view(id, number_field(message, "width"), number_field(message, "height"));
  } else if (call == "move") {
    views::move_view(id, number_field(message, "x"), number_field(message, "y"));
  } else if (call == "anchor") {
    views::anchor_view(id, flag_field(message, "value"), float_field(message, "x"),
                       float_field(message, "y"), float_field(message, "z"),
                       number_field(message, "offsetX"), number_field(message, "offsetY"));
  } else if (call == "screen") {
    // Web.setViewWorldScreen: the screen's four corners as c0..c11 (x, y, z of each); texture
    // when the page goes into the texture of the resource's screen model, its page's size then
    // watched for (render/world_textures.hpp).
    float corners[12] = {};
    for (int index = 0; index < 12; ++index) {
      const std::string name = "c" + std::to_string(index);
      corners[index] = float_field(message, name.c_str());
    }
    // The texture is at least the page's size (with a bar's height); the page goes in its
    // top-left corner.
    const bool texture = flag_field(message, "texture");
    const int texture_width = number_field(message, "textureWidth");
    const int texture_height = number_field(message, "textureHeight");
    if (texture) {
      kcds::runtime::render::world_textures::want(texture_width, texture_height);
    }
    views::screen_view(id, flag_field(message, "value"), corners, texture,
                       number_field(message, "bar"), texture_width, texture_height);
  } else if (call == "emit") {
    views::emit(id, text_field(message, "name"), text_field(message, "payload"));
  } else if (call == "destroyOwned") {
    views::destroy_owned(text_field(message, "owner"));
  }
}

void observe_frame_size(int width, int height) {
  views::set_screen_size(width, height);
}

bool views_active() {
  return views::any_visible();
}

void paint_views(ID2D1DeviceContext* canvas, float width, float height, void* window) {
  views::keep_keys(window);
  views::paint(canvas, width, height);
}

// Chromium allows one browser process per cache folder: a second game process started on the
// same folder hands its start-up to the first one, which then fails a fatal check. Every game
// process claims its own slot under base by holding slot.lock open, exclusively, for its whole
// life; a single game always gets slot-0, so its storage stays the same.
constexpr int kCacheSlots = 16;
std::wstring claim_cache_slot(const std::wstring& base) {
  CreateDirectoryW(base.c_str(), nullptr);
  for (int slot = 0; slot < kCacheSlots; ++slot) {
    std::wstring folder = base + L"\\slot-" + std::to_wstring(slot);
    CreateDirectoryW(folder.c_str(), nullptr);
    HANDLE lock = CreateFileW((folder + L"\\slot.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock != INVALID_HANDLE_VALUE) {
      // Deliberately never closed: the lock lasts as long as the process.
      return folder;
    }
  }
  return std::wstring();
}

}  // namespace

void start(const std::wstring& module_directory, Logger logger) {
  if (started.exchange(true)) {
    return;
  }
  log_line = logger;
  wchar_t local_data[MAX_PATH] = {};
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_data, MAX_PATH);
  std::wstring cache = length > 0 && length < MAX_PATH ? std::wstring(local_data) + L"\\KCDS"
                                                       : module_directory + L"kcds-data";
  CreateDirectoryW(cache.c_str(), nullptr);
  views::Options options;
  options.runtime_folder = module_directory;
  options.cache_folder = claim_cache_slot(cache + L"\\web");
  if (options.cache_folder.empty()) {
    logger("WEB every browser cache slot is in use; web views are off in this process");
    return;
  }
  logger("WEB cache %ls", options.cache_folder.c_str());
  options.log = logger;
  kcd2s::native_chat::set_frame_size_observer(observe_frame_size);
  kcd2s::native_chat::set_view_layer({views_active, paint_views});
  kcd2s::native_chat::set_input_sink(views::handle_window_message);
  views::set_world_texture_source(kcds::runtime::render::world_textures::latest);
  if (!views::start(options)) {
    logger("WEB browser thread failed to start");
  }
}

bool has_input_focus() {
  return started.load() && views::focused_view() != 0;
}

// Whether a server's loading page is up: set by loadingPageShown, cleared by loadingPageClosed
// and by the end of a join.
std::atomic<bool> loading_page_shown{false};

bool loading_page_up() {
  return loading_page_shown.load();
}

void forget_loading_page() {
  loading_page_shown.store(false);
}

void set_view_camera(bool valid, const float* matrix, float fov) {
  views::set_view_camera(valid, matrix, fov);
}

bool handle_runtime_message(const sdk::Value& message) {
  std::string op = kcd2s::integration::script_operation(message);
  if (op == "web") {
    run_call(message);
    return true;
  }
  // A server's loading page is up (LoadingScreen): the server browser's window would
  // cover it. The join's end shows that window again when it goes back to the browser.
  if (op == "loadingPageShown") {
    loading_page_shown.store(true);
    kcds::menu::set_visible(false);
    return true;
  }
  if (op == "loadingPageClosed") {
    loading_page_shown.store(false);
    return true;
  }
  if (op == "webFile") {
    const int request = number_field(message, "fileRequest");
    // A big file the runtime left on the disk: read from the download cache,
    // when it is one of this server's files.
    const std::string cached = text_field(message, "cached");
    if (!cached.empty()) {
      const std::filesystem::path source = kcds::runtime::session::cached_resource_file(cached);
      std::string file;
      std::error_code error;
      const auto size = source.empty() ? 0 : std::filesystem::file_size(source, error);
      bool found = !source.empty() && !error && size <= kMaxCachedFileBytes;
      if (found) {
        std::ifstream in(source, std::ios::binary);
        file.assign(std::size_t(size), '\0');
        found = size == 0 || static_cast<bool>(in.read(file.data(), std::streamsize(size)));
      }
      views::complete_file(request, found, found ? std::move(file) : std::string());
      return true;
    }
    std::string part = decode_base64(text_field(message, "data"));
    std::string file;
    {
      std::lock_guard<std::mutex> lock(file_parts_mutex);
      auto parts = file_parts.find(request);
      if (parts != file_parts.end()) {
        file = std::move(parts->second);
        file_parts.erase(parts);
      }
    }
    if (file.size() + part.size() > kMaxFileBytes) {
      views::complete_file(request, false, std::string());
      return true;
    }
    file += part;
    if (flag_field(message, "more")) {
      std::lock_guard<std::mutex> lock(file_parts_mutex);
      file_parts[request] = std::move(file);
      return true;
    }
    views::complete_file(request, flag_field(message, "found"), std::move(file));
    return true;
  }
  return false;
}

void pump(const void* session, const RuntimeSender& send) {
  if (session != reported_session) {
    // The pages of the runtime before it are gone with it: its view ids would refuse the new
    // runtime's, which numbers from 1 again.
    if (reported_session) {
      views::destroy_resource_views();
    }
    reported_session = session;  // a new runtime has heard nothing yet
    reported_ready = false;
    reported_width = -1;
    reported_height = -1;
  }
  bool ready = views::ready();
  int width = 0;
  int height = 0;
  views::screen_size(width, height);
  if (ready != reported_ready || width != reported_width || height != reported_height) {
    sdk::Value::Map host{{"op", sdk::Value(std::string("webHost"))},
                         {"ready", sdk::Value(ready)},
                         {"width", sdk::Value(static_cast<std::int64_t>(width))},
                         {"height", sdk::Value(static_cast<std::int64_t>(height))}};
    if (send(sdk::Value(std::move(host)))) {
      reported_ready = ready;
      reported_width = width;
      reported_height = height;
    }
  }
  // The level's load, for a server's loading page (LoadingScreen).
  const int load_percent = kcds::runtime::session::load_progress::take_changed();
  if (load_percent >= 0) {
    sdk::Value::Map progress{{"op", sdk::Value(std::string("loadingProgress"))},
                             {"percent", sdk::Value(static_cast<std::int64_t>(load_percent))}};
    send(sdk::Value(std::move(progress)));
  }
  for (std::string& json : views::take_messages()) {
    sdk::Value::Map event{{"op", sdk::Value(std::string("webEvent"))},
                          {"json", sdk::Value(std::move(json))}};
    send(sdk::Value(std::move(event)));
  }
  for (views::FileRequest& request : views::take_file_requests()) {
    sdk::Value::Map file{{"op", sdk::Value(std::string("webFileRequest"))},
                         {"fileRequest", sdk::Value(static_cast<std::int64_t>(request.id))},
                         {"resource", sdk::Value(std::move(request.resource))},
                         {"path", sdk::Value(std::move(request.path))}};
    if (!send(sdk::Value(std::move(file)))) {
      views::complete_file(request.id, false, std::string());
    }
  }
}

}  // namespace kcds::runtime::web
