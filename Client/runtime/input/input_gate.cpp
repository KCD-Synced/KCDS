#include "input_gate.hpp"
#include "bow_input_probe.hpp"

#include "../attach/player_attach.hpp"
#include "../camera/free_look.hpp"
#include "../camera/third_person.hpp"
#include "../action_hints/resource_action_hints.hpp"
#include "../carts/placed_carts.hpp"
#include "../chat/chat_overlay.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../dialogue/dialogue_list.hpp"
#include "../follow/follow.hpp"
#include "../npcs/npc_use.hpp"
#include "../identity/licence_pass.hpp"
#include "../inventory/stock_projection.hpp"
#include "../movement/shared_ladders.hpp"
#include "../movement/walk_default.hpp"
#include "../native_ui/native_ui.hpp"
#include "../noclip/free_camera.hpp"
#include "../props/placer.hpp"
#include "../riding/horse_saddlebags.hpp"
#include "../web/resource_web.hpp"
#include "cuffs.hpp"
#include "frozen.hpp"
#include "interaction_rules.hpp"
#include "focus_release.hpp"
#include "resource_input.hpp"
#include "scripted_input.hpp"

#include <intrin.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

// The scripted input's own actions (KCDS_TEST_HOOKS only), keys the player input does not get:
//   close_inventory_screen the screen's close key (Esc).
//   open_inventory_screen  the inventory key (open_apse_inventory_keyboard): its callback
//                          (a std::function the inventory screen's owner registers
//                          in) reads only its captured owner at + 8; the owner keeps
//                          itself + 16 at services + 432.
//   open_map_screen        the map key (open_apse_map_keyboard): its callback
//                          (registered beside the inventory's) opens the same owner's map page.
// The inventory screen's panel of the player's own items (the one whose + 2736 is the local
// player's inventory, + 2744 their horse's): one of the panels the screen keeps in itself (their
// offsets from the screen's start), as the move_item key's callback gets it.
void* inventory_panel(std::uint8_t* screen_owner) {
  constexpr std::size_t kPanels[] = {27, 372, 537, 673, 907, 976, 1067, 1208, 1262};
  constexpr std::size_t kPanelPlayerInventory = 2736;
  void* player = kcds::runtime::inventory::local_player_inventory();
  if (!player) {
    return nullptr;
  }
  for (const std::size_t slot : kPanels) {
    std::uint8_t* panel = screen_owner + slot * 8;
    if (*reinterpret_cast<void**>(panel + kPanelPlayerInventory) == player) {
      return panel;
    }
  }
  return nullptr;
}

// hint_press: what the use key does at the prompt on screen. The HUD's action handler (the player
// module at services + 296, its + 88) keeps a std::map of its hints at + 48, as the interactor
// registers them: a tree node's flag "head" at + 25, the hint at + 32 with
// its action's name first, its std::function at + 256 (the callable's pointer 56 bytes on). Called
// on the action named "use", or the only one shown, the way a key press reaches it.
void find_shown_hint(const void* node, const void* head, const char* wanted,
                     std::vector<void*>& found) {
  if (node == head) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(node);
  find_shown_hint(*reinterpret_cast<void* const*>(bytes), head, wanted, found);
  const char* name = *reinterpret_cast<const char* const*>(bytes + 32);
  void* callable = *reinterpret_cast<void* const*>(bytes + 256 + 56);
  log("INPUT hint shown %s callable=%p", name ? name : "?", callable);
  if (callable && name && std::strcmp(name, wanted) == 0) {
    found.push_back(callable);
  }
  find_shown_hint(*reinterpret_cast<void* const*>(bytes + 16), head, wanted, found);
}

// The local player's actor this frame (engine thread), for press_player_action.
void* input_actor = nullptr;

