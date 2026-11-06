#include "weapon_hits.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::weapon_hits {

namespace {

// CEntity GetWorldTM (a 3x4 row-major matrix) and GetPhysics; the render component's
// GetLocalBounds (min then max).
constexpr std::size_t kGetWorldTM = 32;
constexpr std::size_t kGetPhysics = 79;
constexpr unsigned kRenderComponent = 0;
constexpr std::size_t kGetLocalBounds = 18;
// A blade's tip lies at least this far and at most this far from the hand (metres): a dagger's
// is short, a polearm's long; bounds outside that are not a blade.
constexpr float kShortestBlade = 0.15f;
constexpr float kLongestBlade = 2.5f;
// The game stops a blade against a wall a hair before its tip touches (a
// longsword's tip rested 1 to 4 cm short of the inn's wall), so the trace reaches this far past the
// tip.
constexpr float kPastTheTip = 0.1f;
// More than this many seconds without a live frame start the next attack.
constexpr double kAttackGap = 0.4;
// A blade moves at most this far between two frames and is still the same swing (metres): a
// teleport in between is not a path.
constexpr float kLongestStep = 3.0f;
// The things met in one attack that are kept apart.
constexpr std::size_t kMostThings = 32;

using ContactCheck = char(__fastcall*)(void** checker, float dt);
using Component = void*(__fastcall*)(void* entity, unsigned id);
using GetLocalBounds = void(__fastcall*)(void* render, float* bounds);

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

Natives natives;
Logger log_line = nullptr;
ContactCheck original_contact_check = nullptr;

// What the hook saw: the actor whose blade was live last, the solver's check object and how many
// live frames there have been.
std::atomic<void*> live_actor{nullptr};
std::atomic<void*> live_checker{nullptr};
std::atomic<std::uint64_t> live_frames{0};

// The attack being traced (engine thread).
struct Attack {
  void* checker = nullptr;
  std::uint64_t frames_seen = 0;
  double last_live = -1;
  bool has_blade = false;
  float hand[3] = {0, 0, 0};
  float tip[3] = {0, 0, 0};
  std::vector<void*> things;
  bool terrain = false;
};
Attack attack;

char __fastcall contact_check(void** checker, float dt) {
  const char found = original_contact_check(checker, dt);
  void* context = checker ? *checker : nullptr;
  void* owner = context ? *static_cast<void**>(context) : nullptr;
  if (owner) {
    live_actor.store(owner);
    live_checker.store(checker);
    live_frames.fetch_add(1);
  }
  return found;
}

float distance_between(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Where the weapon's hand and blade tip are now; false when the weapon draws no blade.
bool blade_of(void* weapon, float hand[3], float tip[3]) {
  const float* m = slot<const float*(__fastcall*)(void*)>(weapon, kGetWorldTM)(weapon);
  void* render = natives.component
                     ? reinterpret_cast<Component>(natives.component)(weapon, kRenderComponent)
                     : nullptr;
  if (!m || !render) {
    return false;
  }
  float bounds[6] = {1, 1, 1, 0, 0, 0};
  slot<GetLocalBounds>(render, kGetLocalBounds)(render, bounds);
  const float reach = bounds[5];
  if (!(reach >= kShortestBlade && reach <= kLongestBlade)) {
    return false;
  }
  // The entity's local +Z in the world is its matrix's third column.
  const float axis[3] = {m[2], m[6], m[10]};
  hand[0] = m[3];
  hand[1] = m[7];
  hand[2] = m[11];
  for (int i = 0; i < 3; ++i) {
    tip[i] = hand[i] + axis[i] * (reach + kPastTheTip);
  }
  return true;
}

void* physics_of(void* entity) {
  return entity ? slot<void*(__fastcall*)(void*)>(entity, kGetPhysics)(entity) : nullptr;
}

// True the first time this attack meets the thing.
bool first_time(const physics::RayHit& hit) {
  if (hit.terrain) {
    const bool first = !attack.terrain;
    attack.terrain = true;
    return first;
  }
  if (!hit.collider) {
    return false;
  }
  if (std::find(attack.things.begin(), attack.things.end(), hit.collider) != attack.things.end()) {
    return false;
  }
  if (attack.things.size() < kMostThings) {
    attack.things.push_back(hit.collider);
  }
  return true;
}

// Traces one segment of the blade's path; a thing met for the first time is a contact.
void trace(const float from[3], const float to[3], void* const* skip, int skip_count,
           const float feet[3], std::vector<Contact>& contacts) {
  if (!(distance_between(from, to) > 1e-3f)) {
    return;
  }
  physics::RayHit hit;
  const auto result = physics::trace(from, to, physics::kHitCover, skip, skip_count, hit);
  if (result != physics::TraceResult::Hit || !first_time(hit)) {
    return;
  }
  contacts.push_back(Contact{hit, distance_between(hit.position, feet)});
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.contact_check || !natives.component) {
    return false;
  }
  return MH_CreateHook(natives.contact_check, reinterpret_cast<void*>(contact_check),
                       reinterpret_cast<void**>(&original_contact_check)) == MH_OK &&
         MH_EnableHook(natives.contact_check) == MH_OK;
}

std::vector<Contact> tick(void* actor, void* entity, void* weapon, double now) {
  std::vector<Contact> contacts;
  const std::uint64_t frames = live_frames.load();
  const bool live = actor && frames != attack.frames_seen && live_actor.load() == actor;
  attack.frames_seen = frames;
  if (!live) {
    if (attack.last_live >= 0 && now - attack.last_live > kAttackGap) {
      attack = Attack{};
      attack.frames_seen = frames;
    }
    return contacts;
  }
  void* checker = live_checker.load();
  if (checker != attack.checker || now - attack.last_live > kAttackGap) {
    attack = Attack{};
    attack.frames_seen = frames;
    attack.checker = checker;
  }
  attack.last_live = now;
  float hand[3];
  float tip[3];
  if (!weapon || !blade_of(weapon, hand, tip)) {
    attack.has_blade = false;
    return contacts;
  }
  const float* m =
      entity ? slot<const float*(__fastcall*)(void*)>(entity, kGetWorldTM)(entity) : nullptr;
  if (!m) {
    return contacts;
  }
  const float feet[3] = {m[3], m[7], m[11]};
  // The player's own physics and the weapon's are never met.
  void* skip[2] = {nullptr, nullptr};
  int skipping = 0;
  for (void* own : {physics_of(entity), physics_of(weapon)}) {
    if (own && (skipping == 0 || skip[0] != own)) {
      skip[skipping] = own;
      ++skipping;
    }
  }
  // The tip's path since the frame before, then the blade from the hand to the tip.
  if (attack.has_blade && distance_between(attack.tip, tip) <= kLongestStep) {
    trace(attack.tip, tip, skip, skipping, feet, contacts);
  }
  trace(hand, tip, skip, skipping, feet, contacts);
  std::memcpy(attack.hand, hand, sizeof(hand));
  std::memcpy(attack.tip, tip, sizeof(tip));
  attack.has_blade = true;
  if (log_line) {
    for (const Contact& contact : contacts) {
      log_line("WEAPON HIT %s at (%.2f %.2f %.2f) %.2f m from the feet",
               contact.hit.terrain ? "terrain" : "thing", contact.hit.position[0],
               contact.hit.position[1], contact.hit.position[2], contact.distance);
    }
  }
  return contacts;
}

}  // namespace kcds::runtime::weapon_hits
