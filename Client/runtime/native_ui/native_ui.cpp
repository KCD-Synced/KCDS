#include "native_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MinHook.h"
#include "kcd2s/integration/item_icon.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcds/menu_native/main_menu.hpp"
#include "../assets/memory_paks.hpp"
#include "../files/base64.hpp"
#include "../files/stored_zip.hpp"
#include "../session/resource_cache.hpp"

namespace kcds::runtime::native_ui {
namespace {

namespace sdk = kcd2s::sdk;

// IFlashUI: +112 GetUIElement(name) and IUIElement +32 GetInstance(id) return smart pointers
// (released with the object's slot 2). reads IFlashUI at +15 (mov rcx, [rip+...]).
constexpr std::size_t kFlashUiRead = 15;
constexpr std::size_t kGetElement = 112;
constexpr std::size_t kGetInstance = 32;
constexpr std::size_t kRelease = 16;
// IUIElement (vtable byte offsets).
constexpr std::size_t kIsVisible = 232;
// A resource screen's element instance (Client/resources/native_ui.cjs SCREEN_INSTANCE_BASE + id).
constexpr int kScreenInstanceBase = 47100;
constexpr std::size_t kUnload = 128;
constexpr std::size_t kReload = 136;
constexpr std::size_t kRequestHide = 208;
constexpr std::size_t kSetVisible = 224;
constexpr std::size_t kSetFlag = 240;
// SetLayer (+288) writes the element's place in the UI stack (+76, the XML's GFx layer).
constexpr std::size_t kSetLayer = 288;
constexpr unsigned kCursorFlag = 1;
constexpr std::size_t kVariableDesc = 352;
constexpr std::size_t kArrayDesc = 376;
constexpr std::size_t kFunctionDesc = 472;
constexpr std::size_t kAddListener = 528;
constexpr std::size_t kRemoveListener = 536;
constexpr std::size_t kCallFunction = 544;
constexpr std::size_t kMovieClip = 568;
constexpr std::size_t kSetVariable = 616;
constexpr std::size_t kGetVariable = 632;
constexpr std::size_t kSetArray = 664;
constexpr std::size_t kGetArray = 680;
// IFlashVariableObject (a movie clip).
constexpr std::size_t kSetDisplayInfo = 200;
constexpr std::size_t kGetDisplayInfo = 208;
constexpr std::size_t kPlayFrame = 336;
constexpr std::size_t kPlayLabel = 344;
constexpr std::size_t kStopFrame = 352;
constexpr std::size_t kStopLabel = 360;
// SUIEventDesc: the event's display name.
constexpr std::size_t kEventName = 8;

// TUIData.
constexpr std::size_t kDataType = 16;
enum DataType : std::uint8_t {
  kInt = 0,
  kFloat = 1,
  kEntityId = 2,
  kVec3 = 3,
  kString = 4,
  kWideString = 5,
  kBool = 6,
};
// SUIArguments: a DynArray of 32-byte entries, each a TUIData at +8; the count is at data - 4.
constexpr std::size_t kArgumentStride = 32;
constexpr std::size_t kArgumentData = 8;
constexpr int kMaxArguments = 16;
constexpr std::size_t kMaxString = 65536;

// SFlashDisplayInfo: x, y, z, xscale, yscale, zscale, rotation, xrotation, yrotation, alpha,
// visible, and which of them are set (+52).
struct DisplayInfo {
  float x;
  float y;
  float z;
  float xscale;
  float yscale;
  float zscale;
  float rotation;
  float xrotation;
  float yrotation;
  float alpha;
  bool visible;
  std::uint8_t pad[11];
  std::uint16_t set;
  std::uint8_t tail[10];
};
static_assert(sizeof(DisplayInfo) == 64);
enum DisplayFlag : std::uint16_t {
  kX = 1,
  kY = 2,
  kXScale = 8,
  kYScale = 16,
  kRotation = 64,
  kAlpha = 512,
  kVisible = 1024,
};

using ElementGet = void*(__fastcall*)(void* unused, const char* name, int instance);
using StringCall = void*(__fastcall*)(void* string);
using ArgsCall = void*(__fastcall*)(void* args);
using ArgsAdd = void*(__fastcall*)(void* args, const void* data);
using Lookup = void*(__fastcall*)(void* element, const char* name);
using Call = bool(__fastcall*)(void* element, void* desc, void* args, void* result, void* unused);
using VariableCall = bool(__fastcall*)(void* element, void* desc, void* data, void* unused);
using ElementCall = void*(__fastcall*)(void* element);
using ElementFlag = void*(__fastcall*)(void* element, bool flag);
using ElementSetFlag = void*(__fastcall*)(void* element, unsigned flag, bool value);
using ElementSetLayer = void(__fastcall*)(void* element, int layer);
using MovieClip = void*(__fastcall*)(void* element, const char* name, void* unused);
using DisplayCall = void*(__fastcall*)(void* clip, DisplayInfo* info);
using FrameCall = void*(__fastcall*)(void* clip, unsigned frame);
using LabelCall = void*(__fastcall*)(void* clip, const char* label);
using ListenerCall = void*(__fastcall*)(void* element, void* listener, const char* name);

// C_UIApse (vtable byte offsets) and its open state machine.
constexpr std::size_t kMenuClose = 16;
constexpr std::size_t kMenuPage = 24;
constexpr std::size_t kMenuState = 104;
constexpr std::size_t kStateGet = 8;
// Open's context: a page id, then a std::function the refused Open frees like the game's own.
constexpr std::size_t kContextFunction = 8;

using MenuOpen = bool(__fastcall*)(void* menu, std::uint8_t page, void* context);
using MenuMove = std::int64_t(__fastcall*)(void* menu, std::uint8_t page, std::uint8_t a,
                                           std::uint8_t b);
using MenuClose = void(__fastcall*)(void* menu, bool instant);
using MenuGet = std::uint8_t(__fastcall*)(void* object);
using FunctionFree = void(__fastcall*)(void* function);

Logger log_line = nullptr;
Natives natives;
MenuOpen original_menu_open = nullptr;
MenuMove original_menu_move = nullptr;
// Bit n set: page n may not open.
std::atomic<std::uint32_t> disabled_pages{0};
// The only page the menu may show, or -1.
std::atomic<int> held_page{-1};
std::atomic<void*> menu_object{nullptr};
std::atomic<bool> screen_focus{false};
void** flash_ui = nullptr;  // gEnv->pFlashUI

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

// One TUIData value, freed with the game's own destructor.
class UiData {
 public:
  UiData() {
    std::memset(bytes_, 0, sizeof(bytes_));
  }
  ~UiData() {
    reinterpret_cast<StringCall>(natives.data_free)(bytes_);
  }
  UiData(const UiData&) = delete;
  UiData& operator=(const UiData&) = delete;

  void* raw() {
    return bytes_;
  }

  // From a script value: numbers are floats unless integral, as the UIAction binding reads Lua
  // numbers; null is the empty string.
  bool set(const sdk::Value& value) {
    switch (value.tag()) {
      case sdk::Tag::Bool:
        bytes_[0] = value.as_bool() ? 1 : 0;
        bytes_[kDataType] = kBool;
        return true;
      case sdk::Tag::Int:
        *reinterpret_cast<std::int32_t*>(bytes_) = static_cast<std::int32_t>(value.as_int());
        bytes_[kDataType] = kInt;
        return true;
      case sdk::Tag::Double:
        *reinterpret_cast<float*>(bytes_) = static_cast<float>(value.as_double());
        bytes_[kDataType] = kFloat;
        return true;
      case sdk::Tag::Str:
        return set_string(value.as_str());
      case sdk::Tag::Null:
        return set_string("");
      default:
        return false;
    }
  }

  sdk::Value get() const {
    switch (bytes_[kDataType]) {
      case kInt:
        return sdk::Value(
            static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(bytes_)));
      case kFloat:
        return sdk::Value(static_cast<double>(*reinterpret_cast<const float*>(bytes_)));
      case kEntityId:
        return sdk::Value(
            static_cast<std::int64_t>(*reinterpret_cast<const std::uint32_t*>(bytes_)));
      case kVec3: {
        auto values = reinterpret_cast<const float*>(bytes_);
        return sdk::Value(sdk::Value::Array{sdk::Value(static_cast<double>(values[0])),
                                            sdk::Value(static_cast<double>(values[1])),
                                            sdk::Value(static_cast<double>(values[2]))});
      }
      case kString: {
        auto text = *reinterpret_cast<const char* const*>(bytes_);
        return sdk::Value(std::string(text ? text : ""));
      }
      case kWideString: {
        auto text = *reinterpret_cast<const wchar_t* const*>(bytes_);
        return sdk::Value(narrow(text));
      }
      case kBool:
        return sdk::Value(bytes_[0] != 0);
      default:
        return sdk::Value::null();
    }
  }

  static sdk::Value read(const std::uint8_t* data) {
    UiData view;
    std::memcpy(view.bytes_, data, 24);
    sdk::Value value = view.get();
    std::memset(view.bytes_, 0, sizeof(view.bytes_));  // a view: nothing of its own to free
    return value;
  }

 private:
  bool set_string(const std::string& text) {
    if (text.size() > kMaxString) {
      return false;
    }
    void* string = nullptr;
    reinterpret_cast<StringCall>(natives.string_init)(&string);
    natives.string_assign(&string, text.c_str());
    std::memcpy(bytes_, &string, sizeof(string));
    bytes_[kDataType] = kString;
    return true;
  }

  static std::string narrow(const wchar_t* text) {
    if (!text) {
      return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
      return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
  }

  alignas(16) std::uint8_t bytes_[32];
};

// One SUIArguments, freed with the game's own destructor.
class UiArguments {
 public:
  UiArguments() {
    std::memset(bytes_, 0, sizeof(bytes_));
    reinterpret_cast<ArgsCall>(natives.args_init)(bytes_);
  }
  ~UiArguments() {
    reinterpret_cast<ArgsCall>(natives.args_free)(bytes_);
  }
  UiArguments(const UiArguments&) = delete;
  UiArguments& operator=(const UiArguments&) = delete;

  void* raw() {
    return bytes_;
  }

  bool add(const sdk::Value& value) {
    UiData data;
    if (!data.set(value)) {
      return false;
    }
    reinterpret_cast<ArgsAdd>(natives.args_add)(bytes_, data.raw());
    return true;
  }

  static sdk::Value::Array read(const void* args) {
    sdk::Value::Array out;
    auto data = *reinterpret_cast<const std::uint8_t* const*>(args);
    if (!data) {
      return out;
    }
    const int count = *reinterpret_cast<const std::int32_t*>(data - 4);
    for (int index = 0; index < count && index < 4096; ++index) {
      out.push_back(UiData::read(data + kArgumentData + kArgumentStride * index));
    }
    return out;
  }

