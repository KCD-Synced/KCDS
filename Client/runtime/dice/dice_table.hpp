// A dice match between two players at the game's own dice table, WHGame 1.5.5.
//
// The local player plays the game's dice minigame (C_Dice) against the other player's replica,
// started as the game's table behavior starts it (Minigame.StartDice, ScriptBindMinigame):
// the local player's dice minigame (made when missing) and its start with a
// target score (the table's behavior entity, the opponent's entity, the target, the
// badge word, no farewell). The minigame's model (C_DiceGameModel, minigame + 1184) has the two
// players' models: the local player's (+ 32) and the opponent's (+ 48), whose AI (+ 72,
// C_SimpleDiceAI) plays the replica's turns. Its states (I_DiceGameModelState) are switched by its
// context (model + 24, SetState: the new state by const reference, copied; the state's
// player from its slot 4).
//
// The server deals every throw. A throw is the Rolling state's (C_RollingDiceGameState): its
// Enter throws the player's dice, which take the faces the game's own next-throw
// override gives them (player model + 409 set, + 416 the faces of the dice not set aside, in
// order, 0 for a one to 5 for a six; the binding ScriptBind_Dice copies them with the
// vector assign it calls at + 0xF0). So the switch into Rolling waits until the server's throw for
// that player has come, which then goes into the override; the model's throw fixing chances (+ 448,
// + 452) are zeroed. The local player's moves are seen as the switches they make: into Scoring
// (score and continue) and Passing (score and pass), the dice they kept the ones selected (a die's
// state at + 64: 0 on the table, 1 selected, 2 set aside; the dice at player model + 216, 16 bytes
// each). The other player's moves are played for the replica in place of its AI's turn
// (C_SimpleDiceAI slot 3, which the minigame asks when it is ready for a next step and,
// when no AI acts, steps the state on: a pass in a turn): its kept dice selected one by one (the
// state's slot 12), then score and continue (slots 18 and 19) or score and pass (slot 22); without
// the move yet, the AI is said to have acted and the turn waits. Who throws first is the server's:
// the game checks the first player's passive badges (C_CheckPassiveBadgeDiceGameState), then the
// other's, then gives the first player the turn, the first player being always the local one; the
// Initial state's step (slot 7) and the check's step (slot 7) run that chain
// from the opponent when the opponent throws first.
//
// The game seats its own player at the seat the table links as "seat1" and looks the opponent at
// it (the minigame reads the table's links at its start: seat1 at minigame + 860, seat2
// at + 864, the player's and the opponent's badge tag points at + 868 and + 872, their close-ups
// at + 876 and + 880). The match's second player sits at seat2_SO: their game swaps the seats and
// the badge tag points. The close-ups stay: the camera that watches the opponent's throw is set
// from them (swapped, it looked from the first seat at the local player).
//
// Engine thread only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/integration/dice.hpp"