bool scripted_special(const char* action, unsigned mode) {
  // toggle_torch: the torch key's own handler (C_PlayerInput's action in the map
  // draw_holster_torch, called with the player's input). Its action is not in the registry the
  // other steps use.
  if (std::strcmp(action, "toggle_torch") == 0) {
    if (mode == 1 && input_actor) {
      using Toggle = void(__fastcall*)(void* input);
      static auto toggle = reinterpret_cast<Toggle>(resolve_unique(
          "TorchToggle",
          "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 10 48 8B F9 33 D2 48 8B 98 90 09 00 00 48 8B CB "
          "48 8B 03 FF 90 98 02 00 00 84 C0 75 36 48 8B 03 B2 0B 48"));
      constexpr std::size_t kActorPlayerInput = 2952;
      void* input =
          *reinterpret_cast<void**>(static_cast<std::uint8_t*>(input_actor) + kActorPlayerInput);
      if (toggle && input) {
        toggle(input);
      }
    }
    return true;
  }
  if (std::strncmp(action, "hint_", 5) == 0) {
    if (mode == 1) {
      auto* services = static_cast<std::uint8_t*>(get_services());
      auto* module = *reinterpret_cast<std::uint8_t**>(services + 296);
      auto* handler = *reinterpret_cast<std::uint8_t**>(module + 88);
      auto* head = *reinterpret_cast<std::uint8_t**>(handler + 48);
      // Pressed after the walk: an action may change the map it is kept in.
      std::vector<void*> found;
      find_shown_hint(*reinterpret_cast<void**>(head + 8), head, action + 5, found);
      using Call = void(__fastcall*)(void*);
      for (void* callable : found) {
        (*static_cast<Call**>(callable))[2](callable);
      }
      log("INPUT hint %s pressed %zu", action + 5, found.size());
    }
    return true;
  }
  // camera_mouse: a mouse event straight to the third-person camera's listener.
  if (std::strcmp(action, "camera_mouse") == 0) {
    kcds::runtime::camera::third_person::test_mouse_event(float(mode));
    return true;
  }
  // free_look_turn: the free look's view turns this many degrees, as the mouse would.
  if (std::strcmp(action, "free_look_turn") == 0) {
    kcds::runtime::camera::free_look::test_turn(double(mode));
    return true;
  }
  if (scripted_bow_probe(action, mode)) {
    return true;
  }
  // licence_probe: the licence pass flow (identity/licence_pass.hpp) against a Master on this
  // machine (127.0.0.1:8099), on its own thread. Its synthetic binding is for
  // diagnostics only and cannot authorize a real server handshake.
  if (std::strcmp(action, "licence_probe") == 0) {
    if (mode == 1) {
      std::thread([] {
        log("LICENCE diagnostic proof request");
        const auto binding = kcd2s::transport::licence_binding(local::nonce());
        kcds::runtime::identity::licence_pass("http://127.0.0.1:8099", "203.0.113.9:7777", "probe",
                                              binding, log);
      }).detach();
    }
    return true;
  }
  // take_from_horse: one unit of the first item of a horse's saddlebags moved to the player by the
  // game's own move, as the inventory screen's Move does (riding/horse_saddlebags.hpp).
  if (std::strcmp(action, "take_from_horse") == 0) {
    if (mode == 1) {
      log("INPUT script take_from_horse %s",
          kcds::runtime::riding::saddlebags::test_take_first() ? "moved" : "nothing to take");
    }
    return true;
  }
  const bool open_screen = std::strcmp(action, "open_inventory_screen") == 0;
  const bool open_map = std::strcmp(action, "open_map_screen") == 0;
  const bool move_item = std::strcmp(action, "move_selected_item") == 0;
  const bool close_screen = std::strcmp(action, "close_inventory_screen") == 0;
  if (!open_screen && !open_map && !move_item && !close_screen) {
    return false;
  }
  if (mode != 1) {
    return true;
  }
  constexpr std::size_t kServicesInventoryScreen = 432;
  constexpr std::size_t kScreenInterface = 16;
  using KeyCallback = void(__fastcall*)(void* callback);
  // 68 bytes: shorter, the prologue is shared with four other key callbacks.
  static const char* const kSignature =
      "4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 "
      "C0 00 00 00 48 8B 79 08 49 8D 53 98 48 8D 4C 24 20 48 8B 47 10 49 C7 43 D8 00 00 00 00 "
      "48 8B 58 08 48 8B 05 53 83 3D 03";
  static auto open =
      reinterpret_cast<KeyCallback>(resolve_unique("InventoryKeyCallback", kSignature));
  // The map key's callback: the same prologue, then the map page's id (qword_185326308).
  static auto open_map_page = reinterpret_cast<KeyCallback>(resolve_unique(
      "MapKeyCallback",
      "4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 "
      "C0 00 00 00 48 8B 79 08 49 8D 53 98 48 8D 4C 24 20 48 8B 47 10 49 C7 43 D8 00 00 00 00 "
      "48 8B 58 08 48 8B 05 43 89 3D 03"));
  auto* screen = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) +
                                                   kServicesInventoryScreen);
  if (!open || !screen) {
    return true;
  }
  if (close_screen) {
    // The screen's close key ("close" of the apse map): its callback calls the
    // screen's method 2 with 0.
    using Close = void(__fastcall*)(void* screen, std::uint64_t how);
    (*reinterpret_cast<Close**>(screen))[2](screen, 0);
    return true;
  }
  if (move_item) {
    // The move_item key (X, Move to horse) on the selected item: its callback reads
    // only its captured panel at + 8.
    static auto move = reinterpret_cast<KeyCallback>(resolve_unique(
        "MoveItemKeyCallback",
        "48 8B 41 08 80 B8 1A 02 00 00 00 75 12 66 C7 80 1A 02 00 00 01 01 48 8B 49 08 E9 61 "
        "17 BB 00"));
    void* panel = inventory_panel(screen - kScreenInterface);
    log("INPUT script move_selected_item panel=%p", panel);
    if (move && panel) {
      void* callback[2] = {nullptr, panel};
      move(callback);
    }
    return true;
  }
  // The std::function object the callback is called on: its vtable, then the captured owner.
  void* callback[2] = {nullptr, screen - kScreenInterface};
  if (open_map) {
    if (open_map_page) {
      open_map_page(callback);
    }
    return true;
  }
  open(callback);
  return true;
}