 private:
  alignas(16) std::uint8_t bytes_[64];
};

// The element instance a request names; null when the game has none (not loaded).
void* element_of(const std::string& name, int instance) {
  if (name.empty() || name.size() > 256 || instance < 0) {
    return nullptr;
  }
  return reinterpret_cast<ElementGet>(natives.element)(nullptr, name.c_str(), instance);
}

using SmartGet = void**(__fastcall*)(void* owner, void** out, const char* name);
using SmartInstance = void**(__fastcall*)(void* owner, void** out, unsigned id);
using Release = void(__fastcall*)(void* object);

// The element instance whether or not its movie is loaded (showing one loads it); the game keeps
// the element, so the smart pointers are released at once, as the UIAction binding does.
void* element_raw(const std::string& name, int instance) {
  void* ui = flash_ui ? *flash_ui : nullptr;
  if (!ui || name.empty() || name.size() > 256 || instance < 0) {
    return nullptr;
  }
  void* holder = nullptr;
  void* element = *slot<SmartGet>(ui, kGetElement)(ui, &holder, name.c_str());
  if (holder) {
    slot<Release>(holder, kRelease)(holder);
  }
  if (!element || instance == 0) {
    return element;
  }
  void* copy = nullptr;
  void* chosen =
      *slot<SmartInstance>(element, kGetInstance)(element, &copy, static_cast<unsigned>(instance));
  if (copy) {
    slot<Release>(copy, kRelease)(copy);
  }
  return chosen;
}

const sdk::Value* field(const sdk::Value& message, const char* name) {
  return kcd2s::integration::script_field(message, name);
}

std::string text_field(const sdk::Value& message, const char* name) {
  const sdk::Value* value = field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

bool has_number(const sdk::Value* value) {
  return value && (value->tag() == sdk::Tag::Int || value->tag() == sdk::Tag::Double);
}

double number(const sdk::Value* value) {
  return value->tag() == sdk::Tag::Int ? static_cast<double>(value->as_int()) : value->as_double();
}

// --- element listener ---------------------------------------------------------------------

struct Event {
  std::string name;
  int instance = 0;
  std::string event;
  sdk::Value::Array args;
};

std::mutex queue_mutex;
std::vector<Event> events;    // raised by the movies, not yet filtered
std::vector<Event> outgoing;  // events a resource listens for
// Keys of the resources' Esc menu buttons the player pressed, queue_mutex held.
std::vector<std::string> menu_presses;

// A press of a resource's Esc menu button (game thread): its key goes to the runtime.
void menu_button_pressed(const std::string& key) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (menu_presses.size() < 16) {
    menu_presses.push_back(key);
  }
  if (log_line) {
    log_line("NATIVEUI menu button %s pressed", key.c_str());
  }
}

// IUIElementEventListener (WHGame 1.5.5, as SUIElementLuaCallback): OnUIEvent,
// OnUIEventEx, five notifications, then the destructor.
class ElementListener {
 public:
  ElementListener(std::string name, int instance) : name_(std::move(name)), instance_(instance) {
  }
  virtual void __fastcall on_event(void* sender, const void* event, const void* args) {
    (void)sender;
    auto title =
        *reinterpret_cast<const char* const*>(static_cast<const std::uint8_t*>(event) + kEventName);
    Event entry{name_, instance_, title ? title : "", UiArguments::read(args)};
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (events.size() < 256) {
      events.push_back(std::move(entry));
    }
  }
  // An fscommand the element declares no event for, such as the screen host movie's
  // fscommand("kcdsClip", "<clip id>,<event>").
  virtual void __fastcall on_event_ex(void* sender, const char* command, const void* args,
                                      void* user) {
    (void)sender;
    (void)user;
    if (!command) {
      return;
    }
    Event entry{name_, instance_, command, UiArguments::read(args)};
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (events.size() < 256) {
      events.push_back(std::move(entry));
    }
  }
  virtual void __fastcall on_init(void*, void*) {
  }
  virtual void __fastcall on_unload(void*) {
  }
  virtual void __fastcall on_set_visible(void*, bool) {
  }
  virtual void __fastcall on_instance_created(void*, void*) {
  }
  virtual void __fastcall on_instance_destroyed(void*, void*) {
  }
  virtual ~ElementListener() = default;

 private:
  std::string name_;
  int instance_;
};

struct Listening {
  std::unique_ptr<ElementListener> listener;
  void* element = nullptr;
  std::map<std::string, int> events;  // event -> resources listening
};
std::map<std::pair<std::string, int>, Listening> listening;  // engine thread only

// --- requests ---------------------------------------------------------------------------------

struct Request {
  std::int64_t id = 0;  // 0 for a change without an answer
  sdk::Value body;
  bool posted = false;  // an element request whose answer nobody waits for
};
struct Reply {
  std::int64_t id = 0;
  sdk::Value value;
};

std::vector<Request> requests;
std::vector<Reply> replies;
const void* reported_session = nullptr;

// The engine hands an ActionScript undefined over as the string UNDEFINED.
sdk::Value defined(sdk::Value value) {
  if (value.tag() == sdk::Tag::Str && value.as_str() == "UNDEFINED") {
    return sdk::Value::null();
  }
  return value;
}

sdk::Value call_function(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "fn");
  void* desc = name.empty() ? nullptr : slot<Lookup>(element, kFunctionDesc)(element, name.c_str());
  if (!desc) {
    return sdk::Value::null();
  }
  UiArguments args;
  const sdk::Value* list = field(body, "args");
  if (list && list->tag() == sdk::Tag::Array) {
    int count = 0;
    for (const sdk::Value& value : list->as_array()) {
      if (++count > kMaxArguments || !args.add(value)) {
        return sdk::Value::null();
      }
    }
  }
  UiData result;
  if (!slot<Call>(element, kCallFunction)(element, desc, args.raw(), result.raw(), nullptr)) {
    return sdk::Value::null();
  }
  return defined(result.get());
}

// A variable the element does not declare is a member below its movie's _root ("a.b.c").
sdk::Value set_member_path(void* element, const std::string& name, const sdk::Value& value);
sdk::Value get_member_path(void* element, const std::string& name);

sdk::Value set_variable(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "variable");
  void* desc = name.empty() ? nullptr : slot<Lookup>(element, kVariableDesc)(element, name.c_str());
  const sdk::Value* value = field(body, "value");
  if (!value) {
    return sdk::Value(false);
  }
  if (!desc) {
    return set_member_path(element, name, *value);
  }
  UiData data;
  if (!data.set(*value)) {
    return sdk::Value(false);
  }
  return sdk::Value(slot<VariableCall>(element, kSetVariable)(element, desc, data.raw(), nullptr));
}
sdk::Value get_variable(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "variable");
  void* desc = name.empty() ? nullptr : slot<Lookup>(element, kVariableDesc)(element, name.c_str());
  if (!desc) {
    return get_member_path(element, name);
  }
  UiData data;
  if (!slot<VariableCall>(element, kGetVariable)(element, desc, data.raw(), nullptr)) {
    return sdk::Value::null();
  }
  return defined(data.get());
}

sdk::Value set_array(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "array");
  void* desc = name.empty() ? nullptr : slot<Lookup>(element, kArrayDesc)(element, name.c_str());
  const sdk::Value* list = field(body, "values");
  if (!desc || !list || list->tag() != sdk::Tag::Array) {
    return sdk::Value(false);
  }
  UiArguments args;
  for (const sdk::Value& value : list->as_array()) {
    if (!args.add(value)) {
      return sdk::Value(false);
    }
  }
  return sdk::Value(slot<VariableCall>(element, kSetArray)(element, desc, args.raw(), nullptr));
}

sdk::Value get_array(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "array");
  void* desc = name.empty() ? nullptr : slot<Lookup>(element, kArrayDesc)(element, name.c_str());
  UiArguments args;
  if (!desc || !slot<VariableCall>(element, kGetArray)(element, desc, args.raw(), nullptr)) {
    return sdk::Value::null();
  }
  return sdk::Value(UiArguments::read(args.raw()));
}

void* movie_clip(void* element, const sdk::Value& body) {
  const std::string name = text_field(body, "clip");
  if (name.empty() || name.size() > 256) {
    return nullptr;
  }
  return slot<MovieClip>(element, kMovieClip)(element, name.c_str(), nullptr);
}

sdk::Value set_clip(void* element, const sdk::Value& body) {
  void* clip = movie_clip(element, body);
  if (!clip) {
    return sdk::Value(false);
  }
  DisplayInfo info{};
  slot<DisplayCall>(clip, kGetDisplayInfo)(clip, &info);
  info.set = 0;
  auto apply = [&](const char* name, float& target, float scale, DisplayFlag flag) {
    const sdk::Value* value = field(body, name);
    if (has_number(value) && std::isfinite(number(value))) {
      target = static_cast<float>(number(value) * scale);
      info.set |= flag;
    }
  };
  // Scale and alpha are multipliers here; the movie keeps them in percent.
  apply("x", info.x, 1.0f, kX);
  apply("y", info.y, 1.0f, kY);
  apply("rotation", info.rotation, 1.0f, kRotation);
  apply("scaleX", info.xscale, 100.0f, kXScale);
  apply("scaleY", info.yscale, 100.0f, kYScale);
  apply("alpha", info.alpha, 100.0f, kAlpha);
  const sdk::Value* visible = field(body, "visible");
  if (visible && visible->tag() == sdk::Tag::Bool) {
    info.visible = visible->as_bool();
    info.set |= kVisible;
  }
  slot<DisplayCall>(clip, kSetDisplayInfo)(clip, &info);
  return sdk::Value(true);
}

sdk::Value get_clip(void* element, const sdk::Value& body) {
  void* clip = movie_clip(element, body);
  if (!clip) {
    return sdk::Value::null();
  }
  DisplayInfo info{};
  slot<DisplayCall>(clip, kGetDisplayInfo)(clip, &info);
  return sdk::Value(sdk::Value::Map{
      {"x", sdk::Value(static_cast<double>(info.x))},
      {"y", sdk::Value(static_cast<double>(info.y))},
      {"rotation", sdk::Value(static_cast<double>(info.rotation))},
      {"scaleX", sdk::Value(static_cast<double>(info.xscale) / 100.0)},
      {"scaleY", sdk::Value(static_cast<double>(info.yscale) / 100.0)},
      {"alpha", sdk::Value(static_cast<double>(info.alpha) / 100.0)},
      {"visible", sdk::Value(info.visible)},
  });
}

sdk::Value goto_clip(void* element, const sdk::Value& body) {
  void* clip = movie_clip(element, body);
  const sdk::Value* frame = field(body, "frame");
  const sdk::Value* play = field(body, "play");
  const bool playing = play && play->tag() == sdk::Tag::Bool && play->as_bool();
  if (!clip || !frame) {
    return sdk::Value(false);
  }
  if (frame->tag() == sdk::Tag::Int && frame->as_int() >= 1) {
    // The timeline counts from 0; scripts count frames from 1 as ActionScript does.
    const auto index = static_cast<unsigned>(frame->as_int() - 1);
    slot<FrameCall>(clip, playing ? kPlayFrame : kStopFrame)(clip, index);
    return sdk::Value(true);
  }
  if (frame->tag() == sdk::Tag::Str && !frame->as_str().empty() && frame->as_str().size() < 256) {
    slot<LabelCall>(clip, playing ? kPlayLabel : kStopLabel)(clip, frame->as_str().c_str());
    return sdk::Value(true);
  }
  return sdk::Value(false);
}

// --- movie clips ------------------------------------------------------------------------------
//
// NativeScreen and NativeClip work on an element's movie through IFlashVariableObject (this
// build's vtable, byte offsets): +0 Release, +32 IsDisplayObject, +64 SetMember(name, value), +72
// GetMember(name, object&), +80 GetMember(name, value&), +112 Invoke(method, args, count, result),
// +88 VisitMembers(visitor), +320 CreateEmptyMovieClip(out, name, depth),
// +328 AttachMovie(out, symbol, name, depth, init). The element's root clip is its "_root" movie
// clip descriptor. Values cross as SFlashVarValue: the type at +0 (0 undefined, 1 null, 2 bool, 3
// int, 4 uint, 5 double, 6 float, 7 string, 8 wide string, 9 object), the value at +8.
constexpr std::size_t kObjectRelease = 0;
constexpr std::size_t kIsDisplayObject = 32;
constexpr std::size_t kSetMember = 64;
constexpr std::size_t kGetMemberObject = 72;
constexpr std::size_t kGetMemberValue = 80;
constexpr std::size_t kInvoke = 112;
constexpr std::size_t kCreateEmptyClip = 320;
constexpr std::size_t kAttachMovie = 328;
constexpr std::size_t kMaxPath = 32;
constexpr std::size_t kMaxInvokeArguments = 16;

struct FlashValue {
  std::int32_t type = 0;
  std::int32_t pad = 0;
  union {
    bool flag;
    std::int32_t integer;
    std::uint32_t unsigned_integer;
    double real;
    float single;
    const char* text;
    const wchar_t* wide;
    const void* object;
  };
  FlashValue() : real(0) {
  }
};
static_assert(sizeof(FlashValue) == 16);
enum FlashType : std::int32_t {
  kFlashUndefined = 0,
  kFlashNull = 1,
  kFlashBool = 2,
  kFlashInt = 3,
  kFlashUInt = 4,
  kFlashDouble = 5,
  kFlashFloat = 6,
  kFlashString = 7,
  kFlashWideString = 8,
};

using ObjectRelease = void(__fastcall*)(void* object);
using IsDisplayCall = bool(__fastcall*)(void* object);
using SetMemberCall = bool(__fastcall*)(void* object, const char* name, const FlashValue* value);
using GetObjectCall = bool(__fastcall*)(void* object, const char* name, void** out);
using GetValueCall = bool(__fastcall*)(void* object, const char* name, FlashValue* out);
using InvokeCall = bool(__fastcall*)(void* object, const char* method, const FlashValue* args,
                                     unsigned count, FlashValue* result);
using CreateClipCall = bool(__fastcall*)(void* object, void** out, const char* name, int depth);
using AttachCall = bool(__fastcall*)(void* object, void** out, const char* symbol, const char* name,
                                     int depth, void* init);