namespace kcds::runtime::dice {

using Logger = void (*)(const char* format, ...);

struct Natives {
  std::uint8_t* image = nullptr;
  std::size_t image_size = 0;
  void* local_minigame = nullptr;
  void* start_with_score = nullptr;
  void* set_state = nullptr;
  void* throw_override = nullptr;
  void* table_links = nullptr;
  void* seat_players = nullptr;
  void* board_camera = nullptr;
  void* board_camera_aim = nullptr;
  void* send_message = nullptr;  // XGenAIModule.SendMessageToEntity's binding
  void* draw_face = nullptr;     // a die's face drawn from its side weights
  void* bust_choice = nullptr;   // a player's choice after a bust with a Resurrection badge
  Logger log = nullptr;
  // Puts the players' names on the game's dice scoreboard (the HUD's fc_showDiceScore writes the
  // local player's own name and "Opponent" there each time it shows the score).
  std::function<void(const std::string& own, const std::string& opponent)> show_names;
};

bool install(const Natives& natives);

struct Start {
  std::uint32_t match = 0;
  std::uint32_t table = 0;     // the table's behavior entity
  std::uint32_t opponent = 0;  // the other player's replica entity
  std::uint8_t seat = 0;       // the local player's: 0 the table's seat1_SO, 1 its seat2_SO
  bool first = false;
  std::uint32_t target = 0;
  // The names the scoreboard shows; empty leaves the game's.
  std::string own_name;
  std::string opponent_name;
  // The meshes of the local player's and the other player's six dice (the game's kinds); none
  // leaves the game's plain dice.
  std::vector<std::string> own_dice;
  std::vector<std::string> opponent_dice;
  // Their dice's side values (0 to 5 the faces 1 to 6, 6 the joker), as many as the meshes.
  std::vector<std::array<std::uint8_t, 6>> own_sides;
  std::vector<std::array<std::uint8_t, 6>> opponent_sides;
  // Both players' badge effects (dice_badge_subtype, 0 Headstart to 12 Balatro), -1 for none.
  int own_badge = -1;
  int opponent_badge = -1;
  // The local player's badge tier (0 tin, 1 silver, 2 gold), for autoplay.
  int own_badge_tier = 0;
};

// Starts the game's dice minigame for a match; false when the game would not.
bool start(const Start& start);

// The scoreboard's names changed (empty for the game's own).
void names(std::uint32_t match, const std::string& own, const std::string& opponent);

// The server's throw, the other player's move and the match's end.
void roll(std::uint32_t match, bool mine,
          const std::array<std::uint8_t, kcd2s::integration::kDiceCount>& faces,
          std::uint8_t rolled);
// The other player's move (Continue, Pass, BadgePass, Badge, BustChoice), played for the replica
// step by step; a BustChoice is played for the local player too when the server took the bust.
void opponent_move(std::uint32_t match, kcd2s::integration::DiceState::Move kind,
                   std::uint8_t held);
// The dice the other player has selected so far in their turn: the replica's dice show it.
void opponent_select(std::uint32_t match, std::uint8_t selected);
// A won match the game is let end itself, once it has played the last move (within ten seconds).
void end(std::uint32_t match, kcd2s::integration::DiceEnd reason,
         kcd2s::integration::DiceResult result);

// Every frame: a throw that waited for the server's faces goes on, and a minigame that is over
// is let go.
void tick();

// While the local player throws in a match: the match and the entity ids of the table's things
// the game moves for the throw (the local player's dice, C_Dice + 1200 + 16 x the player's index
// (+ 88 of the player's model), the board's dice at + 240, a die's entity id at + 48, as the
// game reads a die's place;
// and the cup, + 856), for the players watching (DICE_VIEW). Empty at any other time.
// npc_opponent: the opponent is the server's NPC, which has no game of its own to tell its turn, so
// this game tells both.
std::vector<std::uint32_t> own_table_things(std::uint32_t& match, std::uint32_t& cup,
                                            bool npc_opponent);

// Whether the local player sits in a dice match (any thread; as of the last tick).
bool at_table();

// Whether the local player's dice minigame is in one of the local player's states now (their turn
// has begun in the game).
bool own_turn_now();
// The local player's match's dice cup: its entity (null: no match) and its place and turn on the
// table.
void* cup_rest(float place[3], float turn[4]);

// The entity id of the other player's die at `index` of the throw in the local player's match
// (their board's dice in the order of the throw), 0 for none.
std::uint32_t opponent_die(std::size_t index);
// The skeleton of the server's NPC seated against the local player (null: none), and whether a
// skeleton is it: its clips blend for the other games as the local player's do.
void set_seated_npc(void* skeleton);
bool seated_npc(void* skeleton);

// The local player's dice minigame (C_Dice) of the current match, or null.
void* match_minigame();

// Test hook (the kit): the local player's turns play themselves, a die every step: the dice that
// score (1s, 5s, three or more of a kind, or a straight of all six) are kept, then the turn is
// thrown on with three dice or more left (or all six scored) and banked otherwise.
void set_autoplay(bool on);
bool autoplay_on();

// The local player's moves since the last call.
std::vector<kcd2s::integration::DiceMove> take_moves();

}  // namespace kcds::runtime::dice