// Scripted game input for live tests (input/scripted_input.hpp, KCDS_TEST_HOOKS only): the due
// steps go through the local player's input (C_PlayerInput, actor + 2952, whose actor is at + 16)
// as key presses do, with the actions of the game's action registry (services + 48).
void tick_scripted_input(void* local_actor, double now) {
  input_actor = local_actor;
  constexpr std::size_t kActorPlayerInput = 2952;
  constexpr std::size_t kPlayerInputActor = 16;
  void* input =
      local_actor
          ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + kActorPlayerInput)
          : nullptr;
  if (input && *reinterpret_cast<void**>(static_cast<std::uint8_t*>(input) + kPlayerInputActor) !=
                   local_actor)
    input = nullptr;
  auto registry =
      *reinterpret_cast<const std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
  kcds::runtime::input::scripted::tick(registry, input, now, hooked_player_input_action,
                                       scripted_special);
  // Movement keys held at a switch to another window are let go (input/focus_release.hpp).
  kcds::runtime::input::focus::tick(hooked_player_input_action, log);
  keep_sitting_map();
}

// The game's seat action map held off while a resource blocks a seat's exits
// (input/resource_input.hpp sitting_map_held): turned off every frame, as the seat turns it on
// when the player sits (and could again), and on once when the block ends. The exits are in the
// include map "sitting", which only "sitting_chair" and "sitting_bed" include
// (defaultProfile.xml); turning the include off did nothing, "sitting_chair" is the map the
// game turns on. With it go the seat's camera keys and script_action (U): the client binds U as a
// key of its own then (Client/resources/tabletop.cjs). The action map manager's
// EnableActionMap(name, enable), as free look turns "player" off (camera/free_look.cpp):
// services + 8 the input module, its slot 224 / 8 the manager, the manager's slot 232 / 8.
void keep_sitting_map() {
  static bool held = false;
  const bool wanted = kcds::runtime::input::sitting_map_held();
  if (!wanted && !held) {
    return;
  }
  constexpr std::size_t kServicesInput = 8;
  constexpr std::size_t kActionMaps = 224;
  constexpr std::size_t kEnableActionMap = 232;
  using GetMaps = void*(__fastcall*)(void*);
  using EnableActionMap = void(__fastcall*)(void*, const char*, bool);
  void* services = get_services();
  void* input =
      services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesInput)
               : nullptr;
  void* maps = input ? slot<GetMaps>(input, kActionMaps / 8)(input) : nullptr;
  if (!maps) {
    return;
  }
  slot<EnableActionMap>(maps, kEnableActionMap / 8)(maps, "sitting_chair", !wanted);
  if (wanted != held) {
    log("INPUT sitting_chair action map %s", wanted ? "held off" : "back on");
  }
  held = wanted;
}