// A clip found by its path below _root; the clips GetMember hands out are released here.
class ClipRef {
 public:
  ClipRef() = default;
  ~ClipRef() {
    reset();
  }
  ClipRef(const ClipRef&) = delete;
  ClipRef& operator=(const ClipRef&) = delete;

  void* get() const {
    return object_;
  }

  void take(void* object, bool owned) {
    reset();
    object_ = object;
    owned_ = owned;
  }

 private:
  void reset() {
    if (object_ && owned_) {
      slot<ObjectRelease>(object_, kObjectRelease)(object_);
    }
    object_ = nullptr;
    owned_ = false;
  }

  void* object_ = nullptr;
  bool owned_ = false;
};

// A movie member or clip name: letters, digits and underscores, as ActionScript identifiers.
bool identifier(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (const char character : name) {
    const bool letter =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    const bool digit = character >= '0' && character <= '9';
    if (!letter && !digit && character != '_') {
      return false;
    }
  }
  return true;
}

bool resolve_clip(void* element, const sdk::Value* path, ClipRef& out) {
  void* root = slot<MovieClip>(element, kMovieClip)(element, "_root", nullptr);
  if (!root) {
    return false;
  }
  out.take(root, false);
  if (!path) {
    return true;
  }
  if (path->tag() != sdk::Tag::Array || path->as_array().size() > kMaxPath) {
    return false;
  }
  for (const sdk::Value& part : path->as_array()) {
    if (part.tag() != sdk::Tag::Str || !identifier(part.as_str())) {
      return false;
    }
    void* child = nullptr;
    void* current = out.get();
    slot<GetObjectCall>(current, kGetMemberObject)(current, part.as_str().c_str(), &child);
    if (!child) {
      out.take(nullptr, false);
      return false;
    }
    out.take(child, true);
  }
  return true;
}

// A script value as a movie value; a string stays in text, which outlives the call.
bool to_flash(const sdk::Value& value, FlashValue& out, std::string& text) {
  switch (value.tag()) {
    case sdk::Tag::Null:
      out.type = kFlashNull;
      return true;
    case sdk::Tag::Bool:
      out.type = kFlashBool;
      out.flag = value.as_bool();
      return true;
    case sdk::Tag::Int:
      if (value.as_int() >= INT32_MIN && value.as_int() <= INT32_MAX) {
        out.type = kFlashInt;
        out.integer = static_cast<std::int32_t>(value.as_int());
      } else {
        out.type = kFlashDouble;
        out.real = static_cast<double>(value.as_int());
      }
      return true;
    case sdk::Tag::Double:
      out.type = kFlashDouble;
      out.real = value.as_double();
      return true;
    case sdk::Tag::Str:
      if (value.as_str().size() > kMaxString) {
        return false;
      }
      text = value.as_str();
      out.type = kFlashString;
      out.text = text.c_str();
      return true;
    default:
      return false;
  }
}

sdk::Value from_flash(const FlashValue& value) {
  switch (value.type) {
    case kFlashBool:
      return sdk::Value(value.flag);
    case kFlashInt:
      return sdk::Value(static_cast<std::int64_t>(value.integer));
    case kFlashUInt:
      return sdk::Value(static_cast<std::int64_t>(value.unsigned_integer));
    case kFlashDouble:
      return sdk::Value(value.real);
    case kFlashFloat:
      return sdk::Value(static_cast<double>(value.single));
    case kFlashString:
      return sdk::Value(std::string(value.text ? value.text : ""));
    case kFlashWideString: {
      if (!value.wide) {
        return sdk::Value(std::string());
      }
      const int size =
          WideCharToMultiByte(CP_UTF8, 0, value.wide, -1, nullptr, 0, nullptr, nullptr);
      if (size <= 1) {
        return sdk::Value(std::string());
      }
      std::string out(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.wide, -1, out.data(), size, nullptr, nullptr);
      out.pop_back();
      return sdk::Value(std::move(out));
    }
    default:
      return sdk::Value::null();
  }
}

sdk::Value invoke(void* clip, const std::string& method, const sdk::Value* args) {
  std::vector<FlashValue> values;
  std::vector<std::string> texts;
  if (args && args->tag() == sdk::Tag::Array) {
    if (args->as_array().size() > kMaxInvokeArguments) {
      return sdk::Value::null();
    }
    values.resize(args->as_array().size());
    texts.resize(args->as_array().size());
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (!to_flash(args->as_array()[index], values[index], texts[index])) {
        return sdk::Value::null();
      }
    }
  }
  FlashValue result;
  const bool done = slot<InvokeCall>(clip, kInvoke)(clip, method.c_str(),
                                                    values.empty() ? nullptr : values.data(),
                                                    static_cast<unsigned>(values.size()), &result);
  return done ? from_flash(result) : sdk::Value::null();
}

// The depth a request names, or the clip's next free one (getNextHighestDepth).
int depth_field(const sdk::Value& body, void* parent) {
  const sdk::Value* depth = field(body, "depth");
  if (has_number(depth)) {
    return static_cast<int>(number(depth));
  }
  const sdk::Value next = invoke(parent, "getNextHighestDepth", nullptr);
  if (next.tag() == sdk::Tag::Int) {
    return static_cast<int>(next.as_int());
  }
  return next.tag() == sdk::Tag::Double ? static_cast<int>(next.as_double()) : 0;
}

// IFlashVariableObject::ObjectVisitor: VisitMembers (+88) calls its only slot with each name.
constexpr std::size_t kVisitMembers = 88;
constexpr std::size_t kMaxMembers = 512;
class MemberVisitor {
 public:
  virtual void __fastcall visit(const char* name) {
    if (name && names.size() < kMaxMembers) {
      names.push_back(sdk::Value(std::string(name)));
    }
  }
  sdk::Value::Array names;
};
using VisitCall = void(__fastcall*)(void* object, MemberVisitor* visitor);

bool drawing_method(const std::string& method) {
  return method == "clear" || method == "lineStyle" || method == "beginFill" ||
         method == "endFill" || method == "moveTo" || method == "lineTo" || method == "curveTo";
}

// "a.b.c": the path [a, b] and the member c.
bool split_member(const std::string& name, sdk::Value& path, std::string& member) {
  sdk::Value::Array parts;
  std::size_t start = 0;
  for (std::size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', start)) {
    parts.push_back(sdk::Value(name.substr(start, dot - start)));
    start = dot + 1;
  }
  member = name.substr(start);
  path = sdk::Value(std::move(parts));
  return identifier(member);
}

sdk::Value set_member_path(void* element, const std::string& name, const sdk::Value& value) {
  sdk::Value path;
  std::string member;
  ClipRef parent;
  FlashValue flash;
  std::string text;
  if (!split_member(name, path, member) || !resolve_clip(element, &path, parent) ||
      !to_flash(value, flash, text)) {
    return sdk::Value(false);
  }
  void* target = parent.get();
  return sdk::Value(slot<SetMemberCall>(target, kSetMember)(target, member.c_str(), &flash));
}

sdk::Value get_member_path(void* element, const std::string& name) {
  sdk::Value path;
  std::string member;
  ClipRef parent;
  FlashValue flash;
  if (!split_member(name, path, member) || !resolve_clip(element, &path, parent)) {
    return sdk::Value::null();
  }
  void* target = parent.get();
  if (!slot<GetValueCall>(target, kGetMemberValue)(target, member.c_str(), &flash)) {
    return sdk::Value::null();
  }
  return from_flash(flash);
}

// One NativeClip operation on the clip at body.path below the element's _root.
sdk::Value clip_op(void* element, const sdk::Value& body) {
  ClipRef clip;
  const std::string action = text_field(body, "action");
  if (!resolve_clip(element, field(body, "path"), clip)) {
    return action == "get" || action == "invoke" ? sdk::Value::null() : sdk::Value(false);
  }
  void* target = clip.get();
  if (action == "exists") {
    return sdk::Value(true);
  }
  if (action == "isolate") {
    // A screen starts empty: the library movie's own clips on its _root are hidden.
    MemberVisitor visitor;
    slot<VisitCall>(target, kVisitMembers)(target, &visitor);
    int hidden = 0;
    for (const sdk::Value& member : visitor.names) {
      const std::string& name = member.as_str();
      if (name.rfind("kcds_", 0) == 0) {
        continue;
      }
      ClipRef child;
      void* object = nullptr;
      slot<GetObjectCall>(target, kGetMemberObject)(target, name.c_str(), &object);
      child.take(object, true);
      if (object && slot<IsDisplayCall>(object, kIsDisplayObject)(object)) {
        FlashValue hide;
        hide.type = kFlashBool;
        hide.flag = false;
        slot<SetMemberCall>(object, kSetMember)(object, "_visible", &hide);
        ++hidden;
      }
    }
    return sdk::Value(static_cast<std::int64_t>(hidden));
  }
  if (action == "members") {
    MemberVisitor visitor;
    slot<VisitCall>(target, kVisitMembers)(target, &visitor);
    return sdk::Value(std::move(visitor.names));
  }
  if (action == "create" || action == "attach") {
    const std::string child = text_field(body, "child");
    const std::string symbol = text_field(body, "symbol");
    if (!identifier(child) || (action == "attach" && (symbol.empty() || symbol.size() > 256))) {
      return sdk::Value(false);
    }
    const int depth = depth_field(body, target);
    void* made = nullptr;
    bool done = false;
    if (action == "create") {
      done = slot<CreateClipCall>(target, kCreateEmptyClip)(target, &made, child.c_str(), depth);
    } else {
      done = slot<AttachCall>(target, kAttachMovie)(target, &made, symbol.c_str(), child.c_str(),
                                                    depth, nullptr);
    }
    if (made) {
      slot<ObjectRelease>(made, kObjectRelease)(made);
    }
    return sdk::Value(done && made != nullptr);
  }
  if (action == "text") {
    const std::string child = text_field(body, "child");
    if (!identifier(child)) {
      return sdk::Value(false);
    }
    const int depth = depth_field(body, target);
    sdk::Value::Array args{sdk::Value(child), sdk::Value(static_cast<std::int64_t>(depth))};
    for (const char* key : {"x", "y", "width", "height"}) {
      const sdk::Value* value = field(body, key);
      args.push_back(sdk::Value(has_number(value) ? number(value) : 0.0));
    }
    const sdk::Value list(std::move(args));
    invoke(target, "createTextField", &list);
    ClipRef made;
    void* text = nullptr;
    slot<GetObjectCall>(target, kGetMemberObject)(target, child.c_str(), &text);
    made.take(text, true);
    return sdk::Value(text != nullptr);
  }
  if (action == "set") {
    const std::string member = text_field(body, "member");
    const sdk::Value* value = field(body, "value");
    FlashValue flash;
    std::string text;
    if (!identifier(member) || !value || !to_flash(*value, flash, text)) {
      return sdk::Value(false);
    }
    return sdk::Value(slot<SetMemberCall>(target, kSetMember)(target, member.c_str(), &flash));
  }
  if (action == "get") {
    const std::string member = text_field(body, "member");
    FlashValue flash;
    if (!identifier(member) ||
        !slot<GetValueCall>(target, kGetMemberValue)(target, member.c_str(), &flash)) {
      return sdk::Value::null();
    }
    return from_flash(flash);
  }
  if (action == "invoke") {
    const std::string method = text_field(body, "method");
    if (!identifier(method)) {
      return sdk::Value::null();
    }
    return invoke(target, method, field(body, "args"));
  }
  if (action == "draw") {
    // Drawing commands run together: the pen position does not last until the next frame.
    const sdk::Value* commands = field(body, "commands");
    if (!commands || commands->tag() != sdk::Tag::Array || commands->as_array().size() > 1024) {
      return sdk::Value(false);
    }
    for (const sdk::Value& command : commands->as_array()) {
      const std::string method = text_field(command, "method");
      if (!drawing_method(method)) {
        return sdk::Value(false);
      }
      invoke(target, method, field(command, "args"));
    }
    return sdk::Value(true);
  }
  if (action == "remove") {
    invoke(target, "removeMovieClip", nullptr);
    return sdk::Value(true);
  }
  return sdk::Value(false);
}

