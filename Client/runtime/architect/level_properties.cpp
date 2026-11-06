#include "level_properties.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "../level/level_edits.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_properties {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using kcds::game_client::slot;

// CEntity: the script proxy (GetProxy slot 74, type 2) and the proxy's script table (slot 18).
constexpr std::size_t kGetProxySlot = 74;
constexpr int kScriptProxy = 2;
constexpr std::size_t kScriptTableSlot = 18;
// IScriptTable (CScriptTable).
constexpr std::size_t kReleaseSlot = 4;
constexpr std::size_t kSetValueAnySlot = 7;
constexpr std::size_t kGetValueAnySlot = 8;
constexpr std::size_t kDumpSlot = 21;
// A ScriptAnyValue: 32 bytes, its type first, the value at + 8.
constexpr std::size_t kAnySize = 32;
constexpr std::size_t kAnyValue = 8;
constexpr int kAnyBool = 2;
constexpr int kAnyNumber = 4;
constexpr int kAnyString = 5;
constexpr int kAnyTable = 6;
// The types Dump hands its sink.
constexpr int kDumpTable = 5;
constexpr std::size_t kMostProperties = 80;
constexpr std::size_t kMostQueued = 32;
constexpr std::size_t kLongestText = 256;

using GetProxy = void*(__fastcall*)(void* entity, int type);
using Get = void*(__fastcall*)(void* object);
using GetValueAny = bool(__fastcall*)(void* table, const char* key, void* value, bool chain);
using SetValueAny = void(__fastcall*)(void* table, const char* key, const void* value, bool chain);
using Release = void(__fastcall*)(void* table);
using Dump = void(__fastcall*)(void* table, void* sink);

// IScriptTableDumpSink, built by hand: slot 0 its destructor, slot 1 a numbered key, slot 2 a
// named key (as CScriptTable::Dump, calls them).
struct Sink {
  void* const* vtable;
  std::vector<std::pair<std::string, int>> keys;
};

void __fastcall sink_destroy(Sink*, unsigned) {
}

void __fastcall sink_index(Sink*, int, int) {
}

void __fastcall sink_name(Sink* sink, const char* name, int type) {
  if (name && sink->keys.size() < kMostProperties * 2) {
    sink->keys.emplace_back(std::string(name, strnlen(name, 128)), type);
  }
}

void* const kSinkTable[] = {reinterpret_cast<void*>(&sink_destroy),
                            reinterpret_cast<void*>(&sink_index),
                            reinterpret_cast<void*>(&sink_name)};

// The named keys of a script table and their types.
std::vector<std::pair<std::string, int>> keys_of(void* table) {
  Sink sink{kSinkTable, {}};
  slot<Dump>(table, kDumpSlot)(table, &sink);
  return std::move(sink.keys);
}

bool value_of(void* table, const char* key, std::uint8_t value[kAnySize]) {
  std::memset(value, 0, kAnySize);
  return table && slot<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false);
}

// A table under a key (a reference the caller releases), or null.
void* table_of(void* table, const char* key) {
  alignas(16) std::uint8_t value[kAnySize];
  if (!value_of(table, key, value) || *reinterpret_cast<int*>(value) != kAnyTable) {
    return nullptr;
  }
  void* inner = nullptr;
  std::memcpy(&inner, value + kAnyValue, sizeof(inner));
  return inner;
}

void release(void* table) {
  if (table) {
    slot<Release>(table, kReleaseSlot)(table);
  }
}

void* properties_of(void* entity) {
  void* proxy = entity ? slot<GetProxy>(entity, kGetProxySlot)(entity, kScriptProxy) : nullptr;
  void* script = proxy ? slot<Get>(proxy, kScriptTableSlot)(proxy) : nullptr;
  return script ? table_of(script, "Properties") : nullptr;
}

// One property as the editor shows it; null for a kind it does not show (a function, a table).
sdk::Value property(const std::string& key, const std::uint8_t value[kAnySize]) {
  const int type = *reinterpret_cast<const int*>(value);
  sdk::Value shown;
  std::string kind;
  if (type == kAnyBool) {
    kind = "bool";
    shown = sdk::Value(value[kAnyValue] != 0);
  } else if (type == kAnyNumber) {
    float number = 0.0f;
    std::memcpy(&number, value + kAnyValue, sizeof(number));
    if (!std::isfinite(number)) {
      return sdk::Value();
    }
    kind = "number";
    shown = sdk::Value(double(number));
  } else if (type == kAnyString) {
    const char* text = nullptr;
    std::memcpy(&text, value + kAnyValue, sizeof(text));
    kind = "text";
    shown = sdk::Value(text ? std::string(text, strnlen(text, kLongestText)) : std::string());
  } else {
    return sdk::Value();
  }
  return sdk::Value(sdk::Value::Map{
      {"key", sdk::Value(key)}, {"type", sdk::Value(kind)}, {"value", std::move(shown)}});
}

