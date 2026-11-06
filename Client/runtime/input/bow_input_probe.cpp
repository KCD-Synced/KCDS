// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "bow_input_probe.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../inventory/quickslots.hpp"
#include "../inventory/stock_projection.hpp"
#include "../ranged/shots.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "kcd2s/integration/guid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::game_client {
namespace {

using BowCallback = void(__fastcall*)(void* capture);
using ShootingExpansion = void*(__fastcall*)(void* expansions);
using Settings = void*(__fastcall*)();

// C_MissileWeaponPlayerController::RegisterActions captures these exact
// two pointers in each std::function<void()>: controller at +8, shooting expansion at +16.
// The four full invokers never read the first word, retain this capture, or own its storage.
struct BowCapture {
  void* unused = nullptr;
  void* controller = nullptr;
  void* expansion = nullptr;
};
static_assert(offsetof(BowCapture, controller) == 8);
static_assert(offsetof(BowCapture, expansion) == 16);

constexpr char kPrimary[] =
    "40 53 48 83 EC 20 48 8B 41 08 48 8B D9 80 78 20 00 75 36 E8 ?? ?? ?? ?? 83 B8 "
    "C4 00 00 00 01 74 28 48 8B 43 08 C6 40 21 01";
constexpr char kPrimaryRelease[] =
    "40 53 48 83 EC 20 48 8B 41 08 48 8B D9 80 78 21 00 74 2F C6 40 20 00 45 33 C0 "
    "48 8B 41 08 B2 01 C6 40 21 00 48 8B 49 08";
constexpr char kSecondary[] =
    "40 53 48 83 EC 20 48 8B 41 08 48 8B D9 80 78 20 00 75 2E E8 ?? ?? ?? ?? 83 B8 "
    "C4 00 00 00 00 74 20 48 8B 53 10 48 8B 4B 08";
constexpr char kSecondaryRelease[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B 49 08 80 79 20 00 74 25 80 79 21 00 B2 01 "
    "41 0F 94 C0 E8 ?? ?? ?? ?? 48 8B 43 08 C6 40 20 00";

struct Bindings {
  BowCallback primary = nullptr;
  BowCallback primary_release = nullptr;
  BowCallback secondary = nullptr;
  BowCallback secondary_release = nullptr;
  ShootingExpansion expansion = nullptr;
  Settings settings = nullptr;
  bool attempted = false;

  bool ready() const {
    return primary && primary_release && secondary && secondary_release && expansion && settings;
  }
};

Bindings bindings;

bool enabled() {
  wchar_t value[4]{};
  return GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", value, 4) == 1 && value[0] == L'1';
}

// Only diagnostic reads are guarded. Native callbacks/getters below run outside these guards;
// their exceptions propagate to the engine adapter's existing fault handling.
template <class Value>
bool read(const void* object, std::size_t offset, Value& value) {
  if (!object || offset > UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(object)) {
    return false;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(object) + offset;
  __try {
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool resolve() {
  if (!image || image_size == 0) {
    return false;
  }
  if (!bindings.attempted) {
    bindings.attempted = true;
    // full invokers, not thunks.
    bindings.primary = reinterpret_cast<BowCallback>(resolve_unique("TestBowPrimary", kPrimary));
    bindings.primary_release =
        reinterpret_cast<BowCallback>(resolve_unique("TestBowPrimaryRelease", kPrimaryRelease));
    bindings.secondary =
        reinterpret_cast<BowCallback>(resolve_unique("TestBowSecondary", kSecondary));
    bindings.secondary_release =
        reinterpret_cast<BowCallback>(resolve_unique("TestBowSecondaryRelease", kSecondaryRelease));
    // Same already-verified native getter as ranged/shots.
    bindings.expansion = reinterpret_cast<ShootingExpansion>(resolve_unique(
        "TestBowShootingExpansion", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 B2 04"));
    // Primary's call at +0x13 is the native settings getter; no literal resolver.
    bindings.settings =
        reinterpret_cast<Settings>(resolve_unique("TestBowSettings", kPrimary, 20, 24));
  }
  return bindings.ready();
}

bool is_fixture_bow(void* weapon) {
  void* item_class = nullptr;
  kcd2s::integration::Guid actual_class{};
  kcd2s::integration::Guid expected_class{};
  kcd2s::integration::parse_guid("5c23394a-3300-4570-a8b7-ef1c11519047", expected_class);
  std::uint64_t handle = 0;
  std::uint64_t revision = 0;
  std::string row;
  return weapon && read(weapon, 0x48, item_class) && read(item_class, 8, actual_class) &&
         actual_class == expected_class && kcds::runtime::ranged::is_missile_weapon(weapon) &&
         read(weapon, 0x30, handle) && kcds::runtime::inventory::row_of(handle, row, revision) &&
         row == "many487:weapon";
}

bool refused(const char* action, const char* reason) {
  log("INPUT bow probe refused action=%s reason=%s", action, reason);
  return true;
}

}  // namespace

bool scripted_bow_probe(const char* action, unsigned mode) {
  if (!action) {
    return false;
  }
  const bool primary = std::strcmp(action, "bow_primary") == 0;
  const bool secondary = std::strcmp(action, "bow_secondary") == 0;
  const bool primary_release = std::strcmp(action, "bow_primary_release") == 0;
  const bool secondary_release = std::strcmp(action, "bow_secondary_release") == 0;
  const bool state = std::strcmp(action, "bow_probe_state") == 0;
  const bool draw = std::strcmp(action, "bow_draw_fixture") == 0;
  const bool holster = std::strcmp(action, "bow_holster_fixture") == 0;
  const bool releasing =
      primary_release || secondary_release || ((primary || secondary) && mode == 2);
  if (!primary && !secondary && !primary_release && !secondary_release && !state && !draw &&
      !holster) {
    return false;
  }
  if (!enabled()) {
    return refused(action, "KCDS_TEST_HOOKS must equal 1");
  }
  if (mode != 1 && mode != 2) {
    return refused(action, "only press/release modes supported");
  }
  if (mode == 2 && (primary_release || secondary_release || state || draw || holster)) {
    return true;
  }
  if (!resolve() || !get_services || !local_network_id.load() || !local_engine_id.load()) {
    return refused(action, "native signatures or world unavailable");
  }

  void* services = get_services();
  void* game_interface = nullptr;
  if (!read(services, 8, game_interface) || !game_interface) {
    return refused(action, "game interface unavailable");
  }
  // Existing local-player identity contract used by frame.cpp and replica_role_install.cpp.
  void* actor = slot<void*(__fastcall*)(void*)>(game_interface, 64)(game_interface);
  std::uint32_t actor_id = 0;
  if (!read(actor, 48, actor_id) || actor_id != local_engine_id.load()) {
    return refused(action, "local actor identity mismatch");
  }
  auto* controller = static_cast<std::uint8_t*>(actor) + 3440;
  void* controller_actor = nullptr;
  void* expansions = nullptr;
  if (!read(controller, 24, controller_actor) || controller_actor != actor ||
      !read(actor, 640, expansions) || !expansions) {
    return refused(action, "local controller or expansion owner mismatch");
  }
  void* expansion = bindings.expansion(expansions);
  void* weapon = nullptr;
  void* native_action = nullptr;
  if (!read(expansion, 64, weapon) || !read(expansion, 80, native_action)) {
    return refused(action, "shooting expansion unavailable");
  }
  const bool fixture_bow = is_fixture_bow(weapon);
  const bool held = fixture_bow && kcds::runtime::weapons::item_in_hand(actor, true) == weapon;
  kcd2s::integration::InventoryQuickslots quickslots{};
  const bool slots_known = kcds::runtime::inventory::read_quickslots(actor, quickslots);
  std::uint32_t control_mode = 0;
  std::uint8_t pressed = 0;
  std::uint8_t primary_held = 0;
  // The game's own callbacks read the mode themselves: primary ignores mode 1, secondary mode 0.
  // Any other value (2 is usual) lets both through, so the mode is only logged here.
  if (!read(bindings.settings(), 196, control_mode) || !read(controller, 32, pressed) ||
      !read(controller, 33, primary_held)) {
    return refused(action, "unreadable bow state");
  }
  log("INPUT bow probe state controls=%u pressed=%u primaryHeld=%u weaponState=%u "
      "fixtureBow=%u held=%u slotKnown=%u slot=%u activeAction=%u",
      control_mode, unsigned(pressed), unsigned(primary_held),
      unsigned(kcds::runtime::ranged::weapon_state(actor)), unsigned(fixture_bow), unsigned(held),
      unsigned(slots_known), unsigned(quickslots.active_weapon),
      unsigned(native_action != nullptr));
  if (state) {
    return true;
  }
  if (draw || holster) {
    if (pressed || primary_held) {
      return refused(action, "release both bow controls before draw or holster");
    }
    void* left = kcds::runtime::weapons::item_in_hand(actor, true);
    void* right = kcds::runtime::weapons::item_in_hand(actor, false);
    if ((left && !is_fixture_bow(left)) || (right && !is_fixture_bow(right))) {
      return refused(action, "another held item must be put away through its own native path");
    }
    if (draw) {
      if (!slots_known || quickslots.active_weapon != 0 ||
          quickslots.primary[0] != "many487:weapon") {
        return refused(action, "fixture bow row must occupy selected quickslot 0");
      }
      if (left || right) {
        log("INPUT bow probe draw no-op: fixture already held; verify ready state");
        return true;
      }
      const bool sent = kcds::runtime::weapons::draw_set_item(actor, is_fixture_bow);
      log("INPUT bow probe draw request=%u; actual left-hand/ready state must be observed",
          unsigned(sent));
      return true;
    }
    const bool sent = kcds::runtime::weapons::holster(actor);
    log("INPUT bow probe holster request=%u; actual empty hands must be observed", unsigned(sent));
    return true;
  }
  if (releasing) {
    // Holstering or changing the selected slot must not strand a held controller flag.
    // With no weapon and no action, native all return false;
    // release clears the flags/UI mapping without touching a weapon. A live action with
    // no weapon or a different weapon is refused, because native cancel can dereference it.
    if (!fixture_bow && (weapon || native_action)) {
      return refused(action, "release requires fixture bow or an empty shooting expansion");
    }
  } else {
    if (!held) {
      return refused(action, "catalog bow_a must already be drawn in the local left hand");
    }
    if (!slots_known || quickslots.active_weapon != 0 ||
        quickslots.primary[0] != "many487:weapon") {
      return refused(action, "fixture bow row must occupy selected quickslot 0");
    }
  }

  BowCallback invoke = primary     ? (mode == 1 ? bindings.primary : bindings.primary_release)
                       : secondary ? (mode == 1 ? bindings.secondary : bindings.secondary_release)
                       : primary_release ? bindings.primary_release
                                         : bindings.secondary_release;
  BowCapture capture{nullptr, controller, expansion};
  // Exactly once, synchronously on the engine thread. The native full invoker handles its
  // own control mode, flags, release/cancel, UI map changes and original shooting logic.
  invoke(&capture);
  log("INPUT bow probe native callback action=%s mode=%u", action, mode);
  return true;
}

}  // namespace kcds::game_client