// --- libraries --------------------------------------------------------------------------------
//
// The symbols a game movie exports (its ExportAssets tags), for NativeClip.attach. The movie is
// the element's _root._url; ICryPak reads it (+288 FOpen(path, mode, flags), +304 FReadRaw(data,
// 1, count, file), +368 FGetSize(file), +440 FClose(file)) and the game's zlib 1.2.11
// (inflateInit2_, inflate, inflateEnd) unpacks a compressed (CFX) one.
constexpr std::size_t kPakOpen = 288;
constexpr std::size_t kPakRead = 304;
constexpr std::size_t kPakSize = 368;
constexpr std::size_t kPakClose = 440;
constexpr std::size_t kMaxMovieBytes = 64u * 1024u * 1024u;
constexpr int kLibraryInstance = 47099;
constexpr std::size_t kDestroyInstance = 40;

using PakOpen = void*(__fastcall*)(void* pak, const char* path, const char* mode, unsigned flags);
using PakRead = std::size_t(__fastcall*)(void* pak, void* data, std::size_t size, std::size_t count,
                                         void* file);
using PakSize = std::size_t(__fastcall*)(void* pak, void* file);
using PakClose = int(__fastcall*)(void* pak, void* file);
using InflateInit = int(__fastcall*)(void* stream, int window_bits, const char* version,
                                     int stream_size);
using InflateCall = int(__fastcall*)(void* stream, int flush);
using InflateEnd = int(__fastcall*)(void* stream);
using DestroyCall = bool(__fastcall*)(void* element, unsigned id);

// z_stream of zlib 1.2.11 on Win64 (uLong is 32 bits).
struct ZStream {
  const std::uint8_t* next_in = nullptr;
  std::uint32_t avail_in = 0;
  std::uint32_t total_in = 0;
  std::uint8_t* next_out = nullptr;
  std::uint32_t avail_out = 0;
  std::uint32_t total_out = 0;
  const char* msg = nullptr;
  void* state = nullptr;
  void* zalloc = nullptr;
  void* zfree = nullptr;
  void* opaque = nullptr;
  std::int32_t data_type = 0;
  std::uint32_t adler = 0;
  std::uint32_t reserved = 0;
};
static_assert(sizeof(ZStream) == 88);

// element -> answer. Filled on the engine thread; the network thread answers from it while the
// engine thread is busy (a level loading), so a resource's libraries() at start does not time out.
std::mutex libraries_mutex;
std::map<std::string, sdk::Value> libraries;
// Read on the first engine frame, before resources start.
const char* const kWarmLibraries[] = {"Menu", "hud"};

const sdk::Value* cached_library(const std::string& name) {
  std::lock_guard<std::mutex> lock(libraries_mutex);
  auto cached = libraries.find(name);
  return cached == libraries.end() ? nullptr : &cached->second;  // entries are never removed
}

bool read_game_file(const std::string& path, std::vector<std::uint8_t>& out) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak || path.empty()) {
    return false;
  }
  void* file = slot<PakOpen>(pak, kPakOpen)(pak, path.c_str(), "rb", 0);
  if (!file) {
    return false;
  }
  const std::size_t size = slot<PakSize>(pak, kPakSize)(pak, file);
  bool done = false;
  if (size > 8 && size <= kMaxMovieBytes) {
    out.resize(size);
    done = slot<PakRead>(pak, kPakRead)(pak, out.data(), 1, size, file) == size;
  }
  slot<PakClose>(pak, kPakClose)(pak, file);
  return done;
}

// The movie's tag stream: after the 8-byte header, inflated when the movie is compressed.
bool movie_body(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& body) {
  const bool compressed = (file[0] == 'C' && file[1] == 'F' && file[2] == 'X') ||
                          (file[0] == 'C' && file[1] == 'W' && file[2] == 'S');
  const bool plain = (file[0] == 'G' && file[1] == 'F' && file[2] == 'X') ||
                     (file[0] == 'F' && file[1] == 'W' && file[2] == 'S');
  if (plain) {
    body.assign(file.begin() + 8, file.end());
    return true;
  }
  if (!compressed || !natives.inflate_init || !natives.inflate || !natives.inflate_end) {
    return false;
  }
  std::uint32_t length = 0;
  std::memcpy(&length, file.data() + 4, 4);
  if (length <= 8 || length > kMaxMovieBytes) {
    return false;
  }
  body.resize(length - 8);
  ZStream stream;
  stream.next_in = file.data() + 8;
  stream.avail_in = static_cast<std::uint32_t>(file.size() - 8);
  stream.next_out = body.data();
  stream.avail_out = static_cast<std::uint32_t>(body.size());
  if (reinterpret_cast<InflateInit>(natives.inflate_init)(&stream, 15, "1.2.11",
                                                          static_cast<int>(sizeof(ZStream))) != 0) {
    return false;
  }
  const int result = reinterpret_cast<InflateCall>(natives.inflate)(&stream, 4);  // Z_FINISH
  reinterpret_cast<InflateEnd>(natives.inflate_end)(&stream);
  body.resize(stream.total_out);
  return result == 1 || result == 0;  // Z_STREAM_END, or Z_OK with what fit
}

sdk::Value::Array movie_exports(const std::vector<std::uint8_t>& body) {
  sdk::Value::Array names;
  if (body.empty()) {
    return names;
  }
  const std::size_t bits = body[0] >> 3;
  std::size_t position = (5 + 4 * bits + 7) / 8 + 4;  // frame rectangle, rate and count
  while (position + 2 <= body.size()) {
    std::uint16_t code = 0;
    std::memcpy(&code, body.data() + position, 2);
    position += 2;
    const unsigned tag = code >> 6;
    std::size_t length = code & 0x3f;
    if (length == 0x3f) {
      if (position + 4 > body.size()) {
        break;
      }
      std::uint32_t long_length = 0;
      std::memcpy(&long_length, body.data() + position, 4);
      position += 4;
      length = long_length;
    }
    if (tag == 0 || position + length > body.size()) {
      break;
    }
    if (tag == 56 && length >= 2) {  // ExportAssets: count, then (id, name) pairs
      std::uint16_t count = 0;
      std::memcpy(&count, body.data() + position, 2);
      std::size_t cursor = position + 2;
      const std::size_t end = position + length;
      for (unsigned index = 0; index < count && cursor + 2 < end; ++index) {
        cursor += 2;
        const auto* text = reinterpret_cast<const char*>(body.data() + cursor);
        const std::size_t size = strnlen(text, end - cursor);
        std::string symbol(text, size);
        if (symbol.rfind("__Packages.", 0) != 0) {  // ActionScript classes, not symbols
          names.push_back(sdk::Value(std::move(symbol)));
        }
        cursor += size + 1;
      }
    }
    position += length;
  }
  return names;
}

// The GFx `file` of an element's XML (CFlashUIElement +64, a CryString), below Libs/UI as the
// game loads it; empty when unset.
constexpr std::size_t kElementFile = 64;
std::string element_file(void* base) {
  const char* file = *reinterpret_cast<const char* const*>(static_cast<char*>(base) + kElementFile);
  if (!file || !*file) {
    return std::string();
  }
  std::string path(file);
  if (path.find('/') == std::string::npos) {
    path = "Libs/UI/" + path;
  }
  return path;
}

// { name, movie, exports } of one element, or null when its movie cannot be read.
sdk::Value element_library(const std::string& name) {
  if (const sdk::Value* cached = cached_library(name)) {
    return *cached;
  }
  void* base = element_raw(name, 0);
  if (!base) {
    if (log_line) {
      log_line("NATIVEUI library %s: no such element", name.c_str());
    }
    return sdk::Value::null();
  }
  void* element = element_raw(name, kLibraryInstance);
  std::string movie;
  if (element) {
    ClipRef root;
    if (resolve_clip(element, nullptr, root)) {
      FlashValue url;
      void* target = root.get();
      if (slot<GetValueCall>(target, kGetMemberValue)(target, "_url", &url)) {
        const sdk::Value text = from_flash(url);
        movie = text.tag() == sdk::Tag::Str ? text.as_str() : std::string();
      }
    }
  }
  if (element) {
    slot<DestroyCall>(base, kDestroyInstance)(base, kLibraryInstance);
  }
  if (movie.empty()) {
    // Before the game's UI has loaded a movie, the element's GFx file from its XML (+64).
    movie = element_file(base);
  }
  for (std::size_t slash = movie.find("//"); slash != std::string::npos; slash = movie.find("//")) {
    movie.erase(slash, 1);
  }
  std::vector<std::uint8_t> file;
  std::vector<std::uint8_t> body;
  if (movie.empty() || !read_game_file(movie, file) || !movie_body(file, body)) {
    if (log_line) {
      log_line("NATIVEUI library %s: movie '%s' not read (instance %d, %zu bytes)", name.c_str(),
               movie.c_str(), element ? 1 : 0, file.size());
    }
    return sdk::Value::null();
  }
  sdk::Value answer(sdk::Value::Map{{"name", sdk::Value(name)},
                                    {"movie", sdk::Value(movie)},
                                    {"exports", sdk::Value(movie_exports(body))}});
  std::lock_guard<std::mutex> lock(libraries_mutex);
  libraries.emplace(name, answer);
  return answer;
}

// --- files a screen loads ---------------------------------------------------------------------
//
// NativeClip.load and createScreen({ movie, assets }) load files a resource ships. The runtime
// sends them as { op: "nativeFile", resource, path, offset, total, data (base64) }; complete files
// are kept in memory only (write_shipped), never written to disk. The movie loads them
// with loadMovie("/%USER%/kcds_ui/..."), which the game's URL builder hands to the movie file
// opener as is (hooked_file_open serves it from memory). %USER%/kcds_ui is resolved with ICryPak +8
// AdjustFileName, as the screenshot code resolves %USER%/ScreenShots, only to remove the plain
// files an older client left there.
constexpr std::size_t kPakAdjust = 8;
constexpr unsigned kAdjustForWriting = 0x810000;
constexpr std::size_t kMaxShippedFile = 32u * 1024u * 1024u;
// The streamed files one resource mounts at most.
constexpr std::size_t kMaxStreamFiles = 4096;
using PakAdjust = const char*(__fastcall*)(void* pak, const char* path, char* out, unsigned flags);

struct PendingFile {
  std::vector<std::uint8_t> bytes;
  std::size_t received = 0;
};
std::mutex files_mutex;
std::map<std::string, PendingFile> pending_files;  // "<resource>/<path>"

// A resource name or a relative path of plain segments (no drive, no "..", no backslashes).
bool safe_relative(const std::string& path) {
  if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/') {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find('/', start), path.size());
    const std::string part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    for (const char character : part) {
      const bool letter =
          (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
      const bool digit = character >= '0' && character <= '9';
      if (!letter && !digit && character != '_' && character != '-' && character != '.') {
        return false;
      }
    }
    start = end + 1;
  }
  return true;
}

using kcds::runtime::files::decode_base64;

std::wstring widen(const std::string& text) {
  std::wstring out(text.size(), L'\0');
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         out.data(), static_cast<int>(out.size()));
  out.resize(static_cast<std::size_t>(length > 0 ? length : 0));
  return out;
}

// %USER%/kcds_ui of this game instance as a real folder; empty when ICryPak cannot say.
std::string shipped_root() {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak) {
    return {};
  }
  char resolved[2048] = {};
  const char* path =
      slot<PakAdjust>(pak, kPakAdjust)(pak, "%USER%/kcds_ui", resolved, kAdjustForWriting);
  return path && *path ? std::string(path) : std::string();
}

// Writes key (a relative path with '/') below the real folder root, creating its folders.
bool write_below(const std::wstring& root, const std::string& key,
                 const std::vector<std::uint8_t>& bytes) {
  if (root.empty()) {
    return false;
  }
  std::wstring folder = root;
  CreateDirectoryW(folder.c_str(), nullptr);
  const std::wstring relative = widen(key);
  std::size_t start = 0;
  for (std::size_t slash = relative.find(L'/'); slash != std::wstring::npos;
       slash = relative.find(L'/', start)) {
    folder += L"\\" + relative.substr(start, slash - start);
    CreateDirectoryW(folder.c_str(), nullptr);
    start = slash + 1;
  }
  const std::wstring file = folder + L"\\" + relative.substr(start);
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const bool done =
      WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
      written == bytes.size();
  CloseHandle(handle);
  return done;
}

// The files resources sent, and the client's own (_kcds/...), by key (<resource>/<path>): kept in
// memory only (a resource's files are sealed on the way and never lie plain on disk).
// A movie opens one through a memory pak mounted at %USER%/kcds_ui the first time it asks
// (hooked_file_open); the asset mount, the pictures and the icons read them here (read_shipped).
std::mutex shipped_mutex;
std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> shipped_files;
bool stale_removed = false;  // engine thread

