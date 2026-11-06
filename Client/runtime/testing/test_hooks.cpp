// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "test_hooks.hpp"

#include "bone_camera.hpp"
#include "stack_sampler.hpp"
#include "fake_movement_probe.hpp"
#include "nav_listener_probe.hpp"
#include "prefab_probe.hpp"
#include "navmesh_probe.hpp"
#include "terrain_probe.hpp"

#include "../book/resource_book.hpp"
#include "../combat/attack_observers.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../gathering/herbs.hpp"
#include "../ground/ground_items.hpp"
#include "../input/scripted_input.hpp"
#include "../progression/progression_tick.hpp"
#include "../riding/trample.hpp"
#include "MinHook.h"
#include "combat_tests.hpp"
#include "kcd2s/integration/ground_items.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace kcds::game_client {

// Test hook: holds the local fighter's block natively (tick_native_block_test).
HANDLE native_block_test_event = nullptr;
// Test hooks (named events Local\KCD2S.BookProbe{Open,Turn,Close}.<pid>): open a test book, turn
// its page, close it, so another client's view of the held book can be checked without input.
HANDLE book_probe_open_event = nullptr;
HANDLE book_probe_turn_event = nullptr;
HANDLE book_probe_close_event = nullptr;
// Test hook: runs %TEMP%\kcds-probe-<pid>.lua in the game's script system (to face players).
HANDLE probe_lua_event = nullptr;
// Test hook (Local\KCD2S.GiveItem.<pid>): adds the item whose class id is in
// %TEMP%\kcds-give-<pid>.txt to the local player (book::give_test_item).
HANDLE give_item_event = nullptr;
// Test hook (Local\KCD2S.GroundSpawn.<pid>): lays the stacks in %TEMP%\kcds-ground-<pid>.txt
// ("<class GUID> <amount> <x> <y> <z>" per line) in the world (ground::lay).
HANDLE ground_spawn_event = nullptr;
// Test hook (Local\KCD2S.ChatLine.<pid>): the line in %TEMP%\kcds-chat-<pid>.txt goes out as if
// typed in the chat, so a test reaches the resources' chat handlers without keyboard input (which
// would go to whatever window is in front).
HANDLE chat_line_event = nullptr;
// Test hook (Local\KCD2S.LearnPerk.<pid>): the perk whose GUID is in %TEMP%\kcds-learn-<pid>.txt
// is confirmed as the perk screen confirms it (its learn helper: a cause
// "HUD/INVENTORY" and a C_LearnPerkEffect on the RPG module's queue), so the perk gate is tested
// without the screen.
HANDLE learn_perk_event = nullptr;
// Test hook (Local\KCD2S.PageMessage.<pid>): each line of %TEMP%\kcds-page-<pid>.txt goes to the
// server browser session as if the page sent it ({"type":"direct","address":...},
// {"type":"reconnect"}, ...), so joins and leaves can be tested without the mouse.
HANDLE page_message_event = nullptr;
// Test-only native observation on the engine thread, one bounded log snapshot per signal.
HANDLE body_snapshot_event = nullptr;
// Test hook (Local\KCD2S.DiceAutoplay.<pid>): each signal turns the local player's dice autoplay
// on or off (dice::set_autoplay), so the kit plays whole matches without input.
HANDLE dice_autoplay_event = nullptr;
// Test hook (Local\KCD2S.HerbDump.<pid>): writes the plants the game's respawn manager holds to
// %TEMP%\kcds-herbs-<pid>.csv, for a sweep of a level's herbs (Dev/tools/gathering/build_herbs.py).
HANDLE herb_dump_event = nullptr;
bool signaled(HANDLE event);