// The shown properties of a Properties table, its tables' one level down ("Lock.bLocked").
sdk::Value list(void* properties) {
  sdk::Value::Array found;
  for (const auto& [key, type] : keys_of(properties)) {
    if (found.size() >= kMostProperties) {
      break;
    }
    if (type == kDumpTable) {
      void* inner = table_of(properties, key.c_str());
      if (!inner) {
        continue;
      }
      for (const auto& [inner_key, inner_type] : keys_of(inner)) {
        alignas(16) std::uint8_t value[kAnySize];
        if (inner_type == kDumpTable || found.size() >= kMostProperties ||
            !value_of(inner, inner_key.c_str(), value)) {
          continue;
        }
        sdk::Value shown = property(key + "." + inner_key, value);
        if (shown.tag() == sdk::Tag::Map) {
          found.push_back(std::move(shown));
        }
      }
      release(inner);
      continue;
    }
    alignas(16) std::uint8_t value[kAnySize];
    if (!value_of(properties, key.c_str(), value)) {
      continue;
    }
    sdk::Value shown = property(key, value);
    if (shown.tag() == sdk::Tag::Map) {
      found.push_back(std::move(shown));
    }
  }
  return sdk::Value(std::move(found));
}

// Sets a property (as its present type: a bool, a number or a text); false when there is none.
bool set(void* properties, const std::string& key, const sdk::Value& wanted) {
  const std::size_t dot = key.find('.');
  void* table = properties;
  void* inner = nullptr;
  std::string field = key;
  if (dot != std::string::npos) {
    inner = table_of(properties, key.substr(0, dot).c_str());
    table = inner;
    field = key.substr(dot + 1);
  }
  alignas(16) std::uint8_t value[kAnySize];
  bool done = false;
  if (table && value_of(table, field.c_str(), value)) {
    const int type = *reinterpret_cast<int*>(value);
    alignas(16) std::uint8_t next[kAnySize] = {};
    std::memcpy(next, &type, sizeof(type));
    std::string text;
    if (type == kAnyBool && wanted.tag() == sdk::Tag::Bool) {
      next[kAnyValue] = wanted.as_bool() ? 1 : 0;
      done = true;
    } else if (type == kAnyNumber &&
               (wanted.tag() == sdk::Tag::Double || wanted.tag() == sdk::Tag::Int)) {
      const float number =
          float(wanted.tag() == sdk::Tag::Double ? wanted.as_double() : double(wanted.as_int()));
      std::memcpy(next + kAnyValue, &number, sizeof(number));
      done = std::isfinite(number);
    } else if (type == kAnyString && wanted.tag() == sdk::Tag::Str &&
               wanted.as_str().size() <= kLongestText) {
      text = wanted.as_str();
      const char* pointer = text.c_str();
      std::memcpy(next + kAnyValue, &pointer, sizeof(pointer));
      done = true;
    }
    if (done) {
      slot<SetValueAny>(table, kSetValueAnySlot)(table, field.c_str(), next, false);
    }
  }
  release(inner);
  return done;
}

struct Order {
  std::int64_t question = 0;  // a localEntityProperties question's id; 0 for a set
  std::uint64_t guid = 0;
  std::string key;
  sdk::Value value;
};

std::mutex queue_mutex;
std::vector<Order> orders;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

void queue(Order order) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (orders.size() < kMostQueued) {
    orders.push_back(std::move(order));
  } else if (order.question != 0) {
    replies.emplace_back(order.question, sdk::Value());
  }
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "localEntityProperty") {
    Order order;
    const sdk::Value* value = integration::script_field(message, "value");
    if (local_mode::enabled() && value &&
        integration::parse_level_edit_guid(text(message, "guid"), order.guid)) {
      order.key = text(message, "key");
      order.value = *value;
      queue(std::move(order));
    }
    return true;
  }
  if (operation != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request) != "localEntityProperties") {
    return false;
  }
  Order order;
  order.question = id->as_int();
  integration::parse_level_edit_guid(text(*request, "guid"), order.guid);
  queue(std::move(order));
  return true;
}

void frame(bool in_world) {
  std::vector<Order> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(orders);
  }
  if (taken.empty()) {
    return;
  }
  std::vector<std::uint64_t> guids;
  for (const Order& order : taken) {
    guids.push_back(order.guid);
  }
  const auto found = in_world && local_mode::enabled() ? level::entities_by_guid(guids)
                                                       : std::map<std::uint64_t, void*>{};
  std::vector<std::pair<std::int64_t, sdk::Value>> answered;
  for (const Order& order : taken) {
    const auto entity = found.find(order.guid);
    void* properties = entity != found.end() ? properties_of(entity->second) : nullptr;
    if (order.question != 0) {
      answered.emplace_back(order.question, properties ? list(properties) : sdk::Value());
    } else if (properties) {
      set(properties, order.key, order.value);
    }
    release(properties);
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (auto& reply : answered) {
    replies.push_back(std::move(reply));
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

}  // namespace kcds::runtime::architect::level_properties