// Engine thread, once a run as soon as ICryPak can say where %USER% is: the plain copies an older
// KCDS wrote to %USER%/kcds_ui go.
void remove_stale_shipped() {
  if (stale_removed) {
    return;
  }
  const std::string root = shipped_root();
  if (root.empty()) {
    return;
  }
  stale_removed = true;
  std::error_code error;
  const std::uintmax_t removed =
      std::filesystem::remove_all(std::filesystem::path(widen(root)), error);
  if (log_line) {
    log_line(
        "NATIVEUI plain files of older runs: %llu removed from %s%s",
        static_cast<unsigned long long>(removed == static_cast<std::uintmax_t>(-1) ? 0 : removed),
        root.c_str(), error ? " (not all)" : "");
  }
}

bool write_shipped(const std::string& key, const std::vector<std::uint8_t>& bytes) {
  std::lock_guard<std::mutex> lock(shipped_mutex);
  auto& stored = shipped_files[key];
  // The same bytes sent again (a rejoin, a screen opened again) keep their entry, so a movie's pak
  // for them is mounted once.
  if (!stored || *stored != bytes) {
    stored = std::make_shared<const std::vector<std::uint8_t>>(bytes);
  }
  return true;
}

std::shared_ptr<const std::vector<std::uint8_t>> shipped_file(const std::string& key) {
  std::lock_guard<std::mutex> lock(shipped_mutex);
  const auto found = shipped_files.find(key);
  return found == shipped_files.end() ? nullptr : found->second;
}

// --- pictures for the book screen (Book.image) ------------------------------------------------
//
// The book screen loads <img src='name'> as img://Libs/UI/Textures/Books/Unique/<name>_ui.dds
// (the UI picture loader, is asked for that), and the renderer loads it from the
// mounted archives only. A shipped .dds is therefore stored in a small uncompressed zip
// ("<path>.pak" in %TEMP%\kcds-book, see write_pack) and mounted with ICryPak OpenPack
// (vtable +48, bindingRoot, path, flags; the game's own OpenCachePak, mounts
// "%USER%/cache/..." the same way) at Libs/UI/Textures/Books/Unique/ as a lookup sees it
// (lookup_root), where the entry is <name>_ui.dds (book_picture_name).
constexpr std::size_t kPakOpenPack = 48;
constexpr unsigned kOpenPackFlags = 268502020;  // as OpenCachePak passes them
constexpr const char* kTextureRoot = "Libs/UI/Textures/Books/Unique/";
// An item type's own icon: the inventory loads
// img://Libs/UI/Textures/Icons/Items/<icon>_icon.dds, the icon being the same name.
constexpr const char* kItemIconRoot = "Libs/UI/Textures/Icons/Items/";
// A blip's own icon: the map (ApseMap.gfx) loads a mark's icon as
// img://Libs/UI/Textures/Icons/Map/<name>_icon.dds, with <name>_undiscovered_icon.dds and the small
// <name>_sh_icon.dds beside it (the game's are 64, 64 and 32 pixels). The small one is the game's
// shadow, drawn beside the icon: ours is empty (the picture there showed twice).
constexpr const char* kMapIconRoot = "Libs/UI/Textures/Icons/Map/";
struct MapIconFile {
  const char* suffix;
  std::uint32_t size;
  bool empty;
};
constexpr MapIconFile kMapIconFiles[] = {
    {"_icon.dds", 64, false}, {"_undiscovered_icon.dds", 64, false}, {"_sh_icon.dds", 32, true}};

// Mounts a blip's own icon from a shipped .dds; engine thread. Every file is a pack of its own.
void mount_map_icon(void* pak, const std::string& resource, const std::string& path);
// The map icons the game's file system reads (their names), engine thread only.
std::set<std::string> mounted_map_icons;

// The name a book's <img src> gives for a resource's picture, as Client/resources/native_ui.cjs
// makes it: kcds_<resource>_<path without .dds>, lower case, other characters as '_'.
std::string book_picture_name(const std::string& resource, const std::string& path) {
  std::string stem = "kcds_" + resource + "_" + path;
  if (stem.size() >= 4 && _stricmp(stem.c_str() + stem.size() - 4, ".dds") == 0) {
    stem.resize(stem.size() - 4);
  }
  for (char& character : stem) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    } else if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9')) {
      character = '_';
    }
  }
  return stem;
}
using OpenPack = bool(__fastcall*)(void* pak, const char* binding_root, const char* name,
                                   unsigned flags, void* memory, void* full_path);

struct MountRequest {
  std::string resource;
  std::string path;
  bool item_icon = false;  // an item type's icon, else a book picture
  bool map_icon = false;   // a blip's own map icon
};
std::mutex mount_mutex;
std::vector<MountRequest> mount_requests;
// A resource's streamed game files (its stream/ folder) to mount as packs, and the
// paths the game's file system reads afterwards, to tell the runtime.
struct StreamRequest {
  std::string resource;
  std::vector<std::string> files;  // "stream/<top>/kcds/<resource>/..." or "replace/<path>"
  bool replace = false;            // the files stand in for the game's own at their paths
};
struct StreamMounted {
  std::string resource;
  std::vector<std::string> paths;  // "<top>/kcds/<resource>/..."
};
std::vector<StreamRequest> stream_requests;
std::vector<StreamMounted> streams_mounted;
std::set<std::string> mounted_packs;  // "<resource>/<path>", engine thread only

using kcds::runtime::files::stored_zip;
using kcds::runtime::files::stored_zip_entries;

bool read_shipped(const std::string& key, std::vector<std::uint8_t>& out) {
  const auto bytes = shipped_file(key);
  if (!bytes || bytes->empty()) {
    return false;
  }
  out = *bytes;
  return true;
}

// The binding root a pack must have for ICryPak to find its files. A lookup adjusts the asked
// path without flags (CCryPak::AdjustFileName: backslashes, lower case, the game
// data folder put in front of a relative path) and compares the pack's root byte by byte
// while OpenPack adjusts the root as a real path (0x50000) without that
// folder. Adjusting the root here the way a lookup does keeps both in step.
std::string lookup_root(void* pak, const std::string& folder) {
  char resolved[2048] = {};
  const char* path = slot<PakAdjust>(pak, kPakAdjust)(pak, folder.c_str(), resolved, 0);
  if (!path || !*path) {
    return {};
  }
  std::string root(path);
  if (root.back() != '\\' && root.back() != '/') {
    root += '\\';
  }
  return root;
}

// The Windows temp folder for picture packs (%TEMP%\kcds-book), which Windows clears by itself.
// OpenPack takes the pack's real path (the name is adjusted as a real path, 0x10000, so an
// absolute path is opened as it is); empty when the path is not plain ASCII, since ICryPak takes
// narrow paths.
std::string temp_pack_root() {
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD length = GetTempPathW(MAX_PATH + 1, buffer);
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  std::string root;
  for (DWORD index = 0; index < length; ++index) {
    if (buffer[index] > 0x7F) {
      return {};
    }
    root += static_cast<char>(buffer[index]);
  }
  return root + "kcds-book";
}

bool plain_ascii(const std::string& text) {
  for (char character : text) {
    if (static_cast<unsigned char>(character) > 0x7F) {
      return false;
    }
  }
  return true;
}

// Gives a pack the name OpenPack opens it by: from memory, or, without the memory paks, written to
// the temp folder; empty when neither can be done.
std::string write_pack(const std::string& key, const std::vector<std::uint8_t>& bytes,
                       bool own_name = false) {
  // With the memory paks in (assets/memory_paks) nothing is written: the pack is
  // served from memory under a name of its own, and no plain game file of a resource lies on
  // disk.
  if (kcds::runtime::assets::memory_paks::available()) {
    return kcds::runtime::assets::memory_paks::provide_virtual(key, bytes);
  }
  const std::string temp = temp_pack_root();
  // Another game on this PC may hold the pack of that key open (ICryPak keeps it): this one's
  // then has the process id in its name.
  const std::string own = key + "." + std::to_string(GetCurrentProcessId()) + ".pak";
  std::vector<std::string> names;
  if (!own_name) {
    names.push_back(key + ".pak");
  }
  names.push_back(own);
  for (const std::string& pack : names) {
    if (!temp.empty() && plain_ascii(pack) && write_below(widen(temp), pack, bytes)) {
      std::string name = temp + "\\" + pack;
      std::replace(name.begin(), name.end(), '/', '\\');
      return name;
    }
  }
  return {};
}

bool mount_pack(void* pak, const std::string& binding, const std::string& key,
                const std::vector<std::uint8_t>& zip) {
  if (binding.empty()) {
    return false;
  }
  // Another game on this PC may be writing the shared pack right now: then this process's own.
  for (const bool own_name : {false, true}) {
    const std::string pack = write_pack(key, zip, own_name);
    if (!pack.empty() && slot<OpenPack>(pak, kPakOpenPack)(pak, binding.c_str(), pack.c_str(),
                                                           kOpenPackFlags, nullptr, nullptr)) {
      return true;
    }
  }
  return false;
}

// The movie loader opens files with ICryPak paths resolved in the paks and the game folder only
// (CryGFxFileOpener::OpenFile builds a GFileCryPak without path flags). A movie asks for a
// shipped file as %USER%/kcds_ui/<key>: the hook mounts the file's bytes in a pak of its own at
// kcds-shipped/<number>/ and lets the game's opener read it from there (a new number for every
// new version of the file, so a resent file is read as sent). OpenPack takes the pak lock itself
// (CCryPak), so it may run on the loader's thread.
constexpr const char* kShippedPrefix = "%USER%/kcds_ui/";
constexpr const char* kShippedMount = "kcds-shipped/";
using FileOpen = void*(__fastcall*)(void* opener, const char* path, int flags);
FileOpen original_file_open = nullptr;

struct MountedShipped {
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  std::string path;  // where the game reads it: kcds-shipped/<number>/<key>
};
// One movie open mounts at a time.
std::mutex shipped_mount_mutex;
std::unordered_map<std::string, MountedShipped> shipped_paths;
unsigned shipped_mount_count = 0;

std::string mount_movie_file(void* pak, const std::string& key) {
  const auto bytes = shipped_file(key);
  if (!bytes) {
    return {};
  }
  std::lock_guard<std::mutex> lock(shipped_mount_mutex);
  const auto found = shipped_paths.find(key);
  if (found != shipped_paths.end() && found->second.bytes == bytes) {
    return found->second.path;
  }
  const std::string number = std::to_string(++shipped_mount_count);
  const std::string folder = std::string(kShippedMount) + number + "/";
  const std::string binding = lookup_root(pak, folder);
  const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries{{key, *bytes}};
  const bool mounted = mount_pack(pak, binding, "_shipped/" + number, stored_zip_entries(entries));
  if (log_line) {
    log_line("NATIVEUI movie file %s %s at %s", key.c_str(), mounted ? "mounted" : "not mounted",
             binding.c_str());
  }
  if (!mounted) {
    return {};
  }
  shipped_paths[key] = MountedShipped{bytes, folder + key};
  return folder + key;
}

void* __fastcall hooked_file_open(void* opener, const char* path, int flags) {
  const std::size_t prefix = std::strlen(kShippedPrefix);
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!path || std::strncmp(path, kShippedPrefix, prefix) != 0 || (flags & 0xFFFFFFDE) != 0 ||
      !pak || !safe_relative(path + prefix)) {
    return original_file_open(opener, path, flags);
  }
  const std::string mounted = mount_movie_file(pak, path + prefix);
  return original_file_open(opener, mounted.empty() ? path : mounted.c_str(), flags);
}

bool install_file_hook() {
  if (!natives.file_open) {
    return false;
  }
  return MH_CreateHook(natives.file_open, reinterpret_cast<void*>(hooked_file_open),
                       reinterpret_cast<void**>(&original_file_open)) == MH_OK &&
         MH_EnableHook(natives.file_open) == MH_OK;
}

// Packs and mounts the shipped pictures asked for; engine thread.
// Whether the game's file system has a file at this path (in a pack or on disk).
bool game_file_exists(void* pak, const std::string& path) {
  void* file = slot<PakOpen>(pak, kPakOpen)(pak, path.c_str(), "rb", 0);
  if (!file) {
    return false;
  }
  slot<PakClose>(pak, kPakClose)(pak, file);
  return true;
}

