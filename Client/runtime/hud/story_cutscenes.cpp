#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "story_cutscenes.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::hud::story_cutscenes {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::size_t kInstanceName = 24;
constexpr std::size_t kInstanceCutscene = 32;
constexpr std::size_t kMaxQueued = 64;
constexpr std::size_t kMaxNameBytes = 256;
// The listeners' events a played cutscene goes through: queued, about to play, played, finished.
constexpr int kPlayedEvents[] = {0, 1, 2, 3};

using Queue = void(__fastcall*)(void* player, void** instance);
using Notify = void(__fastcall*)(void* player, void** instance, int event);

Logger log_line = nullptr;
Natives natives;
Queue original_queue = nullptr;
std::atomic<bool> enabled{false};
std::mutex queue_mutex;
std::vector<std::string> suppressed;  // names not yet reported
const void* reported_session = nullptr;

template <class T>
T field(const void* object, std::size_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(object) + offset);
}

std::string name_of(const void* instance) {
  const char* name = field<const char*>(instance, kInstanceName);
  if (!name) {
    return {};
  }
  std::string text;
  for (std::size_t i = 0; i < kMaxNameBytes && name[i] != '\0'; ++i) {
    text += name[i];
  }
  return text;
}

void __fastcall hooked_queue(void* player, void** instance) {
  const bool playable = instance && *instance && field<void*>(*instance, kInstanceCutscene);
  if (enabled.load() || !playable) {
    original_queue(player, instance);
    return;
  }
  std::string name = name_of(*instance);
  if (log_line) {
    log_line("HUD story cutscene suppressed: %s", name.c_str());
  }
  // The quest's node hears the cutscene play out at once, so its graph goes on.
  for (const int event : kPlayedEvents) {
    reinterpret_cast<Notify>(natives.notify)(player, instance, event);
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (suppressed.size() < kMaxQueued) {
    suppressed.push_back(std::move(name));
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.queue || !natives.notify) {
    return false;
  }
  return MH_CreateHook(natives.queue, reinterpret_cast<void*>(hooked_queue),
                       reinterpret_cast<void**>(&original_queue)) == MH_OK &&
         MH_EnableHook(natives.queue) == MH_OK;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "hudStoryCutscenes") {
    return false;
  }
  const sdk::Value* value = kcd2s::integration::script_field(message, "enabled");
  enabled.store(value && value->tag() == sdk::Tag::Bool && value->as_bool());
  return true;
}

void pump(const void* session, const RuntimeSender& send) {
  std::vector<std::string> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(suppressed);
  }
  if (session != reported_session) {
    reported_session = session;
    enabled.store(false);  // a new session suppresses story cutscenes again
    taken.clear();
  }
  for (const std::string& name : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("hudStoryCutsceneSuppressed"))},
                                    {"cutscene", sdk::Value(name)}}));
  }
}

}  // namespace kcds::runtime::hud::story_cutscenes