void press_player_action(const char* action, unsigned mode, float value) {
  constexpr std::size_t kActorPlayerInput = 2952;
  void* actor = input_actor;
  void* input =
      actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorPlayerInput)
            : nullptr;
  auto registry =
      *reinterpret_cast<const std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
  if (!input || !registry) {
    return;
  }
  const char** id = kcds::runtime::input::scripted::action_named(registry, action);
  if (id) {
    hooked_player_input_action(input, id, mode, value);
  }
}

// MCP: C_PlayerInput::OnAction(self, ActionId const&, mode, value).
// Suppressed holds receive the original release once. Physical release clears the gate,
// so keys held when chat opens cannot resume attacks while a line is being typed.
PlayerInputAction original_player_input_action;
thread_local std::unordered_set<std::string> chat_suppressed_actions;
// The chat input, a focused web view or a focused native screen owns keyboard and mouse.
bool ui_owns_input() {
  return native_chat::editing() || kcds::runtime::web::has_input_focus() ||
         kcds::runtime::native_ui::has_focus();
}
bool camera_mouse_blocked() {
  return ui_owns_input() || kcds::runtime::input::controls_disabled() ||
         kcds::runtime::input::controls_cursor() ||
         kcds::runtime::input::action_disabled("rotateyaw");
}
// Game actions are suppressed while the UI owns the input or a resource holds Controls.disable.
bool game_actions_blocked() {
  return ui_owns_input() || kcds::runtime::input::controls_disabled();
}
void* __fastcall hooked_player_input_action(void* self, const char** action, unsigned mode,
                                            float value) {
  const bool local = self && *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 16) ==
                                 local_combat_actor.load();
  if (!local || !action || !*action)
    return original_player_input_action(self, action, mode, value);
  // A game action a resource holds off does nothing (Controls.blockAction), and nor
  // does the release of a press held off; the release of a key held when it was disabled still
  // goes through, so it does not stay down (input/resource_input.hpp hold_off).
  if (kcds::runtime::input::hold_off(*action, mode)) {
    return nullptr;
  }
  // A server conversation takes the movement and use actions for its list, before a
  // hint or an NPC on the same key sees them.
  if (kcds::runtime::dialogue::on_action(*action, mode))
    return nullptr;
  // A script's hint on the key takes the press.
  if (kcds::runtime::action_hints::on_player_action(*action, mode)) {
    return nullptr;
  }
  kcds::runtime::movement::walk_default::on_action(*action, mode);
  kcds::runtime::input::focus::on_action(self, action, mode);
  // A follow the player may not end takes its keys (Follow.start playerCanStop).
  if (kcds::runtime::follow::on_action(*action, mode)) {
    return nullptr;
  }
  kcds::runtime::npcs::use::on_action(*action, mode);
  if (kcds::runtime::movement::ladders::refuse_action(self, action, mode))
    return nullptr;
  // A cuffed player only walks and looks around (Player.setCuffed).
  if (kcds::runtime::input::cuffs::refuse_action(*action, mode))
    return nullptr;
  // A player attached to another only looks around (Player.attachTo).
  if (kcds::runtime::attach::refuse_action(*action, mode))
    return nullptr;
  // A frozen player only looks around (Player.frozen).
  if (kcds::runtime::input::frozen::refuse_action(*action, mode))
    return nullptr;
  // The game's own interactions a KCDS server has not allowed (the whistle).
  if (kcds::runtime::input::interaction_rules::refuse_action(*action, mode)) {
    log("INTERACTIONS %s refused: the server has not allowed it", *action);
    return nullptr;
  }
  // The driver of a harnessed cart drives it with the movement actions.
  if (kcds::runtime::carts::placed::take_action(*action, mode))
    return nullptr;
  // A player placing a prop only walks and looks around; the mouse buttons place.
  if (kcds::runtime::props::placer::refuse_action(*action, mode))
    return nullptr;
  // MCP 8E5324 dispatches combat input through global CombatModule+40.
  // Engine input can arrive before our update restores the local selector.
  // Correct only a tracked replica selection; preserve original action gates.
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto own_movement = *reinterpret_cast<void**>(local_actor + 632);
  auto combat = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 248);
  auto selected = combat ? *reinterpret_cast<void**>(combat + 40) : nullptr;
  if (selected != own_movement && remote_combat_id(selected)) {
    log("CONTROL input owner corrected selected=%p own=%p name=%.64s mode=%u tick=%llu", selected,
        own_movement, *action, mode, GetTickCount64());
    retain_local_input(local_actor);
  }
  auto invoke_original = [&](unsigned input_mode, float input_value) {
    auto result = original_player_input_action(self, action, input_mode, input_value);
    if ((input_mode == 1 || input_mode == 2) && std::strncmp(*action, "move", 4) == 0) {
      auto input = static_cast<std::uint8_t*>(self);
      log("CONTROL movement accepted mode=%u value=%.3f flags=%x previous=%x direction=(%.4f %.4f "
          "%.4f) owner=%p selector=%p tick=%llu",
          input_mode, input_value, *reinterpret_cast<unsigned*>(input + 24),
          *reinterpret_cast<unsigned*>(input + 28), *reinterpret_cast<float*>(input + 88),
          *reinterpret_cast<float*>(input + 92), *reinterpret_cast<float*>(input + 96),
          *reinterpret_cast<void**>(input + 16),
          combat ? *reinterpret_cast<void**>(combat + 40) : nullptr, GetTickCount64());
      log_native_control_state("input after", own_movement);
      DWORD foreground_pid = 0;
      GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
      log("CONTROL physical input name=%.64s mode=%u foreground=%lu own_pid=%lu W=%u A=%u S=%u "
          "D=%u caller=%p tick=%llu",
          *action, input_mode, foreground_pid, GetCurrentProcessId(),
          unsigned((GetAsyncKeyState('W') & 0x8000) != 0),
          unsigned((GetAsyncKeyState('A') & 0x8000) != 0),
          unsigned((GetAsyncKeyState('S') & 0x8000) != 0),
          unsigned((GetAsyncKeyState('D') & 0x8000) != 0), _ReturnAddress(), GetTickCount64());
      if (input_mode == 2) {
        void* stack[10]{};
        const auto count = CaptureStackBackTrace(1, 10, stack, nullptr);
        const auto base = reinterpret_cast<std::uintptr_t>(image);
        for (unsigned i = 0; i < count; ++i) {
          const auto at = reinterpret_cast<std::uintptr_t>(stack[i]);
          if (at >= base && at - base < image_size)
            log("CONTROL input release caller frame=%u ida=%llx", i, 0x180000000ULL + at - base);
        }
      }
    }
    return result;
  };
  const std::string name = *action;
  if ((mode == 1 || mode == 2) && (name.compare(0, 4, "move") == 0 ||
                                   name.find("attack") != std::string::npos || name == "block")) {
    log("CONTROL input enter name=%.64s mode=%u value=%.3f chat=%u tick=%llu", name.c_str(), mode,
        value, unsigned(native_chat::editing()), GetTickCount64());
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    log_native_control_state("input before", *reinterpret_cast<void**>(actor + 632));
  }
  if (mode == 2) {
    chat_suppressed_actions.erase(name);
    return invoke_original(mode, value);
  }
  if (game_actions_blocked()) {
    const bool first = chat_suppressed_actions.insert(name).second;
    if (first && mode == 4)
      return invoke_original(2, 0.f);
    return nullptr;
  }
  chat_suppressed_actions
      .clear();  // Analog axes have no physical release event. Resume immediately after blur.
  return invoke_original(mode, value);
}
// WHGame 1.5.5 gEnv->pHardwareMouse (IHardwareMouse; IncrementCounter is vtable slot 11 at
// and shows the pointer, DecrementCounter slot 12 hides it again).
// CXConsole::ShowConsole uses the same pair for the console; its +0x40 is
// mov rcx,[rip+gEnv.pHardwareMouse], which locates the slot without another signature.
void** hardware_mouse_slot = nullptr;
// The pointer hold this client keeps for a focused view or Controls.showCursor.
std::atomic<bool> web_pointer_shown{false};
// A game menu (pause, inventory, map, a dialogue choice) shows the engine's pointer: its holds,
// the count IncrementCounter adds to at IHardwareMouse + 48, are then more than our own.
constexpr std::size_t kHardwareMouseHolds = 48;
bool game_menu_open() {
  void* mouse = hardware_mouse_slot ? *hardware_mouse_slot : nullptr;
  if (!mouse) {
    return false;
  }
  const auto holds = *reinterpret_cast<const volatile std::int32_t*>(
      static_cast<const std::uint8_t*>(mouse) + kHardwareMouseHolds);
  return holds - (web_pointer_shown.load() ? 1 : 0) > 0;
}
// A focused web view shows the pointer through the engine's own counter, so the game stops
// re-centring the mouse; the count is given back when the focus leaves. Not while the free camera
// turns with the mouse: it puts the pointer back in the middle every frame, where it stood still
// over the view (an admin flight mode can keep a focused page for its keys).
void sync_web_pointer() {
  void* mouse = hardware_mouse_slot ? *hardware_mouse_slot : nullptr;
  const bool shown = web_pointer_shown.load();
  const bool asked = kcds::runtime::web::has_input_focus() ||
                     kcds::runtime::native_ui::has_focus() ||
                     kcds::runtime::input::controls_cursor();
  const bool wanted = asked && !kcds::runtime::noclip::steers_pointer();
  if (!mouse || wanted == shown) {
    return;
  }
  using CounterFn = void(__fastcall*)(void*);
  slot<CounterFn>(mouse, wanted ? 11 : 12)(mouse);
  web_pointer_shown.store(wanted);
  log("WEB pointer %s (focused view or Controls.showCursor)", wanted ? "shown" : "hidden");
}
// While the chat input is open, every game action map is disabled through the engine's own
// IActionMapManager::Enable (CActionMapManager vtable slot 28, also bound to the
// script function EnableActionMapManager). Disabling with resetStateOnDisable releases held
// actions, so menus and other UI actions bound to typed keys cannot open while typing. When the
// chat closes, only a state this gate changed is restored. The enabled flag is CActionMapManager
// +252 (written by the same function).
void sync_chat_input_gate() {
  static bool disabled_by_chat = false;
  static bool verified = false;
  static bool refused = false;
  auto manager = static_cast<std::uint8_t*>(action_map_manager.load());
  if (!manager || refused) {
    return;
  }
  using EnableActionMapManager = void(__fastcall*)(void*, bool, bool);
  auto enable = slot<EnableActionMapManager>(manager, 28);
  if (!verified) {
    // mov [rsp+10h],rbx; push rbp/rsi/rdi; sub rsp,20h; mov [rcx+0FCh],dl
    static constexpr std::uint8_t kEnablePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55,
                                                       0x56, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                       0x88, 0x91, 0xFC, 0x00, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<const void*>(enable), kEnablePrologue,
                    sizeof(kEnablePrologue)) != 0) {
      log("CHAT input gate disabled: ActionMapManager slot 28 is not Enable");
      refused = true;
      return;
    }
    verified = true;
  }
  // The browser page over the world (Disconnected popup) takes the keyboard like the chat.
  const bool editing = game_actions_blocked() || page_over_world.load();
  const bool enabled = *reinterpret_cast<bool*>(manager + 252);
  if (editing && enabled) {
    enable(manager, false, true);
    disabled_by_chat = true;
    log("CHAT input gate: game action maps disabled while typing");
  } else if (!editing && disabled_by_chat) {
    if (!enabled) {
      enable(manager, true, false);
    }
    disabled_by_chat = false;
    log("CHAT input gate: game action maps restored");
  }
}

}  // namespace kcds::game_client