// The game path of a streamed file: "stream/<top>/kcds/<resource>/<rest>" without "stream/";
// empty for a path outside the resource's own folder (a streamed file never stands in for the
// game's own or another resource's).
std::string stream_path(const std::string& resource, const std::string& file) {
  if (file.rfind("stream/", 0) != 0 || !safe_relative(file)) {
    return {};
  }
  const std::string path = file.substr(7);
  const std::size_t top = path.find('/');
  if (top == std::string::npos || top == 0) {
    return {};
  }
  const std::string own = "kcds/" + resource + "/";
  if (path.size() <= top + 1 + own.size() ||
      _strnicmp(path.c_str() + top + 1, own.c_str(), own.size()) != 0) {
    return {};
  }
  return path;
}

// The game path a replacing file stands in for: "replace/<path>" without "replace/"; empty for a
// path that is not a plain relative one with a folder.
std::string replace_path(const std::string& file) {
  if (file.rfind("replace/", 0) != 0 || !safe_relative(file)) {
    return {};
  }
  const std::string path = file.substr(8);
  const std::size_t top = path.find('/');
  return top == std::string::npos || top == 0 ? std::string() : path;
}

// Whether the game's file system reads exactly these bytes at the path.
bool game_file_is(void* pak, const std::string& path, const std::vector<std::uint8_t>& bytes) {
  void* file = slot<PakOpen>(pak, kPakOpen)(pak, path.c_str(), "rb", 0);
  if (!file) {
    return false;
  }
  const std::size_t size = slot<PakSize>(pak, kPakSize)(pak, file);
  bool same = size == bytes.size();
  if (same && size > 0) {
    std::vector<std::uint8_t> read(size);
    same = slot<PakRead>(pak, kPakRead)(pak, read.data(), 1, size, file) == size && read == bytes;
  }
  slot<PakClose>(pak, kPakClose)(pak, file);
  return same;
}

// Mounts a resource's streamed (or replacing) files as packs, one per top folder at that
// folder's root; engine thread. Answers the paths the game's file system reads (for replacing
// files, those it reads as the resource's).
std::vector<std::string> mount_stream(void* pak, const StreamRequest& request) {
  std::map<std::string, std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> folders;
  std::vector<std::string> paths;
  std::map<std::string, std::vector<std::uint8_t>> contents;
  for (const std::string& file : request.files) {
    const std::string path =
        request.replace ? replace_path(file) : stream_path(request.resource, file);
    if (path.empty()) {
      if (log_line) {
        log_line("NATIVEUI stream %s/%s refused: not under <folder>/kcds/%s/",
                 request.resource.c_str(), file.c_str(), request.resource.c_str());
      }
      continue;
    }
    std::vector<std::uint8_t> data;
    if (!read_shipped(request.resource + "/" + file, data)) {
      continue;
    }
    const std::size_t top = path.find('/');
    if (request.replace) {
      contents[path] = data;
    }
    folders[path.substr(0, top + 1)].emplace_back(path.substr(top + 1), std::move(data));
    paths.push_back(path);
  }
  for (auto& [folder, entries] : folders) {
    const std::string key = request.resource + (request.replace ? "/replace-" : "/stream-") +
                            folder.substr(0, folder.size() - 1);
    if (mounted_packs.count(key) != 0) {
      continue;
    }
    const std::string binding = lookup_root(pak, folder);
    const std::string name =
        binding.empty() ? std::string() : write_pack(key, stored_zip_entries(entries));
    const bool mounted =
        !name.empty() && slot<OpenPack>(pak, kPakOpenPack)(pak, binding.c_str(), name.c_str(),
                                                           kOpenPackFlags, nullptr, nullptr);
    if (mounted) {
      mounted_packs.insert(key);
    }
    if (log_line) {
      log_line("NATIVEUI %s %s %s: %zu files at %s", request.replace ? "replace" : "stream",
               request.resource.c_str(), mounted ? "mounted" : "not mounted", entries.size(),
               binding.c_str());
    }
  }
  std::vector<std::string> read;
  for (const std::string& path : paths) {
    const bool reads =
        request.replace ? game_file_is(pak, path, contents[path]) : game_file_exists(pak, path);
    if (reads) {
      read.push_back(path);
      // A replacing file the game reads may be a patch of a recipe table.
      if (request.replace && natives.table_patch) {
        const std::vector<std::uint8_t>& data = contents[path];
        natives.table_patch(path, std::string(data.begin(), data.end()));
      }
    }
  }
  if (request.replace && log_line) {
    log_line("NATIVEUI replace %s: the game reads %zu of %zu files as the resource's",
             request.resource.c_str(), read.size(), paths.size());
  }
  return read;
}

void mount_textures() {
  std::vector<MountRequest> taken;
  std::vector<StreamRequest> streams;
  {
    std::lock_guard<std::mutex> lock(mount_mutex);
    taken.swap(mount_requests);
    streams.swap(stream_requests);
  }
  void* pak = natives.pak ? *natives.pak : nullptr;
  for (const StreamRequest& stream : streams) {
    if (!pak || !safe_relative(stream.resource) || stream.resource.find('/') != std::string::npos) {
      continue;
    }
    StreamMounted mounted{stream.resource, mount_stream(pak, stream)};
    // Only new files count for Assets.has: a replaced file is the game's own path.
    if (stream.replace) {
      continue;
    }
    std::lock_guard<std::mutex> lock(mount_mutex);
    streams_mounted.push_back(std::move(mounted));
  }
  for (const MountRequest& request : taken) {
    const std::string key =
        request.resource + "/" + request.path + (request.item_icon ? ".icon" : "");
    if (!pak || !safe_relative(request.resource) || !safe_relative(request.path)) {
      continue;
    }
    if (request.map_icon) {
      mount_map_icon(pak, request.resource, request.path);
      continue;
    }
    const char* root = request.item_icon ? kItemIconRoot : kTextureRoot;
    const std::string entry = book_picture_name(request.resource, request.path) +
                              (request.item_icon ? "_icon.dds" : "_ui.dds");
    std::vector<std::uint8_t> data;
    std::string binding;
    std::string name;
    bool mounted = false;
    if (mounted_packs.count(key) != 0) {
      // ICryPak keeps the pack open (its file cannot be rewritten) and opens it once.
      mounted = true;
    } else if (read_shipped(request.resource + "/" + request.path, data)) {
      // An item icon is drawn at its own size: it is fitted to the game's 64 x 64.
      if (request.item_icon) {
        data = kcd2s::integration::fit_item_icon(data);
        if (data.empty() && log_line) {
          log_line("NATIVEUI item icon %s refused: neither 64 x 64 nor an uncompressed 32-bit .dds",
                   key.c_str());
        }
      }
      binding = data.empty() ? std::string() : lookup_root(pak, root);
      if (!binding.empty()) {
        name = write_pack(key, stored_zip(entry, data));
      }
      mounted =
          !name.empty() && slot<OpenPack>(pak, kPakOpenPack)(pak, binding.c_str(), name.c_str(),
                                                             kOpenPackFlags, nullptr, nullptr);
      if (mounted) {
        mounted_packs.insert(key);
      }
    }
    // Whether the game's file system now finds the picture at its mounted path.
    std::vector<std::uint8_t> check;
    const std::string mounted_path = std::string(root) + entry;
    const bool readable = mounted && read_game_file(mounted_path, check);
    if (log_line) {
      log_line("NATIVEUI texture %s %s at %s, root %s, pack %s (readable %u, %zu bytes)",
               key.c_str(), mounted ? "mounted" : "not mounted", mounted_path.c_str(),
               binding.c_str(), name.c_str(), unsigned(readable), check.size());
    }
  }
}

void mount_map_icon(void* pak, const std::string& resource, const std::string& path) {
  const std::string name = book_picture_name(resource, path);
  std::vector<std::uint8_t> source;
  bool have_source = false;
  for (const MapIconFile& file : kMapIconFiles) {
    const std::string key = resource + "/" + path + file.suffix;
    if (mounted_packs.count(key) != 0) {
      continue;
    }
    if (!have_source && !read_shipped(resource + "/" + path, source)) {
      if (log_line) {
        log_line("NATIVEUI map icon %s/%s is not on disk (not sent yet)", resource.c_str(),
                 path.c_str());
      }
      return;
    }
    have_source = true;
    const std::vector<std::uint8_t> data = file.empty
                                               ? kcd2s::integration::transparent_icon(file.size)
                                               : kcd2s::integration::map_icon(source);
    if (data.empty()) {
      if (log_line) {
        log_line(
            "NATIVEUI map icon %s/%s refused: neither %u x %u nor an uncompressed 32-bit "
            ".dds",
            resource.c_str(), path.c_str(), file.size, file.size);
      }
      return;
    }
    const std::string entry = name + file.suffix;
    const std::string binding = lookup_root(pak, kMapIconRoot);
    const std::string pack =
        binding.empty() ? std::string() : write_pack(key, stored_zip(entry, data));
    const bool mounted =
        !pack.empty() && slot<OpenPack>(pak, kPakOpenPack)(pak, binding.c_str(), pack.c_str(),
                                                           kOpenPackFlags, nullptr, nullptr);
    if (mounted) {
      mounted_packs.insert(key);
    }
    std::vector<std::uint8_t> check;
    const bool readable = mounted && read_game_file(std::string(kMapIconRoot) + entry, check);
    if (readable && !file.empty) {
      mounted_map_icons.insert(name);
    }
    if (log_line) {
      log_line("NATIVEUI map icon %s %s at %s%s (readable %u)", key.c_str(),
               mounted ? "mounted" : "not mounted", kMapIconRoot, entry.c_str(),
               unsigned(readable));
    }
  }
}

// Mounts a shipped file as <root><entry>, as mount_textures mounts a picture; true when the game's
// file system reads it there. Engine thread.
bool mount_shipped_entry(const std::string& resource, const std::string& path,
                         const std::string& root, const std::string& entry) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak || !safe_relative(resource) || resource.find('/') != std::string::npos ||
      !safe_relative(path) || !safe_relative(entry) || entry.find('/') != std::string::npos) {
    return false;
  }
  const std::string key = resource + "/" + path + ".mounted";
  if (mounted_packs.count(key) == 0) {
    std::vector<std::uint8_t> data;
    if (!read_shipped(resource + "/" + path, data)) {
      if (log_line) {
        log_line("NATIVEUI shipped %s/%s is not on disk (not sent yet)", resource.c_str(),
                 path.c_str());
      }
      return false;
    }
    const std::string binding = lookup_root(pak, root);
    const std::string name =
        binding.empty() ? std::string() : write_pack(key, stored_zip(entry, data));
    const bool mounted =
        !name.empty() && slot<OpenPack>(pak, kPakOpenPack)(pak, binding.c_str(), name.c_str(),
                                                           kOpenPackFlags, nullptr, nullptr);
    if (log_line) {
      log_line("NATIVEUI shipped %s/%s %s at %s%s, root %s, pack %s", resource.c_str(),
               path.c_str(), mounted ? "mounted" : "not mounted", root.c_str(), entry.c_str(),
               binding.c_str(), name.c_str());
    }
    if (!mounted) {
      return false;
    }
    mounted_packs.insert(key);
  }
  std::vector<std::uint8_t> check;
  return read_game_file(root + entry, check);
}

