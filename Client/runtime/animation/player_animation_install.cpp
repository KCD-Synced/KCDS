// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "player_animation_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../native_ui/native_ui.hpp"
#include "../seats/seat_reservations.hpp"
#include "player_animation.hpp"
#include "female_retarget.hpp"
#include "no_gravity.hpp"
#include "pointing.hpp"
#include "../input/interaction_rules.hpp"
#include "kcd2s/integration/interaction_rules.hpp"
#include "../chat/chat_overlay.hpp"
#include "upper_body.hpp"

namespace kcds::game_client {

void install_player_animation() {
  // The server's playAnimation by the game's Mannequin, as its flow node
  // "Actor:PlayMannequinFragment" plays a fragment.
  kcds::runtime::player_animation::Natives natives;
  natives.allocate =
      resolve_unique("GameAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0");
  natives.construct = resolve_unique("AnimationAction", "45 33 D2 C7 41 14 00 00 80 BF 4C 89");
  natives.assign =
      resolve_unique("ActionReference",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 09 48 3B D1 75 09 48 8B C3 48 83 C4 20 "
                     "5B C3 48 89 13 48 85 D2 74 04 F0 FF 42 58 48 85");
  natives.stop = resolve_unique("ActionForceFinish",
                                "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 61 2C FB 33");
  natives.find_tag = resolve_unique("TagDefinitionFind", "4C 8B C9 48 85 D2 74 48 41 83 C8 FF");
  natives.set_tag = resolve_unique("TagStateSet", "45 8A D1 4C 8B D9 45 85 C0 78 7C 48");
  natives.set_global_tag =
      resolve_unique("ContextTagStateSet", "8B C2 45 8A C8 48 8D 51 08 44 8B C0");
  natives.services = reinterpret_cast<void*>(get_services);
  natives.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  natives.progress = [](void* skeleton, const void* animation) {
    return native_animation_progress ? native_animation_progress(skeleton, animation) : 0.0f;
  };
  natives.log = log;
  natives.seated = []() { return !kcds::runtime::seats::local_seats().empty(); };
  // Clips a resource ships: CAnimationSet::LoadFileCAF (its `mov edi, [rip+rel32]`
  // at + 67 reads the cvar ca_UseIMG_CAF's int) and the name hash it keys clips by.
  if (auto load_caf = static_cast<std::uint8_t*>(resolve_unique(
          "AnimationSetLoadFileCAF",
          "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 30 "
          "4C 8B F2 4C 8B F9 49 8B D0 48 83 C1 28 49 8B F1 49 8B E8 E8 ?? ?? ?? ?? 48 83 CB FF "
          "3B C3 74 07 8B C3 E9 ?? ?? ?? ?? 8B 3D"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, load_caf + 69, 4);
    natives.load_file_caf = load_caf;
    natives.use_img_caf = reinterpret_cast<int*>(load_caf + 73 + displacement);
  }
  natives.name_hash = resolve_unique("AnimationNameHash",
                                     "48 89 5C 24 08 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 "
                                     "48 83 EC 60 8B FA 48 8B E9 83 FA 20 76 24 83 FF 40 77 31 8B "
                                     "D7 E8");
  natives.mount_shipped = &kcds::runtime::native_ui::mount_shipped;
  // A paired request's slave: the body's world matrix (CEntity GetWorldTM, + 256) and the
  // replication's move.
  natives.body_place = [](void* entity, float q[4], float t[3]) {
    using GetWorldTM = const float*(__fastcall*)(void* entity);
    const float* m = slot<GetWorldTM>(entity, 32)(entity);
    if (!m) {
      return false;
    }
    const core::Quat turn = rotation(m);
    q[0] = turn.x;
    q[1] = turn.y;
    q[2] = turn.z;
    q[3] = turn.w;
    t[0] = m[3];
    t[1] = m[7];
    t[2] = m[11];
    return true;
  };
  natives.move_body = [](void* entity, const float q[4], const float t[3]) {
    set_transform(entity, core::Vec3{t[0], t[1], t[2]}, core::Quat{q[0], q[1], q[2], q[3]});
  };
  // The upper-body clips' legs: a character's animation pass, where it reads the
  // modifiers pushed for its layers.
  kcds::runtime::animation::upper_body::install(
      resolve_unique("CharacterAnimationPass",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 60 48 "
                     "8B DA 4D 8B F0 48 8B D1 48 8B 0B 48 81 C1 A4 0A 00 00 E8"),
      log);
  // A player woman on the male clips: a modifier the animation pass pushes.
  kcds::runtime::animation::female_retarget::install(log);
  // Pointing: the B key by its place on the keyboard, while the game has the keyboard.
  kcds::runtime::animation::pointing::Natives pointing;
  pointing.camera = [](float matrix[12]) {
    if (!view_matrix_valid) {
      return false;
    }
    std::copy(view_matrix.begin(), view_matrix.end(), matrix);
    return true;
  };
  pointing.key_down = [] {
    constexpr unsigned kPointKeyScan = 0x30;
    if (GetForegroundWindow() != static_cast<HWND>(kcd2s::native_chat::window()) ||
        ui_owns_input() || page_over_world.load() || game_menu_open()) {
      return false;
    }
    const UINT key = MapVirtualKeyExW(kPointKeyScan, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
    return key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
  };
  pointing.allowed = [] {
    return !kcds::runtime::input::interaction_rules::allowed(kcd2s::integration::kRefusePointing);
  };
  pointing.log = log;
  kcds::runtime::animation::pointing::install(pointing);
  // A request without gravity: the body's physics off while it runs.
  kcds::runtime::animation::no_gravity::install(
      resolve_unique(
          "EntityPhysicsComponent",
          "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 8D 99 A8 00 00 00"),
      log);
  const bool animation = kcds::runtime::player_animation::install(natives);
  log("ANIMATION player animation %s",
      animation ? "installed" : "unavailable (playAnimation requests are refused)");
}

}  // namespace kcds::game_client
