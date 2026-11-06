#include "monologue.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"
#include "../branding/runtime_branding.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::hud::monologue {
namespace {

namespace sdk = kcd2s::sdk;

constexpr const char* kMonologSignal = "dialog:monologRequest";
constexpr std::size_t kActorSystem = 464;
constexpr std::size_t kLocalActor = 48;
constexpr std::size_t kEntityId = 1072;
constexpr std::size_t kMaxQueued = 64;

using SendSignal = std::int64_t(__fastcall*)(void* self, unsigned entity, const char* const* signal,
                                             const char* const* params);
using EnvGetter = std::uint8_t*(__fastcall*)();
using ActorGetter = void*(__fastcall*)(void* system);
using IdGetter = unsigned(__fastcall*)(void* actor, int unused);
// The actor system's actor of a dialog participant's id (its vtable slot 0, as the dialog start's
// participant check calls it).
using ActorById = void*(__fastcall*)(void* system, std::uint64_t id);

// A metarole's lines: their localization keys, separated by '|'.
struct MetaroleLines {
  const char* metarole;
  const char* keys;
};

constexpr MetaroleLines kLines[] = {
#include "monologue_lines.inc"
};

// A suppressed line: its localization key (the metarole when the game has no line for it) and its
// text in the player's language ("" when unknown).
struct Suppressed {
  std::string text_key;
  std::string text;
};

Logger log_line = nullptr;
Natives natives;
SendSignal original_send = nullptr;
std::atomic<bool> enabled{false};
std::mutex queue_mutex;
std::vector<Suppressed> suppressed;  // lines not yet reported
const void* reported_session = nullptr;
std::minstd_rand pick_random{std::random_device{}()};

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

// The local player's entity id, as the game finds it for its own monologue requests; 0 when
// there is no player.
unsigned local_entity() {
  std::uint8_t* env = reinterpret_cast<EnvGetter>(natives.env)();
  void* system = env ? *reinterpret_cast<void**>(env + kActorSystem) : nullptr;
  void* actor = system ? slot<ActorGetter>(system, kLocalActor)(system) : nullptr;
  return actor ? slot<IdGetter>(actor, kEntityId)(actor, 0) : 0;
}

// The metarole (or alias) a request names: "metarole('NAME'),...".
std::string metarole(const char* params) {
  if (!params) {
    return {};
  }
  for (const char* key : {"metarole('", "alias('"}) {
    const char* start = std::strstr(params, key);
    if (start) {
      start += std::strlen(key);
      const char* end = std::strchr(start, '\'');
      return end ? std::string(start, end) : std::string();
    }
  }
  return {};
}

// One line of the metarole's pool, as the dialogue system would pick one: its key, or "" for a
// metarole the game has no player monologue for.
std::string pick_line(const std::string& role) {
  for (const MetaroleLines& entry : kLines) {
    if (role != entry.metarole) {
      continue;
    }
    std::vector<std::string_view> keys;
    std::string_view rest = entry.keys;
    while (!rest.empty()) {
      const std::size_t bar = rest.find('|');
      keys.push_back(rest.substr(0, bar));
      rest = bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1);
    }
    if (keys.empty()) {
      return {};
    }
    std::uniform_int_distribution<std::size_t> index(0, keys.size() - 1);
    return std::string(keys[index(pick_random)]);
  }
  return {};
}

std::int64_t __fastcall hooked_send(void* self, unsigned entity, const char* const* signal,
                                    const char* const* params) {
  // Test kits only (KCDS_TEST_HOOKS): every signal the local player gets, by name.
  static const bool trace = GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0;
  if (trace && log_line && entity != 0 && entity == local_entity()) {
    log_line("HUD signal %s (%s)", signal && *signal ? *signal : "(none)",
             params && *params ? *params : "");
  }
  const bool monolog = signal && *signal && std::strcmp(*signal, kMonologSignal) == 0;
  if (!monolog || enabled.load() || entity == 0 || entity != local_entity()) {
    return original_send(self, entity, signal, params);
  }
  const std::string role = metarole(params ? *params : nullptr);
  Suppressed line;
  line.text_key = pick_line(role);
  if (line.text_key.empty()) {
    line.text_key = role;
  } else {
    line.text = kcds::runtime::branding::localize(line.text_key);
  }
  if (log_line) {
    log_line("HUD monologue suppressed %s: %s \"%s\"", role.c_str(), line.text_key.c_str(),
             line.text.c_str());
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (suppressed.size() < kMaxQueued) {
    suppressed.push_back(std::move(line));
  }
  return 0;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.send_signal || !natives.env) {
    return false;
  }
  return MH_CreateHook(natives.send_signal, reinterpret_cast<void*>(hooked_send),
                       reinterpret_cast<void**>(&original_send)) == MH_OK &&
         MH_EnableHook(natives.send_signal) == MH_OK;
}

bool player_alone(const std::uint64_t* first, const std::uint64_t* last) {
  if (enabled.load() || !natives.env || !first || last - first != 1) {
    return false;
  }
  std::uint8_t* env = reinterpret_cast<EnvGetter>(natives.env)();
  void* system = env ? *reinterpret_cast<void**>(env + kActorSystem) : nullptr;
  if (!system) {
    return false;
  }
  void* local = slot<ActorGetter>(system, kLocalActor)(system);
  void* participant = slot<ActorById>(system, 0)(system, *first);
  return local && participant == local;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "hudMonologue") {
    return false;
  }
  const sdk::Value* value = kcd2s::integration::script_field(message, "enabled");
  enabled.store(value && value->tag() == sdk::Tag::Bool && value->as_bool());
  return true;
}

void pump(const void* session, const RuntimeSender& send) {
  std::vector<Suppressed> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(suppressed);
  }
  if (session != reported_session) {
    reported_session = session;
    enabled.store(false);  // a new session suppresses the monologue again
    taken.clear();
  }
  for (const Suppressed& line : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("hudMonologueSuppressed"))},
                                    {"textKey", sdk::Value(line.text_key)},
                                    {"text", sdk::Value(line.text)}}));
  }
}

}  // namespace kcds::runtime::hud::monologue