void receive_file(const sdk::Value& message) {
  const std::string resource = text_field(message, "resource");
  const std::string path = text_field(message, "path");
  const sdk::Value* offset = field(message, "offset");
  const sdk::Value* total = field(message, "total");
  if (!safe_relative(resource) || resource.find('/') != std::string::npos || !safe_relative(path)) {
    return;
  }
  // A big file the runtime left on the disk, when it is one of this server's
  // files: read from the download cache into the shipped files in memory, as every other file is
  // kept (no plain copy beside the game).
  const std::string cached = text_field(message, "cached");
  if (!cached.empty()) {
    const std::string key = resource + "/" + path;
    const std::filesystem::path source = session::cached_resource_file(cached);
    std::vector<std::uint8_t> bytes;
    bool kept = false;
    if (!source.empty()) {
      std::ifstream stream(source, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
      kept = stream.good() || stream.eof() ? write_shipped(key, bytes) : false;
    }
    if (log_line) {
      log_line("NATIVEUI file %s %s from the cache (%zu bytes)", key.c_str(),
               kept ? "kept" : "not kept", bytes.size());
    }
    return;
  }
  if (!has_number(offset) || !has_number(total)) {
    return;
  }
  const double size = number(total);
  const double at = number(offset);
  if (size < 0 || size > kMaxShippedFile || at < 0 || at > size) {
    return;
  }
  std::vector<std::uint8_t> chunk;
  if (!decode_base64(text_field(message, "data"), chunk)) {
    return;
  }
  const std::string key = resource + "/" + path;
  std::vector<std::uint8_t> complete;
  {
    std::lock_guard<std::mutex> lock(files_mutex);
    PendingFile& file = pending_files[key];
    if (at == 0) {
      file.bytes.assign(static_cast<std::size_t>(size), 0);
      file.received = 0;
    }
    const std::size_t begin = static_cast<std::size_t>(at);
    if (file.bytes.size() != static_cast<std::size_t>(size) ||
        begin + chunk.size() > file.bytes.size()) {
      pending_files.erase(key);
      return;
    }
    std::copy(chunk.begin(), chunk.end(), file.bytes.begin() + static_cast<std::ptrdiff_t>(begin));
    file.received += chunk.size();
    if (file.received < file.bytes.size()) {
      return;
    }
    complete = std::move(file.bytes);
    pending_files.erase(key);
  }
  const bool written = write_shipped(key, complete);
  if (log_line) {
    log_line("NATIVEUI file %s %s (%zu bytes)", key.c_str(), written ? "written" : "not written",
             complete.size());
  }
}

// Whether the element's movie has a member of this name on its _root (a function it defines).
bool movie_defines(void* element, const char* member) {
  ClipRef root;
  if (!resolve_clip(element, nullptr, root)) {
    return false;
  }
  FlashValue value;
  void* target = root.get();
  return slot<GetValueCall>(target, kGetMemberValue)(target, member, &value) &&
         value.type != kFlashUndefined && value.type != kFlashNull;
}

// The resources' native screens (createScreen's element instances) shown now, and those a picture
// of the world hid for its frames (hide_screens_for_picture). Engine thread only.
std::set<std::pair<std::string, int>> shown_screens;
std::vector<std::pair<std::string, int>> picture_hidden;

void remember_screen(const std::string& name, int instance, const std::string& action) {
  if (action == "show") {
    shown_screens.insert({name, instance});
  } else if (action == "hide" || action == "unload") {
    shown_screens.erase({name, instance});
  }
}

sdk::Value element_state(void* element, const std::string& action, const sdk::Value& body) {
  if (action == "show") {
    slot<ElementFlag>(element, kSetVisible)(element, true);
  } else if (action == "hide") {
    slot<ElementFlag>(element, kSetVisible)(element, false);
  } else if (action == "askHide") {
    // RequestHide calls the movie's own cry_requestHide (its hide transition, which ends with
    // fscommand cry_hideElement); a movie without one does nothing, so it did not take it.
    if (!movie_defines(element, "cry_requestHide")) {
      return sdk::Value(false);
    }
    slot<ElementCall>(element, kRequestHide)(element);
  } else if (action == "reload") {
    slot<ElementFlag>(element, kReload)(element, false);
  } else if (action == "unload") {
    slot<ElementFlag>(element, kUnload)(element, false);
  } else if (action == "noCursor") {
    // A screen's instance does not hold the game's mouse cursor (the element's cursor flag,
    // which counts as an open game menu); the pointer comes with the screen's focus instead.
    slot<ElementSetFlag>(element, kSetFlag)(element, kCursorFlag, false);
  } else if (action == "layer") {
    const sdk::Value* layer = field(body, "layer");
    if (!layer || layer->tag() != sdk::Tag::Int) {
      return sdk::Value(false);
    }
    slot<ElementSetLayer>(element, kSetLayer)(element, static_cast<int>(layer->as_int()));
  } else {
    return sdk::Value(false);
  }
  return sdk::Value(true);
}

sdk::Value answer(const sdk::Value& body) {
  const std::string op = text_field(body, "op");
  const std::string name = text_field(body, "name");
  const sdk::Value* instance = field(body, "instance");
  const int id =
      instance && instance->tag() == sdk::Tag::Int ? static_cast<int>(instance->as_int()) : 0;
  if (op == "elementInfo") {
    void* any = element_raw(name, id);
    return sdk::Value(sdk::Value::Map{
        {"valid", sdk::Value(any != nullptr)},
        {"visible",
         sdk::Value(any != nullptr && slot<bool(__fastcall*)(void*)>(any, kIsVisible)(any))}});
  }
  if (op == "elementLibrary") {
    return element_library(name);
  }
  if (op == "elementState") {
    void* any = element_raw(name, id);
    const std::string action = text_field(body, "action");
    if (id >= kScreenInstanceBase) {
      remember_screen(name, id, action);
    }
    return any ? element_state(any, action, body) : sdk::Value(false);
  }
  void* element = element_of(name, id);
  if (!element) {
    // An unloaded element returns null; reading never triggers a load.
    return op == "elementGetVariable" || op == "elementGetArray" || op == "elementGetClip" ||
                   op == "elementCall"
               ? sdk::Value::null()
               : sdk::Value(false);
  }
  if (op == "elementCall") {
    return call_function(element, body);
  }
  if (op == "elementSetVariable") {
    return set_variable(element, body);
  }
  if (op == "elementGetVariable") {
    return get_variable(element, body);
  }
  if (op == "elementSetArray") {
    return set_array(element, body);
  }
  if (op == "elementGetArray") {
    return get_array(element, body);
  }
  if (op == "elementSetClip") {
    return set_clip(element, body);
  }
  if (op == "elementGetClip") {
    return get_clip(element, body);
  }
  if (op == "elementClip") {
    return clip_op(element, body);
  }
  if (op == "elementGotoClip") {
    return goto_clip(element, body);
  }
  return sdk::Value::null();
}

// Adds or drops one resource's interest in an element event.
void listen(const sdk::Value& body, bool on) {
  const std::string name = text_field(body, "name");
  const std::string event = text_field(body, "event");
  const sdk::Value* instance = field(body, "instance");
  const int id =
      instance && instance->tag() == sdk::Tag::Int ? static_cast<int>(instance->as_int()) : 0;
  if (name.empty() || event.empty()) {
    return;
  }
  auto key = std::make_pair(name, id);
  auto found = listening.find(key);
  if (on) {
    if (found == listening.end()) {
      void* element = element_of(name, id);
      if (!element) {
        return;
      }
      Listening entry;
      entry.listener = std::make_unique<ElementListener>(name, id);
      entry.element = element;
      slot<ListenerCall>(element, kAddListener)(element, entry.listener.get(), "KCDS NativeUI");
      found = listening.emplace(key, std::move(entry)).first;
    }
    ++found->second.events[event];
    return;
  }
  if (found == listening.end()) {
    return;
  }
  auto count = found->second.events.find(event);
  if (count != found->second.events.end() && --count->second <= 0) {
    found->second.events.erase(count);
  }
  if (found->second.events.empty()) {
    void* element = element_of(name, id);
    if (element == found->second.element) {
      slot<ListenerCall>(element, kRemoveListener)(element, found->second.listener.get(), nullptr);
    }
    // A listener the game may still hold is kept alive; it is tiny and forwards nothing.
    found->second.listener.release();
    listening.erase(found);
  }
}

bool listened(const std::string& name, int instance, const std::string& event) {
  auto found = listening.find(std::make_pair(name, instance));
  // "*" is a screen's listener for every event its movie raises (NativeScreen "event").
  return found != listening.end() &&
         (found->second.events.count(event) != 0 || found->second.events.count("*") != 0);
}

bool page_disabled(std::uint8_t page) {
  const int held = held_page.load();
  if (held >= 0 && page != held) {
    return true;
  }
  return page < 32 && (disabled_pages.load() & (1u << page)) != 0;
}

bool __fastcall hooked_menu_open(void* menu, std::uint8_t page, void* context) {
  menu_object.store(menu);
  if (!page_disabled(page)) {
    return original_menu_open(menu, page, context);
  }
  reinterpret_cast<FunctionFree>(natives.function_free)(static_cast<std::uint8_t*>(context) +
                                                        kContextFunction);
  log_line("NATIVEUI menu page %u refused", static_cast<unsigned>(page));
  return false;
}

std::int64_t __fastcall hooked_menu_move(void* menu, std::uint8_t page, std::uint8_t a,
                                         std::uint8_t b) {
  menu_object.store(menu);
  if (page_disabled(page)) {
    log_line("NATIVEUI menu page %u refused", static_cast<unsigned>(page));
    return 0;
  }
  return original_menu_move(menu, page, a, b);
}

// Closes the vanilla menu when it shows a page that was disabled while it was open.
void enforce_menus() {
  void* menu = menu_object.load();
  if (!menu || disabled_pages.load() == 0) {
    return;
  }
  void* state = static_cast<std::uint8_t*>(menu) + kMenuState;
  const std::uint8_t phase = slot<MenuGet>(state, kStateGet)(state);
  if (phase == 0 || phase == 1) {
    return;
  }
  const std::uint8_t page = slot<MenuGet>(menu, kMenuPage)(menu);
  if (page_disabled(page)) {
    log_line("NATIVEUI menu page %u closed", static_cast<unsigned>(page));
    slot<MenuClose>(menu, kMenuClose)(menu, false);
  }
}

bool install_menu_hooks() {
  if (!natives.menu_open || !natives.menu_move || !natives.function_free) {
    return false;
  }
  return MH_CreateHook(natives.menu_open, reinterpret_cast<void*>(hooked_menu_open),
                       reinterpret_cast<void**>(&original_menu_open)) == MH_OK &&
         MH_EnableHook(natives.menu_open) == MH_OK &&
         MH_CreateHook(natives.menu_move, reinterpret_cast<void*>(hooked_menu_move),
                       reinterpret_cast<void**>(&original_menu_move)) == MH_OK &&
         MH_EnableHook(natives.menu_move) == MH_OK;
}

}  // namespace

void hide_screens_for_picture() {
  for (const auto& [name, instance] : shown_screens) {
    void* element = element_raw(name, instance);
    if (element && slot<bool(__fastcall*)(void*)>(element, kIsVisible)(element)) {
      slot<ElementFlag>(element, kSetVisible)(element, false);
      picture_hidden.push_back({name, instance});
    }
  }
}

void show_screens_after_picture() {
  for (const auto& [name, instance] : picture_hidden) {
    // A screen its resource hid or closed meanwhile stays as the resource left it.
    if (shown_screens.count({name, instance}) == 0) {
      continue;
    }
    if (void* element = element_raw(name, instance)) {
      slot<ElementFlag>(element, kSetVisible)(element, true);
    }
  }
  picture_hidden.clear();
}

bool mount_files(const std::string& key, const std::string& folder,
                 const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak || entries.empty()) {
    return false;
  }
  for (const auto& [path, unused] : entries) {
    if (!safe_relative(path)) {
      return false;
    }
  }
  const std::string binding = lookup_root(pak, folder);
  return mount_pack(pak, binding, key, stored_zip_entries(entries));
}

bool mount_table_patch(const std::string& path, const std::string& text) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  const std::size_t top = path.find('/');
  if (!pak || !safe_relative(path) || top == std::string::npos || top == 0) {
    return false;
  }
  const std::string key = "_kcds/tables/" + path;
  const std::vector<std::uint8_t> data(text.begin(), text.end());
  bool mounted = mounted_packs.count(key) != 0;
  if (!mounted) {
    const std::string folder = path.substr(0, top + 1);
    const std::string binding = lookup_root(pak, folder);
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
    entries.emplace_back(path.substr(top + 1), data);
    mounted = mount_pack(pak, binding, key, stored_zip_entries(entries));
    if (mounted) {
      mounted_packs.insert(key);
    }
  }
  const bool reads = mounted && game_file_is(pak, path, data);
  if (reads && natives.table_patch) {
    natives.table_patch(path, text);
  }
  if (log_line) {
    log_line("NATIVEUI table patch %s %s", path.c_str(),
             reads ? "mounted" : (mounted ? "mounted but not read" : "not mounted"));
  }
  return reads;
}

bool mount_item_icon(const std::string& name, const std::vector<std::uint8_t>& dds) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak || name.empty() || !safe_relative(name) || name.find('/') != std::string::npos) {
    return false;
  }
  const std::string key = "_kcds/icons/" + name;
  const std::string entry = name + "_icon.dds";
  if (mounted_packs.count(key) != 0) {
    return true;
  }
  const std::vector<std::uint8_t> data = kcd2s::integration::fit_item_icon(dds);
  const std::string binding = data.empty() ? std::string() : lookup_root(pak, kItemIconRoot);
  const bool mounted = mount_pack(pak, binding, key, stored_zip(entry, data));
  if (mounted) {
    mounted_packs.insert(key);
  }
  if (log_line) {
    log_line("NATIVEUI item icon %s %s", name.c_str(), mounted ? "mounted" : "not mounted");
  }
  return mounted;
}

