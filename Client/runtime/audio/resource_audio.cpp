#include "resource_audio.hpp"

#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../crafting/sounds.hpp"
#include "voice_files.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::audio {
namespace {

namespace sdk = kcd2s::sdk;
namespace sounds = crafting::sounds;

// The audio system's slots (IAudioSystem).
constexpr std::size_t kRtpcId = 8;
constexpr std::size_t kSwitchId = 9;
constexpr std::size_t kSwitchStateId = 10;
constexpr std::size_t kFreeProxy = 18;
// An entity's audio component's slots.
constexpr std::size_t kPlayFile = 28;
constexpr std::size_t kStopAll = 32;
constexpr std::size_t kSetSwitchState = 33;
constexpr std::size_t kSetRtpc = 34;
constexpr std::uint32_t kAllAuxObjects = 0;
// The component's own (default) audio object, where the game plays its spoken files.
constexpr std::uint32_t kDefaultAuxObject = 1;
// The switch whose state picks the voice host event a file plays through.
constexpr char kFilePurposeSwitch[] = "standalone_file_purpose";
// A free audio proxy's slots (CAudioProxy).
constexpr std::size_t kProxyInitialize = 1;
constexpr std::size_t kProxyRelease = 2;
constexpr std::size_t kProxyExecute = 6;
constexpr std::size_t kProxyStopAll = 8;
constexpr std::size_t kProxyPosition = 13;
constexpr char kProxyName[] = "KCDS resource sound";
// Requests waiting at once; a script waits about one frame for its answer.
constexpr std::size_t kMaxQueued = 256;
// Coordinates a sound may be played at (the world's own range).
constexpr double kMaxCoordinate = 16384.0;
// Kept sounds (Audio.loopAt) at once.
constexpr std::size_t kMaxLoops = 256;

using NameId = bool(__fastcall*)(void* audio, const char* name, std::uint32_t* id);
using StateId = bool(__fastcall*)(void* audio, std::uint32_t switch_id, const char* name,
                                  std::uint32_t* id);
using FreeProxy = void*(__fastcall*)(void* audio);
using PlayFile = void(__fastcall*)(void* component, const char* file, float start,
                                   std::uint32_t aux_object, void* callback);
using StopAll = void(__fastcall*)(void* component, std::uint32_t aux_object);
using SetSwitchState = void(__fastcall*)(void* component, std::uint32_t switch_id,
                                         std::uint32_t state, std::uint32_t aux_object);
using SetRtpc = void(__fastcall*)(void* component, std::uint32_t rtpc, float value,
                                  std::uint32_t aux_object);
using ProxyInitialize = void(__fastcall*)(void* proxy, const char* name, bool asynchronous);
using ProxyRelease = void(__fastcall*)(void* proxy);
using ProxyExecute = void(__fastcall*)(void* proxy, std::uint32_t trigger, std::uint32_t lip_sync,
                                       void* callback);
using ProxyPosition = void(__fastcall*)(void* proxy, const float* matrix);
using ProxyStopAll = void(__fastcall*)(void* proxy);
using EmptyCallback = void*(__fastcall*)();

struct Request {
  std::int64_t id = 0;
  sdk::Value body;
};

struct Reply {
  std::int64_t id = 0;
  bool value = false;
};

Natives natives;
std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;
// The resources' kept sounds by their number (engine thread).
std::map<std::int64_t, void*> loops;
// This frame's entity system and local player entity.
void* frame_entities = nullptr;
void* frame_local = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

const sdk::Value* field(const sdk::Value& message, const char* name) {
  return kcd2s::integration::script_field(message, name);
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string{};
}

bool number(const sdk::Value& message, const char* name, double& out) {
  const sdk::Value* value = field(message, name);
  if (!value) {
    return false;
  }
  if (value->tag() == sdk::Tag::Double) {
    out = value->as_double();
  } else if (value->tag() == sdk::Tag::Int) {
    out = double(value->as_int());
  } else {
    return false;
  }
  return std::isfinite(out);
}

std::uint32_t named(std::size_t slot, const std::string& name) {
  void* audio = sounds::audio_system();
  std::uint32_t id = 0;
  if (!audio || name.empty() || !slot_of<NameId>(audio, slot)(audio, name.c_str(), &id)) {
    return 0;
  }
  return id;
}

// The engine entity the request names, null when there is none.
void* entity_of(const sdk::Value& request) {
  double id = 0;
  if (!number(request, "entity", id) || id < 1 || id > 4294967295.0 || !natives.get_entity ||
      !frame_entities) {
    return nullptr;
  }
  return natives.get_entity(frame_entities, std::uint32_t(id));
}

bool play_at(std::uint32_t trigger, const sdk::Value& request) {
  double x = 0;
  double y = 0;
  double z = 0;
  const bool placed = number(request, "x", x) && number(request, "y", y) && number(request, "z", z);
  if (!placed || std::fabs(x) > kMaxCoordinate || std::fabs(y) > kMaxCoordinate ||
      std::fabs(z) > kMaxCoordinate || !natives.empty_callback) {
    return false;
  }
  void* audio = sounds::audio_system();
  void* proxy = audio ? slot_of<FreeProxy>(audio, kFreeProxy)(audio) : nullptr;
  if (!proxy) {
    return false;
  }
  // Rows of the 3 x 4 matrix: no turn, the point as its translation.
  const float matrix[12] = {1.0f, 0.0f,     0.0f, float(x), 0.0f, 1.0f,
                            0.0f, float(y), 0.0f, 0.0f,     1.0f, float(z)};
  slot_of<ProxyInitialize>(proxy, kProxyInitialize)(proxy, kProxyName, false);
  slot_of<ProxyPosition>(proxy, kProxyPosition)(proxy, matrix);
  void* callback = reinterpret_cast<EmptyCallback>(natives.empty_callback)();
  slot_of<ProxyExecute>(proxy, kProxyExecute)(proxy, trigger, 0, callback);
  slot_of<ProxyRelease>(proxy, kProxyRelease)(proxy);
  return true;
}

// The file without its extension: the game's audio adds the format's own (an .ogg).
std::string without_extension(const std::string& file) {
  const std::size_t dot = file.rfind('.');
  const std::size_t slash = file.rfind('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return file;
  }
  return file.substr(0, dot);
}

// A sound file on the entity through the purpose's voice host event, as the game plays a spoken
// line: the purpose switch's state first, then the file on the same audio object.
bool play_file(void* component, const sdk::Value& request) {
  void* audio = sounds::audio_system();
  const std::string file = without_extension(text(request, "file"));
  const std::string purpose = text(request, "purpose");
  const std::uint32_t switch_id = named(kSwitchId, kFilePurposeSwitch);
  std::uint32_t state = 0;
  const bool known =
      switch_id != 0 && !file.empty() && !purpose.empty() && natives.empty_callback &&
      slot_of<StateId>(audio, kSwitchStateId)(audio, switch_id, purpose.c_str(), &state);
  if (!known) {
    return false;
  }
  slot_of<SetSwitchState>(component, kSetSwitchState)(component, switch_id, state,
                                                      kDefaultAuxObject);
  void* callback = reinterpret_cast<EmptyCallback>(natives.empty_callback)();
  slot_of<PlayFile>(component, kPlayFile)(component, file.c_str(), 0.0f, kDefaultAuxObject,
                                          callback);
  return true;
}

// Audio.loopAt and stopLoop: a kept sound under the runtime's number.
bool answer_loop(const std::string& op, const sdk::Value& request) {
  double number_given = 0;
  if (!number(request, "loop", number_given) || number_given < 1) {
    return false;
  }
  const std::int64_t id = std::int64_t(number_given);
  auto kept = loops.find(id);
  if (op == "audioLoopStop") {
    if (kept == loops.end()) {
      return false;
    }
    stop_loop(kept->second);
    loops.erase(kept);
    return true;
  }
  double x = 0;
  double y = 0;
  double z = 0;
  const bool placed = number(request, "x", x) && number(request, "y", y) && number(request, "z", z);
  if (!placed || kept != loops.end() || loops.size() >= kMaxLoops ||
      std::fabs(x) > kMaxCoordinate || std::fabs(y) > kMaxCoordinate ||
      std::fabs(z) > kMaxCoordinate) {
    return false;
  }
  const float position[3] = {float(x), float(y), float(z)};
  void* loop = start_loop(text(request, "trigger").c_str(), position);
  if (!loop) {
    return false;
  }
  loops[id] = loop;
  return true;
}

bool answer(const sdk::Value& request) {
  const std::string op = kcd2s::integration::script_operation(request);
  if (!sounds::audio_system()) {
    return false;
  }
  if (op == "audioLoopAt" || op == "audioLoopStop") {
    return answer_loop(op, request);
  }
  if (op == "audioHasTrigger") {
    return sounds::trigger_id(text(request, "trigger").c_str()) != 0;
  }
  if (op == "audioPlay" || op == "audioPlayAt" || op == "audioPlayOnEntity" ||
      op == "audioStopOnEntity") {
    const std::uint32_t trigger = sounds::trigger_id(text(request, "trigger").c_str());
    if (trigger == 0) {
      return false;
    }
    if (op == "audioPlayAt") {
      return play_at(trigger, request);
    }
    void* entity = op == "audioPlay" ? frame_local : entity_of(request);
    if (!entity || !sounds::audio_of(entity)) {
      return false;
    }
    sounds::play(entity, trigger, op != "audioStopOnEntity");
    return true;
  }
  void* entity = entity_of(request);
  void* component = entity ? sounds::audio_of(entity) : nullptr;
  if (!component) {
    return false;
  }
  if (op == "audioPlayFile") {
    return play_file(component, request);
  }
  if (op == "audioStopAll") {
    slot_of<StopAll>(component, kStopAll)(component, kAllAuxObjects);
    return true;
  }
  if (op == "audioParameter") {
    double value = 0;
    const std::uint32_t rtpc = named(kRtpcId, text(request, "parameter"));
    if (rtpc == 0 || !number(request, "value", value)) {
      return false;
    }
    slot_of<SetRtpc>(component, kSetRtpc)(component, rtpc, float(value), kAllAuxObjects);
    return true;
  }
  if (op == "audioSwitch") {
    const std::uint32_t switch_id = named(kSwitchId, text(request, "switch"));
    std::uint32_t state = 0;
    void* audio = sounds::audio_system();
    const std::string state_name = text(request, "state");
    if (switch_id == 0 || state_name.empty() ||
        !slot_of<StateId>(audio, kSwitchStateId)(audio, switch_id, state_name.c_str(), &state)) {
      return false;
    }
    slot_of<SetSwitchState>(component, kSetSwitchState)(component, switch_id, state,
                                                        kAllAuxObjects);
    return true;
  }
  return false;
}

// The hooks of the spoken files (voice_files.hpp) go on through the local player's component;
// test kits log every file played.
void watch_files(void* local_entity) {
  static void* watched_entity = nullptr;
  if (!local_entity || local_entity == watched_entity) {
    return;
  }
  void* component = sounds::audio_of(local_entity);
  if (component) {
    watched_entity = local_entity;
    voices::watch(component, component, natives.log);
  }
}

// Rows of the 3 x 4 matrix: no turn, the point as its translation.
void place_proxy(void* proxy, const float position[3]) {
  const float matrix[12] = {1.0f, 0.0f,        0.0f, position[0], 0.0f, 1.0f,
                            0.0f, position[1], 0.0f, 0.0f,        1.0f, position[2]};
  slot_of<ProxyPosition>(proxy, kProxyPosition)(proxy, matrix);
}

}  // namespace

