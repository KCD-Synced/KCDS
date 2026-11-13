// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "dice_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../appearance/outfit.hpp"
#include "../native_ui/native_ui.hpp"
#include "../props/prop_entity.hpp"
#include "../vendors/vendors_tick.hpp"
#include "dice_badges.hpp"
#include "dice_sit.hpp"
#include "dice_table.hpp"

namespace kcds::game_client {
namespace {

namespace sdk = kcd2s::sdk;

// One label of the HUD's dice scoreboard (hud.gfx: _root.bl.diceScoreContainer.score.inside).
void set_score_label(const char* label, const std::string& text) {
  sdk::Value::Array path{sdk::Value("bl"), sdk::Value("diceScoreContainer"), sdk::Value("score"),
                         sdk::Value("inside"), sdk::Value(label)};
  kcds::runtime::native_ui::request(
      sdk::Value(sdk::Value::Map{{"op", sdk::Value("elementClip")},
                                 {"name", sdk::Value("hud")},
                                 {"instance", sdk::Value(std::int64_t(0))},
                                 {"path", sdk::Value(std::move(path))},
                                 {"action", sdk::Value("set")},
                                 {"member", sdk::Value("text")},
                                 {"value", sdk::Value(text)}}));
}

}  // namespace

void install_dice() {
  kcds::runtime::dice::Natives natives;
  natives.image = image;
  natives.image_size = image_size;
  // The local player's dice minigame, made when missing.
  natives.local_minigame = resolve_unique(
      "DiceLocalMinigame", "40 53 48 83 EC 30 E8 ?? ?? ?? ?? 48 8B 48 08 48 8B 01 FF 90 00");
  // Its start at a table against an opponent with a target score.
  natives.start_with_score = resolve_unique(
      "DiceStartWithScore", "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 30 C7 40 20");
  // The dice model's context switching its state.
  natives.set_state = resolve_unique(
      "DiceSetState",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 30 80 "
      "B9 0C");
  // ScriptBind_Dice's next-throw override, for the vector assign it calls.
  natives.throw_override = resolve_unique(
      "DiceThrowOverride",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 8B 05 ?? ?? "
      "?? ?? 48 33 C4 48 89 45 40 4D");
  // C_Dice reading its table's links (seats, cup, badge tag points) at its start.
  natives.table_links = resolve_unique(
      "DiceTableLinks",
      "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 48 8B 41 18");
  // C_Dice putting the local player and the opponent on their seats (the seat links at + 860 and
  // + 864), at the end of reading the table's links and again as the match begins.
  natives.seat_players = resolve_unique(
      "DiceSeatPlayers",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 41 18 48 8B D9 48 8B 0D ?? ?? ?? ?? "
      "48 8B 70 38 48 8B 01 8B 93 4C 03 00 00 FF 50 70 48 8B F8 48 85 C0 74 1E 44 8B 83 5C 03 "
      "00 00");
  // The board camera's place and turn from the board of the player on turn, the eye (the local
  // player's place) and the die it turns to.
  natives.board_camera_aim = resolve_unique(
      "DiceBoardCameraAim",
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 "
      "EC C0 00 00 00 0F 29 70 C8 4C 8D 91 18 01 00 00");
  // C_Dice setting its camera: mode 1 above the thrower's board.
  natives.board_camera = resolve_unique(
      "DiceBoardCamera", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 89");
  // The script binding XGenAIModule.SendMessageToEntity: a dice table's "Play dice"
  // sends its behaviour "dice:init" through it (DiceInteractor:OnUsed).
  natives.send_message = resolve_unique(
      "ScriptSendMessageToEntity", "48 8B C4 48 89 58 10 48 89 48 08 57 48 83 EC 30 48 8B FA 4C");
  // A die's face drawn from its side weights: a badge's dice thrown again.
  natives.draw_face = resolve_unique(
      "DiceDrawFace",
      "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 0F 29 70 E8 48 8B F2 0F 29 78 D8 48");
  // A player's choice after a bust with a Resurrection badge.
  natives.bust_choice = resolve_unique(
      "DiceBustChoice", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D B9");
  natives.log = log;
  natives.show_names = [](const std::string& own, const std::string& opponent) {
    if (!own.empty()) {
      set_score_label("playerLabel", own);
    }
    if (!opponent.empty()) {
      set_score_label("NPCLabel", opponent);
    }
  };
  log("DICE %s", kcds::runtime::dice::install(natives) ? "installed" : "unavailable");

  // The game's own sit-down at the table (dice_sit.hpp): its sit interrupt added as the table's
  // AddInterrupt adds it.
  kcds::runtime::dice::sit::Natives sit;
  sit.make_string = resolve_unique("GameStringFromText",
                                   "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 "
                                   "44 8B 05 ?? ?? ?? ?? 48 8B F1");
  sit.free_string = resolve_unique("GameStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8");
  sit.behavior_id = resolve_unique("AiBehaviorIdByName",
                                   "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 48 8D 0D ?? ?? ?? "
                                   "?? 49 8B F8 48 8B DA E8 ?? ?? ?? ?? 84 C0 0F");
  sit.host_of =
      resolve_unique("SmartObjectByWuid", "48 89 54 24 10 48 83 EC 28 48 8B C2 48 C1 E8 38");
  sit.build_request = resolve_unique("AiInterruptRequest", "8A 44 24 28 88 41 11 8A 44 24 40 88");
  sit.move_request =
      resolve_unique("AiInterruptRequestMove", "8B 02 45 33 C0 89 01 48 8B 42 08 48");
  sit.enqueue = resolve_unique("AiInterruptEnqueue",
                               "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 F6 81 "
                               "B0");
  sit.destroy_request = resolve_unique("AiInterruptRequestDestroy",
                                       "48 8B 49 28 48 85 C9 75 ?? C3 E9 ?? ?? ?? ?? CC 40");
  // `mov r8, [rip + disp32]` of the NPC manager in a getter of it.
  if (auto getter = static_cast<std::uint8_t*>(
          resolve_unique("NpcManagerGetter", "4C 8B 05 ?? ?? ?? ?? 48 8D 51 38 49 8B C8"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, getter + 3, 4);
    sit.npc_manager = reinterpret_cast<void* const*>(getter + 7 + displacement);
  }
  sit.wuid_of = &entity_wuid;
  sit.log = log;
  log("DICE sit-down %s", kcds::runtime::dice::sit::install(sit) ? "installed" : "unavailable");

  kcds::runtime::dice::badges::Natives badges;
  // C_Dice binding both players' badges once it has made their models.
  badges.bind = resolve_unique(
      "DiceBindBadges",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 49 8B D8 48 8B CA 48 8B FA E8 ?? ?? "
      "?? ?? 84 C0 74 ?? 48 8D");
  // A badge put on the table at its tag point.
  badges.place = resolve_unique(
      "DicePlaceBadge",
      "48 8B C4 48 89 58 18 48 89 78 20 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 0F 29 78 "
      "D8 48");
  badges.create_item = [](std::uint32_t entity, const kcd2s::integration::Guid& item) -> void* {
    void* found = kcds::runtime::props::entity::by_id(entity);
    if (!found) {
      return nullptr;
    }
    return kcds::runtime::appearance::outfit::create_item_in_inventory(found, item);
  };
  badges.match_minigame = [] { return kcds::runtime::dice::match_minigame(); };
  badges.log = log;
  log("DICE badges %s", kcds::runtime::dice::badges::install(badges) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