std::string take_test_chat_line() {
  if (!signaled(chat_line_event)) {
    return {};
  }
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-chat-%lu.txt", folder, GetCurrentProcessId());
  std::ifstream input(file, std::ios::binary);
  std::string line((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  log("PROBE chat line %zu bytes", line.size());
  return line;
}

std::string take_probe_lua() {
  if (!signaled(probe_lua_event)) {
    return {};
  }
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-probe-%lu.lua", folder, GetCurrentProcessId());
  std::ifstream input(file, std::ios::binary);
  std::string chunk((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  log("PROBE lua %zu bytes", chunk.size());
  return chunk;
}

bool signaled(HANDLE event) {
  return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

// Test hook: logs what the UI's picture loader (CryGFxImageLoader) is asked for
// and whether it made an image.
LoadUiImage original_load_ui_image = nullptr;

void* __fastcall hooked_load_ui_image(void* loader, const char* url) {
  void* picture = original_load_ui_image(loader, url);
  log("PROBE ui image %s -> %p", url ? url : "(null)", picture);
  return picture;
}

// Test hook: the game's System.LogAlways (CScriptBind_System::LogAlways) puts a
// probe's "KCDSPROBE ..." lines into this log, since the game's own log stops after start and its
// Lua has no file access. The text is the call's first argument, read with the function
// handler's GetParam for strings.
using LogAlwaysBinding = std::int64_t(__fastcall*)(void* binding, void* handler);
using HandlerStringParam = bool(__fastcall*)(void* handler, int index, const char** text);
LogAlwaysBinding original_log_always = nullptr;
HandlerStringParam handler_string_param = nullptr;

std::int64_t __fastcall hooked_log_always(void* binding, void* handler) {
  const char* text = nullptr;
  if (handler && handler_string_param && handler_string_param(handler, 1, &text) && text &&
      std::strncmp(text, "KCDSPROBE", 9) == 0) {
    log("PROBE lua says %s", text + 9);
  }
  return original_log_always(binding, handler);
}

// The probe book's first page: %TEMP%\kcds-book-probe-<pid>.html when there is one.
std::string probe_first_page() {
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-book-probe-%lu.html", folder, GetCurrentProcessId());
  std::ifstream input(file, std::ios::binary);
  std::string page((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return page.empty() ? std::string("<title>Probe</title>") : page;
}

// The probe book's picture: page 0 shows the native picture named in
// %TEMP%\kcds-book-probe-<pid>.image when there is one.
kcd2s::sdk::Value::Array probe_images() {
  namespace sdk = kcd2s::sdk;
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-book-probe-%lu.image", folder, GetCurrentProcessId());
  std::ifstream input(file, std::ios::binary);
  std::string name;
  input >> name;
  if (name.empty()) {
    return {};
  }
  return {sdk::Value(sdk::Value::Map{{"page", sdk::Value(0)}, {"image", sdk::Value(name)}})};
}

// Engine thread.
void tick_book_probe() {
  namespace sdk = kcd2s::sdk;
  if (signaled(give_item_event)) {
    wchar_t folder[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, folder);
    wchar_t file[MAX_PATH + 64];
    swprintf_s(file, L"%skcds-give-%lu.txt", folder, GetCurrentProcessId());
    // One class id per line, optionally followed by the item's health; several lines give a
    // whole kit at once.
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      std::string class_id;
      float health = -1.0f;
      if (!(fields >> class_id)) {
        continue;
      }
      fields >> health;
      log("TEST give item %s health=%.3f added=%u", class_id.c_str(), health,
          unsigned(kcds::runtime::book::give_test_item(class_id, health)));
    }
  }
  if (signaled(herb_dump_event)) {
    wchar_t folder[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, folder);
    wchar_t file[MAX_PATH + 64];
    swprintf_s(file, L"%skcds-herbs-%lu.csv", folder, GetCurrentProcessId());
    log("TEST herb dump: %ld plants", kcds::runtime::gathering::dump_plants(file));
  }
  if (signaled(ground_spawn_event)) {
    wchar_t folder[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, folder);
    wchar_t file[MAX_PATH + 64];
    swprintf_s(file, L"%skcds-ground-%lu.txt", folder, GetCurrentProcessId());
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      std::string class_id;
      kcds::runtime::ground::Stack stack;
      kcd2s::integration::Guid guid{};
      if (!(fields >> class_id >> stack.amount >> stack.position[0] >> stack.position[1] >>
            stack.position[2]) ||
          !kcd2s::integration::parse_guid(class_id, guid)) {
        continue;
      }
      std::memcpy(stack.item.data(), guid.data(), guid.size());
      void* item = kcds::runtime::ground::lay(stack);
      log("TEST ground stack %s x%u at %.2f %.2f %.2f: %p", class_id.c_str(), stack.amount,
          stack.position[0], stack.position[1], stack.position[2], item);
    }
  }
  if (signaled(book_probe_open_event)) {
    static std::int64_t next_id = 9000;
    sdk::Value::Map message{
        {"op", sdk::Value("bookOpen")},
        {"id", sdk::Value(++next_id)},
        {"style", sdk::Value(0)},
        {"type", sdk::Value(1)},
        {"visual", sdk::Value(3)},
        {"legibility", sdk::Value(1.0)},
        // Four pages: two spreads, so the probe can turn a page.
        {"pages", sdk::Value(sdk::Value::Array{sdk::Value(probe_first_page()),
                                               sdk::Value("<paragraph>Second page</paragraph>"),
                                               sdk::Value("<paragraph>Third page</paragraph>"),
                                               sdk::Value("<paragraph>Fourth page</paragraph>")})},
        {"images", sdk::Value(probe_images())},
    };
    kcds::runtime::book::handle_runtime_message(sdk::Value(message));
    log("BOOK probe open");
  }
  if (signaled(book_probe_turn_event)) {
    kcds::runtime::book::page_turned(true);
    log("BOOK probe page turn");
  }
  if (signaled(book_probe_close_event)) {
    kcds::runtime::book::handle_runtime_message(
        sdk::Value(sdk::Value::Map{{"op", sdk::Value("bookClose")}}));
    log("BOOK probe close");
  }
}

DWORD install_test_hooks() {
  wchar_t test_name[96];
  swprintf_s(test_name, L"Local\\KCD2S.NativeBlockTest.%lu", GetCurrentProcessId());
  native_block_test_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
  swprintf_s(test_name, L"Local\\KCD2S.NativeAttackTest.%lu", GetCurrentProcessId());
  native_attack_test_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
  swprintf_s(test_name, L"Local\\KCD2S.MasterStrikeTest.%lu", GetCurrentProcessId());
  master_strike_test_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
  if (kcds::runtime::input::scripted::install(log))
    log("INPUT script hook installed");
  // Test hooks only for a kit that asks for them.
  if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) > 0) {
    // the navigation listener set (log only, nothing changes).
    kcds::runtime::testing::nav_listeners::install();
    // Issue 016: who hands an NPC to the game's fake movement (log only, nothing changes).
    kcds::runtime::testing::fake_movement::install();
    auto actor_animation_start = resolve_unique(
        "TestNativeActorAnimationStart",
        "48 89 5C 24 08 57 48 83 EC 40 B2 01 48 8B D9 E8 ?? ?? ?? ?? 48 8B 93 80 00 00 00 "
        "48 8D 4C 24 20 48 81 C2 60 01 00 00 E8 ?? ?? ?? ??");
    if (!actor_animation_start ||
        MH_CreateHook(actor_animation_start,
                      reinterpret_cast<void*>(observed_actor_animation_start),
                      reinterpret_cast<void**>(&original_actor_animation_start)) != MH_OK ||
        MH_EnableHook(actor_animation_start) != MH_OK) {
      log("ACTOR ANIMATION start test probe unavailable");
      return 1;
    }
    auto movement_combat_entry = resolve_unique(
        "TestNativeMovementCombatEntry",
        "48 89 5C 24 08 57 48 83 EC 20 80 3A 00 48 8B D9 0F 85 ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? "
        "48 8B 89 E8 03 00 00 E8 ?? ?? ?? ?? 84 C0");
    if (!movement_combat_entry ||
        MH_CreateHook(movement_combat_entry,
                      reinterpret_cast<void*>(observed_movement_combat_entry),
                      reinterpret_cast<void**>(&original_movement_combat_entry)) != MH_OK ||
        MH_EnableHook(movement_combat_entry) != MH_OK) {
      log("COMBAT movement entry test probe unavailable");
      return 1;
    }
    // The attack request check itself is hooked in every build (hooked_attack_request_check); the
    // kit also observes its nested player-controller check and logs every request.
    auto attack_controller_check = resolve_unique(
        "TestNativeAttackControllerCheck",
        "48 89 5C 24 08 57 48 83 EC 60 0F 29 74 24 50 41 8A D8 48 8B F9 48 85 D2 74 1D E8 "
        "?? ?? ?? ?? 84 C0 74 14 48 8B 8F 50 04 00 00 48 8B 01 FF 50 20");
    if (!attack_controller_check ||
        MH_CreateHook(attack_controller_check,
                      reinterpret_cast<void*>(observed_attack_controller_check),
                      reinterpret_cast<void**>(&original_attack_controller_check)) != MH_OK ||
        MH_EnableHook(attack_controller_check) != MH_OK) {
      log("COMBAT attack request test probes unavailable");
      return 1;
    }
    attack_request_probe = true;
    auto riposte_opponent = resolve_unique(
        "TestNativeRiposteOpponent",
        "40 53 48 83 EC 20 48 8B 81 88 00 00 00 33 DB 48 8B 88 10 02 00 00 48 8B 89 F8 0C 00 "
        "00 48 85 C9 74 0C 8D 53 10");
    auto riposte_eligibility = resolve_unique(
        "TestNativeRiposteEligibility",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 89 88 00 00 00 48 8B 01 FF 90 A8 05 00 00 "
        "32 DB 84 C0 74 3F 48 8D 4F 68 48 8B 01 FF 50 08 83 78 08");
    auto movement_can_attack = resolve_unique(
        "TestNativeMovementCanAttack",
        "48 89 5C 24 10 57 48 83 EC 20 48 8B F9 48 8B 89 E8 03 00 00 E8 ?? ?? ?? ?? 84 C0 0F 84 "
        "FA 00 00 00 BA 40 00 00 00 48 8B CF E8 ?? ?? ?? ?? 84 C0");
    if (!riposte_opponent || !riposte_eligibility || !movement_can_attack ||
        MH_CreateHook(riposte_opponent, reinterpret_cast<void*>(observed_riposte_opponent),
                      reinterpret_cast<void**>(&original_riposte_opponent)) != MH_OK ||
        MH_CreateHook(riposte_eligibility, reinterpret_cast<void*>(observed_riposte_eligibility),
                      reinterpret_cast<void**>(&original_riposte_eligibility)) != MH_OK ||
        MH_CreateHook(movement_can_attack, reinterpret_cast<void*>(observed_movement_can_attack),
                      reinterpret_cast<void**>(&original_movement_can_attack)) != MH_OK ||
        MH_EnableHook(movement_can_attack) != MH_OK || MH_EnableHook(riposte_opponent) != MH_OK ||
        MH_EnableHook(riposte_eligibility) != MH_OK) {
      log("COMBAT riposte eligibility test probes unavailable");
      return 1;
    }
    swprintf_s(test_name, L"Local\\KCD2S.BodySnapshot.%lu", GetCurrentProcessId());
    body_snapshot_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.BookProbeOpen.%lu", GetCurrentProcessId());
    book_probe_open_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.BookProbeTurn.%lu", GetCurrentProcessId());
    book_probe_turn_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.BookProbeClose.%lu", GetCurrentProcessId());
    book_probe_close_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    if (void* loader = resolve_unique("UiImageLoad",
                                      "48 89 5C 24 08 55 56 57 41 54 41 56 48 8B EC 48 83 EC 60")) {
      if (MH_CreateHook(loader, reinterpret_cast<void*>(hooked_load_ui_image),
                        reinterpret_cast<void**>(&original_load_ui_image)) == MH_OK) {
        MH_EnableHook(loader);
      }
    }
    swprintf_s(test_name, L"Local\\KCD2S.ProbeLua.%lu", GetCurrentProcessId());
    probe_lua_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    handler_string_param = reinterpret_cast<HandlerStringParam>(
        resolve_unique("ScriptHandlerStringParam",
                       "48 89 5C 24 08 57 48 83 EC 40 48 8B 01 49 8B F8 4C 8D 44 24 20 C7 44 24 "
                       "20 05 00 00 00 FF 50 38 32 DB 84 C0 74 ?? 83 7C 24 20 05 75 ?? 48 8B 44"));
    if (void* binding = resolve_unique(
            "ScriptLogAlways",
            "40 53 48 83 EC 30 48 8B 02 48 8B CA 48 8B DA FF 50 28 BA 01 00 00 00 48 8B CB 3B C2 "
            "74 ?? 48 8B 03 FF 50 28 BA 02 00 00 00 4C 8D 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 89 "
            "44 24 20 8D 4A 05 E8 ?? ?? ?? ?? EB ?? 4C 8D 44 24 48 48 C7 44 24 48 00 00 00 00 E8 "
            "?? ?? ?? ?? 4C 8B 4C")) {
      if (handler_string_param &&
          MH_CreateHook(binding, reinterpret_cast<void*>(hooked_log_always),
                        reinterpret_cast<void**>(&original_log_always)) == MH_OK) {
        MH_EnableHook(binding);
      }
    }
    swprintf_s(test_name, L"Local\\KCD2S.GiveItem.%lu", GetCurrentProcessId());
    give_item_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.GroundSpawn.%lu", GetCurrentProcessId());
    ground_spawn_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.ChatLine.%lu", GetCurrentProcessId());
    chat_line_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.LearnPerk.%lu", GetCurrentProcessId());
    learn_perk_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    screen_learn_perk = reinterpret_cast<ScreenLearnPerk>(
        resolve_unique("ScreenLearnPerk",
                       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B 05 "
                       "?? ?? ?? ?? 41"));
    swprintf_s(test_name, L"Local\\KCD2S.PageMessage.%lu", GetCurrentProcessId());
    page_message_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.DiceAutoplay.%lu", GetCurrentProcessId());
    dice_autoplay_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    swprintf_s(test_name, L"Local\\KCD2S.HerbDump.%lu", GetCurrentProcessId());
    herb_dump_event = CreateEventW(nullptr, FALSE, FALSE, test_name);
    kcds::runtime::testing::prefab_probe::install();
    kcds::runtime::testing::terrain_probe::install();
    kcds::runtime::testing::navmesh_probe::install();
    install_bone_camera();
    install_stack_sampler();
    // The kit's game may lose the focus to whatever else runs on the PC: without it the engine
    // sleeps up to 25 ms a frame, which measures nothing about a
    // crowd.
    using BackgroundSleep = void(__fastcall*)(std::uint8_t* system);
    const BackgroundSleep no_background_sleep = [](std::uint8_t* system) { system[10799] = 0; };
    if (auto background_sleep = resolve_unique("BackgroundSleep",
                                               "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 B9 "
                                               "C1 05 00 00 00 48 8B D9 C6 81 2F 2A 00 00 00")) {
      MH_CreateHook(background_sleep, reinterpret_cast<void*>(no_background_sleep), nullptr);
      MH_EnableHook(background_sleep);
    }
  }
  return 0;
}

void create_trample_replay_event() {
  if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) > 0) {
    wchar_t replay_name[96];
    swprintf_s(replay_name, L"Local\\KCD2S.TrampleReplay.%lu", GetCurrentProcessId());
    trample_replay_event = CreateEventW(nullptr, FALSE, FALSE, replay_name);
  }
}

}  // namespace kcds::game_client