void* start_loop(const char* trigger, const float position[3]) {
  const std::uint32_t id = sounds::trigger_id(trigger);
  void* audio = sounds::audio_system();
  if (id == 0 || !audio || !natives.empty_callback) {
    return nullptr;
  }
  void* proxy = slot_of<FreeProxy>(audio, kFreeProxy)(audio);
  if (!proxy) {
    return nullptr;
  }
  slot_of<ProxyInitialize>(proxy, kProxyInitialize)(proxy, kProxyName, false);
  place_proxy(proxy, position);
  void* callback = reinterpret_cast<EmptyCallback>(natives.empty_callback)();
  slot_of<ProxyExecute>(proxy, kProxyExecute)(proxy, id, 0, callback);
  return proxy;
}

void move_loop(void* loop, const float position[3]) {
  if (loop) {
    place_proxy(loop, position);
  }
}

void stop_loop(void* loop) {
  if (!loop) {
    return;
  }
  slot_of<ProxyStopAll>(loop, kProxyStopAll)(loop);
  slot_of<ProxyRelease>(loop, kProxyRelease)(loop);
}

void stop_resource_loops() {
  for (const auto& [id, loop] : loops) {
    stop_loop(loop);
  }
  loops.clear();
}

void install(const Natives& given) {
  natives = given;
  voices::set_empty_callback(given.empty_callback);
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = field(message, "id");
  const sdk::Value* request = field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request) {
    return false;
  }
  if (kcd2s::integration::script_operation(*request).rfind("audio", 0) != 0) {
    return false;  // another module's synchronous request
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMaxQueued) {
    requests.push_back(Request{id->as_int(), *request});
  }
  return true;
}

void frame(void* entity_system, void* local_entity) {
  frame_entities = entity_system;
  frame_local = local_entity;
  watch_files(local_entity);
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(Reply{request.id, answer(request.body)});
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (const Reply& reply : answered) {
    replies.push_back(reply);
  }
}

void pump(const RuntimeSender& send) {
  std::vector<Reply> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (const Reply& reply : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", sdk::Value(reply.value)}}));
  }
}

}  // namespace kcds::runtime::audio