bool mount_recipe_picture(bool product, std::uint32_t recipe_id, std::uint32_t from,
                          const std::vector<std::uint8_t>& dds) {
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak || recipe_id == 0) {
    return false;
  }
  const std::string root =
      std::string("Libs/UI/Textures/Books/") + (product ? "Products/" : "Recipes/");
  const std::string entry = std::to_string(recipe_id) + "_ui.dds";
  const std::string key = std::string("_kcds/recipe_book/") + (product ? "product_" : "recipe_") +
                          std::to_string(recipe_id);
  if (mounted_packs.count(key) != 0) {
    return true;
  }
  std::vector<std::uint8_t> data = dds;
  if (data.empty() && from != 0) {
    read_game_file(root + std::to_string(from) + "_ui.dds", data);
  }
  const std::string binding = data.empty() ? std::string() : lookup_root(pak, root);
  const bool mounted = mount_pack(pak, binding, key, stored_zip(entry, data));
  if (mounted) {
    mounted_packs.insert(key);
  }
  std::vector<std::uint8_t> check;
  const bool readable = mounted && read_game_file(root + entry, check);
  if (log_line) {
    log_line("NATIVEUI recipe picture %s%s %s (from %u, readable %u, %zu bytes)", root.c_str(),
             entry.c_str(), mounted ? "mounted" : "not mounted", unsigned(from), unsigned(readable),
             check.size());
  }
  return readable;
}

bool mount_shipped(const std::string& resource, const std::string& path, const std::string& root,
                   const std::string& entry) {
  return mount_shipped_entry(resource, path, root, entry);
}

std::vector<std::string> mount_local_files(
    const std::string& key,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
  std::vector<std::string> read;
  void* pak = natives.pak ? *natives.pak : nullptr;
  if (!pak) {
    return read;
  }
  std::map<std::string, std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> folders;
  for (const auto& [path, data] : files) {
    const std::size_t top = path.find('/');
    if (!safe_relative(path) || top == std::string::npos || top == 0) {
      continue;
    }
    folders[path.substr(0, top + 1)].emplace_back(path.substr(top + 1), data);
  }
  for (auto& [folder, entries] : folders) {
    const std::string pack = "architect-" + key + "-" + folder.substr(0, folder.size() - 1);
    const std::string binding = lookup_root(pak, folder);
    const bool mounted = mount_pack(pak, binding, pack, stored_zip_entries(entries));
    if (log_line) {
      log_line("NATIVEUI architect assets %s %s: %zu files at %s", pack.c_str(),
               mounted ? "mounted" : "not mounted", entries.size(), binding.c_str());
    }
  }
  for (const auto& [path, data] : files) {
    if (game_file_exists(pak, path)) {
      read.push_back(path);
    }
  }
  return read;
}

bool install(const Natives& given, Logger logger) {
  log_line = logger;
  natives = given;
  kcds::menu_native::set_menu_button_pressed(menu_button_pressed);
  if (natives.element) {
    auto read = static_cast<const std::uint8_t*>(natives.element) + kFlashUiRead;
    if (read[0] == 0x48 && read[1] == 0x8B && read[2] == 0x0D) {
      std::int32_t displacement;
      std::memcpy(&displacement, read + 3, 4);
      flash_ui = reinterpret_cast<void**>(const_cast<std::uint8_t*>(read) + 7 + displacement);
    }
  }
  const bool files = install_file_hook();
  if (log_line) {
    log_line("NATIVEUI shipped files %s", files ? "installed" : "unavailable");
  }
  const bool menus = install_menu_hooks();
  if (log_line) {
    log_line("NATIVEUI menus %s", menus ? "installed" : "unavailable");
  }
  return flash_ui && natives.string_assign && natives.string_init && natives.string_free &&
         natives.element && natives.args_init && natives.args_free && natives.args_add &&
         natives.data_free;
}

void* element_instance(const char* name, int instance) {
  return natives.element ? element_of(name ? name : "", instance) : nullptr;
}

sdk::Value request(const sdk::Value& body) {
  return natives.element ? answer(body) : sdk::Value::null();
}

void hold_page(int page) {
  held_page.store(page);
}

bool has_focus() {
  return screen_focus.load();
}

bool read_game_data(const std::string& path, std::vector<std::uint8_t>& out) {
  return read_game_file(path, out);
}

bool write_own_file(const std::string& key, const std::vector<std::uint8_t>& bytes) {
  return key.rfind("_kcds/", 0) == 0 && safe_relative(key) && write_shipped(key, bytes);
}

bool map_icon_ready(const std::string& name) {
  return mounted_map_icons.count(name) != 0;
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "syncRequest") {
    const sdk::Value* id = field(message, "id");
    const sdk::Value* request = field(message, "request");
    const sdk::Value* posted = field(message, "posted");
    if (!id || id->tag() != sdk::Tag::Int || !request) {
      return true;
    }
    const std::string op = text_field(*request, "op");
    if (op.rfind("element", 0) != 0) {
      return false;  // another module's synchronous request
    }
    const sdk::Value* library =
        op == "elementLibrary" ? cached_library(text_field(*request, "name")) : nullptr;
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (library) {
      replies.push_back(Reply{id->as_int(), *library});
      return true;
    }
    if (requests.size() < 1024) {
      const bool post = posted && posted->tag() == sdk::Tag::Bool && posted->as_bool();
      requests.push_back(Request{post ? 0 : id->as_int(), *request, post});
    }
    return true;
  }
  if (operation == "nativeFile") {
    receive_file(message);
    return true;
  }
  if (operation == "assetsMount") {
    StreamRequest request;
    request.resource = text_field(message, "resource");
    const sdk::Value* replace = field(message, "replace");
    request.replace = replace && replace->tag() == sdk::Tag::Bool && replace->as_bool();
    const sdk::Value* files = field(message, "files");
    if (files && files->tag() == sdk::Tag::Array && files->as_array().size() <= kMaxStreamFiles) {
      for (const sdk::Value& file : files->as_array()) {
        if (file.tag() == sdk::Tag::Str) {
          request.files.push_back(file.as_str());
        }
      }
    }
    std::lock_guard<std::mutex> lock(mount_mutex);
    if (stream_requests.size() < 64) {
      stream_requests.push_back(std::move(request));
    }
    return true;
  }
  if (operation == "nativeMount") {
    std::lock_guard<std::mutex> lock(mount_mutex);
    if (mount_requests.size() < 256) {
      const sdk::Value* icon = field(message, "itemIcon");
      const sdk::Value* map_icon = field(message, "mapIcon");
      mount_requests.push_back(
          MountRequest{text_field(message, "resource"), text_field(message, "path"),
                       icon && icon->tag() == sdk::Tag::Bool && icon->as_bool(),
                       map_icon && map_icon->tag() == sdk::Tag::Bool && map_icon->as_bool()});
    }
    return true;
  }
  if (operation == "nativeScreenFocus") {
    const sdk::Value* focused = field(message, "focused");
    screen_focus.store(focused && focused->tag() == sdk::Tag::Bool && focused->as_bool());
    return true;
  }
  if (operation == "nativeMenus") {
    std::uint32_t mask = 0;
    const sdk::Value* disabled = field(message, "disabled");
    if (disabled && disabled->tag() == sdk::Tag::Array) {
      for (const sdk::Value& page : disabled->as_array()) {
        if (page.tag() == sdk::Tag::Int && page.as_int() > 0 && page.as_int() < 32) {
          mask |= 1u << page.as_int();
        }
      }
    }
    disabled_pages.store(mask);
    return true;
  }
  if (operation == "nativeMenuButtons") {
    // The resources' own Esc menu buttons, as they are now.
    std::vector<kcds::menu_native::MenuButton> buttons;
    const sdk::Value* list = field(message, "buttons");
    if (list && list->tag() == sdk::Tag::Array) {
      for (const sdk::Value& entry : list->as_array()) {
        const sdk::Value* key = field(entry, "key");
        const sdk::Value* label = field(entry, "label");
        if (key && label && key->tag() == sdk::Tag::Str && label->tag() == sdk::Tag::Str &&
            buttons.size() < kcds::menu_native::kMaxMenuButtons) {
          buttons.push_back({key->as_str(), label->as_str()});
        }
      }
    }
    if (log_line) {
      log_line("NATIVEUI menu buttons: %zu", buttons.size());
    }
    kcds::menu_native::set_menu_buttons(std::move(buttons));
    return true;
  }
  if (operation == "nativeElementListen" || operation == "nativeElementUnlisten") {
    std::lock_guard<std::mutex> lock(queue_mutex);
    requests.push_back(Request{0, message});
    return true;
  }
  return false;
}

void mount_pending() {
  mount_textures();
}

// The game's Esc menu (the "Menu" element) as the scripts last heard: open or not, and the
// change frame() saw for pump() to send (nativeMenuOpened / nativeMenuClosed).
bool menu_seen_open = false;
int menu_change = -1;
std::uint64_t menu_checked_at = 0;
constexpr std::uint64_t kMenuCheckMs = 100;

void watch_menu() {
  const std::uint64_t now = GetTickCount64();
  if (now - menu_checked_at < kMenuCheckMs) {
    return;
  }
  menu_checked_at = now;
  void* menu = element_raw("Menu", 0);
  const bool open = menu && slot<bool(__fastcall*)(void*)>(menu, kIsVisible)(menu);
  if (open == menu_seen_open) {
    return;
  }
  menu_seen_open = open;
  std::lock_guard<std::mutex> lock(queue_mutex);
  menu_change = open ? 1 : 0;
}

void frame(bool in_world) {
  static bool warmed = false;
  if (!warmed) {
    warmed = true;
    for (const char* name : kWarmLibraries) {
      element_library(name);
    }
  }
  remove_stale_shipped();
  mount_textures();
  if (in_world) {
    enforce_menus();
    watch_menu();
  }
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    if (request.posted) {
      answer(request.body);
      continue;
    }
    if (request.id == 0) {
      listen(request.body,
             kcd2s::integration::script_operation(request.body) == "nativeElementListen");
      continue;
    }
    answered.push_back(Reply{request.id, answer(request.body)});
  }
  std::vector<Event> raised;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    raised.swap(events);
  }
  std::vector<Event> wanted;
  for (Event& event : raised) {
    if (listened(event.name, event.instance, event.event)) {
      wanted.push_back(std::move(event));
    }
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (Reply& reply : answered) {
    replies.push_back(std::move(reply));
  }
  for (Event& event : wanted) {
    if (outgoing.size() < 256) {
      outgoing.push_back(std::move(event));
    }
  }
}

void pump(const void* session, const RuntimeSender& send) {
  std::vector<Reply> taken_replies;
  std::vector<Event> taken_events;
  std::vector<std::string> taken_presses;
  int taken_menu_change = -1;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken_replies.swap(replies);
    taken_events.swap(outgoing);
    taken_presses.swap(menu_presses);
    taken_menu_change = std::exchange(menu_change, -1);
  }
  if (taken_menu_change >= 0) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("nativeMenu"))},
                                    {"open", sdk::Value(taken_menu_change == 1)}}));
  }
  if (session != reported_session) {
    // A new session, or none: no resource holds a menu or has a button in it any more.
    reported_session = session;
    disabled_pages.store(0);
    screen_focus.store(false);
    kcds::menu_native::set_menu_buttons({});
    taken_presses.clear();
  }
  for (std::string& key : taken_presses) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("nativeMenuButton"))},
                                    {"key", sdk::Value(std::move(key))}}));
  }
  for (const Reply& reply : taken_replies) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", reply.value}}));
  }
  std::vector<StreamMounted> mounted_streams;
  {
    std::lock_guard<std::mutex> lock(mount_mutex);
    mounted_streams.swap(streams_mounted);
  }
  for (StreamMounted& mounted : mounted_streams) {
    sdk::Value::Array paths;
    for (std::string& path : mounted.paths) {
      paths.push_back(sdk::Value(std::move(path)));
    }
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("assetsMounted"))},
                                    {"resource", sdk::Value(mounted.resource)},
                                    {"paths", sdk::Value(std::move(paths))}}));
  }
  for (Event& event : taken_events) {
    send(sdk::Value(
        sdk::Value::Map{{"op", sdk::Value(std::string("nativeElementEvent"))},
                        {"name", sdk::Value(event.name)},
                        {"instance", sdk::Value(static_cast<std::int64_t>(event.instance))},
                        {"event", sdk::Value(event.event)},
                        {"args", sdk::Value(std::move(event.args))}}));
  }
}

}  // namespace kcds::runtime::native_ui
