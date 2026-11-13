#include "dice_table.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::dice {
namespace {

namespace wire = kcd2s::integration;

// The minigame's model and its parts.
constexpr std::size_t kMinigameModel = 1184;
constexpr std::size_t kModelContext = 24;
constexpr std::size_t kModelState = 264;
constexpr std::size_t kModelTarget = 256;
constexpr std::size_t kContextLocal = 8;
constexpr std::size_t kContextOpponent = 24;
constexpr std::size_t kPlayerScore = 48;
constexpr std::size_t kPlayerDice = 216;
constexpr std::size_t kPlayerOverride = 409;
constexpr std::size_t kPlayerOverrideFaces = 416;
constexpr std::size_t kPlayerThrowFixing = 448;
constexpr std::size_t kPlayerBustFixing = 452;
constexpr std::size_t kDieId = 40;
constexpr std::size_t kDieState = 64;
constexpr std::uint8_t kDieOnTable = 0;
constexpr std::uint8_t kDieSelected = 1;
constexpr std::uint8_t kDieSetAside = 2;
// The dice a player may have: six, seven with a Might badge's die.
constexpr std::size_t kDiceCount = 7;
constexpr std::size_t kSixDice = 6;
// A die's face (0 a one to 5 a six, 6 a joker die's joker).
constexpr std::size_t kDieFace = 88;
// The minigame's slot 8: whether it runs.
constexpr std::size_t kMinigameRuns = 8;
// A state's slots: its player, the next step (Initial), a die selected, the turn given up,
// whether it may score and continue, score and continue, score and pass.
constexpr std::size_t kStatePlayer = 4;
constexpr std::size_t kStateNext = 7;
constexpr std::size_t kStateGiveUp = 11;
constexpr std::size_t kStateSelectDie = 12;
constexpr std::size_t kStateMayContinue = 18;
constexpr std::size_t kStateContinue = 19;
constexpr std::size_t kStatePass = 22;
// A turn's badge actions (PlayerTurn): the Might badge's die, the selected dice thrown again, the
// badge's die selection begun; in that selection (BadgeSelecting), the change confirmed.
constexpr std::size_t kStateExtraDie = 15;
constexpr std::size_t kStateReroll = 16;
constexpr std::size_t kStateBadgeSelect = 26;
constexpr std::size_t kStateBadgeConfirm = 27;
// A state's Enter (the Resurrection badge's check after a bust asks the AI there).
constexpr std::size_t kStateEnter = 5;
// The factor PlayerTurn keeps for a pass with a Doppelganger or Warlord badge.
constexpr std::size_t kTurnBadgeFactor = 32;
// A player's badge (C_DiceBadge, shared pointer) and a badge's uses left.
constexpr std::size_t kPlayerBadge = 392;
constexpr std::size_t kBadgeUses = 48;
// The badge effects the client plays (dice_badge_subtype).
constexpr int kFormations = 1;
constexpr int kAntiBust = 4;
constexpr int kDoubleTake = 5;
constexpr int kMultiplier = 6;
constexpr int kExtraDice = 7;
constexpr int kRerollDice = 8;
constexpr int kSetDice = 9;
constexpr int kRerollPips = 10;
constexpr int kBalatro = 12;
// The AI's turn (C_SimpleDiceAI slot 3).
constexpr std::size_t kAiAct = 3;
// The Initial state's step calls the passive badge check for the first player here; the check's
// step calls the first player's turn here.
constexpr std::size_t kInitialCheckCall = 0x18;
constexpr std::size_t kCheckTurnCall = 0x60;
// ScriptBind_Dice's next-throw override calls the vector assign here.
constexpr std::size_t kOverrideAssignCall = 0xF0;
// The board camera's die to turn toward (reads it; -1 for none), and the camera's
// mode (1 the board camera).
constexpr std::size_t kBoardCameraDie = 1264;
constexpr std::size_t kCameraMode = 1124;
constexpr int kBoardCamera = 1;
// The board camera's turn (x, y, z, w) and how far down the game's own board camera looks
// (radians; its turn was about x by -1.39 from either seat, live).
constexpr std::size_t kBoardCameraTurn = 1140;
constexpr float kBoardLookPitch = -1.39f;
constexpr float kPi = 3.14159265f;
// How often a match's dice of the game's kinds are checked for their meshes (milliseconds).
constexpr std::uint64_t kDiceKindsMilliseconds = 250;
// The badge word of a match without badges, as the script binding leaves it.
constexpr std::uint16_t kNoBadge = 0x030D;
// How often the other player's move steps on when the game does not ask (milliseconds).
constexpr std::uint64_t kStepMilliseconds = 700;
// How long the replica's scoring or banking waits for the game to step it on (milliseconds).
constexpr std::uint64_t kStepOnMilliseconds = 1500;
// How long the game is given to reach the server's win itself, and how long a minigame the game
// closed waits for the server's end (milliseconds).
constexpr std::uint64_t kEndGraceMilliseconds = 10000;
constexpr std::uint64_t kClosedGraceMilliseconds = 3000;
// The table's links the minigame keeps, in player and opponent pairs: the seats and the badge tag
// points.
constexpr std::size_t kSeatPairs[][2] = {{860, 864}, {868, 872}};

struct SharedState {
  void* state = nullptr;
  volatile long* control = nullptr;
};

struct IntVector {
  int* first = nullptr;
  int* last = nullptr;
  int* end = nullptr;
};

using LocalMinigame = void*(__fastcall*)();
using StartWithScore = bool(__fastcall*)(void* minigame, std::uint32_t table,
                                         std::uint32_t opponent, int target,
                                         const std::uint16_t* badge, bool no_farewell);
using SetState = std::int64_t(__fastcall*)(void* context, const SharedState* next);
using StateFor = std::int64_t(__fastcall*)(void* context, void* player);
using StateStep = bool(__fastcall*)(void* state, void* context);
using AiAct = bool(__fastcall*)(void* ai, void* model, void* player, void* state,
                                std::uint8_t* handled);
using AssignInts = void*(__fastcall*)(void* vector, const IntVector* from);
using StatePlayer = void*(__fastcall*)(void* state);
using SelectDie = bool(__fastcall*)(void* state, void* context, std::uint32_t die, bool on);
using MayContinue = bool(__fastcall*)(void* state, void* context, void* player);
using Continue = bool(__fastcall*)(void* state, void* context);
using Pass = bool(__fastcall*)(void* state, void* context, bool badge);
using GiveUp = bool(__fastcall*)(void* state, void* context, void* player);
using MinigameRuns = bool(__fastcall*)(void* minigame);
using StateAction = bool(__fastcall*)(void* state, void* context);
using StateEnter = char(__fastcall*)(void* state, void* context);
using DrawFace = std::uint32_t(__fastcall*)(void* die_sides, void* random);
using BustChoice = bool(__fastcall*)(void* model, char choice);

struct Roll {
  std::array<std::uint8_t, kDiceCount> faces{};
  std::uint8_t rolled = 0;
};

struct Move {
  wire::DiceState::Move kind = wire::DiceState::Move::Continue;
  std::uint8_t held = 0;
};

// What a held switch throws: the turn's dice (the next-throw override), or a badge's dice (each
// die's face drawn: selected dice thrown again, the Might badge's die).
enum class Thrown { Dice, BadgeDice };

struct Match {
  std::uint32_t id = 0;
  void* minigame = nullptr;
  // The other player's replica entity.
  std::uint32_t opponent = 0;
  // The meshes of the local player's and the other player's dice (none: the game's plain dice),
  // and when they were last put on the board's dice.
  std::vector<std::string> own_dice;
  std::vector<std::string> opponent_dice;
  std::vector<std::array<std::uint8_t, 6>> own_sides;
  std::vector<std::array<std::uint8_t, 6>> opponent_sides;
  std::uint64_t dice_shown_at = 0;
  std::uint8_t seat = 0;
  bool first = false;
  std::string own_name;
  std::string opponent_name;
  std::deque<Roll> mine;
  std::deque<Roll> theirs;
  std::deque<Move> moves;
  // The switch into Rolling that waits for the server's throw, whose it is and what it throws.
  SharedState held;
  bool held_local = false;
  Thrown held_thrown = Thrown::Dice;
  // The side values the game draws next for a badge's dice (the server's faces less one).
  std::deque<std::uint32_t> drawn_faces;
  // Both players' badge effects (dice_badge_subtype), -1 for none.
  int own_badge = -1;
  int opponent_badge = -1;
  // The local player's badge tier (0 tin, 1 silver, 2 gold).
  int own_badge_tier = 0;
  // The other player's Transmutation or Balatro badge: its die selection was begun.
  bool badge_begun = false;
  // Autoplay (the kit) used the local player's badge in this throw, and how far.
  int autoplay_badge_step = 0;
  bool autoplay_bust_rethrown = false;
  // The replica's turn waits for the other player's move, and when it last stepped.
  bool waiting_move = false;
  std::uint64_t last_step_at = 0;
  // The local player's last throw, while autoplay has not played it.
  std::optional<Roll> own_roll;
  // The local player's selection the server was last told of in this throw.
  std::uint8_t told_selection = 0;
  // The other player's selection so far in their throw, as the server said it.
  std::uint8_t their_selection = 0;
  // The other player's turn state the board camera was last put on, and when.
  void* watched_turn = nullptr;
  std::uint64_t watched_turn_at = 0;
  // The game ran the minigame, and the game's own end was seen.
  bool ran = false;
  bool ended = false;
  bool gave_up_sent = false;
  // The server's win, which the game is let reach itself, and since when.
  std::optional<wire::DiceResult> server_result;
  std::uint64_t server_end_at = 0;
  // When the game closed its minigame without its own end.
  std::uint64_t closed_at = 0;
  // The state last seen in tick and since when (a state the game does not leave is logged), and
  // when the replica's AI was last asked, in which state.
  void* seen_state = nullptr;
  std::uint64_t seen_since = 0;
  bool seen_logged = false;
  std::uint64_t asked_at = 0;
  void* asked_kind = nullptr;
};

struct Vtables {
  void* initial = nullptr;
  void* badge_check = nullptr;
  void* player_turn = nullptr;
  void* rolling = nullptr;
  void* scoring = nullptr;
  void* passing = nullptr;
  void* ending = nullptr;
  void* end = nullptr;
  void* ai = nullptr;
  void* rerolling = nullptr;
  void* extra_rolling = nullptr;
  void* changing_value = nullptr;
  void* check_bust = nullptr;
  void* badge_selecting = nullptr;
  void* holding = nullptr;
};

Natives natives;
Vtables vtables;
SetState original_set_state = nullptr;
StateStep original_initial_next = nullptr;
StateStep original_check_next = nullptr;
AiAct original_ai_act = nullptr;
using TableLinks = std::int64_t(__fastcall*)(void* minigame);
TableLinks original_table_links = nullptr;
using BoardCamera = std::uint64_t(__fastcall*)(void* minigame, int mode);
BoardCamera original_board_camera = nullptr;
using SendMessage = std::uint64_t(__fastcall*)(void* binding, void* script, std::uint64_t wuid,
                                               const char* type, const char* value);
SendMessage original_send_message = nullptr;
DrawFace original_draw_face = nullptr;
BustChoice original_bust_choice = nullptr;
StateEnter original_check_bust_enter = nullptr;
// The passive badge check for a player, and a player's turn.
StateFor check_badges = nullptr;
StateFor turn_of = nullptr;
AssignInts assign_ints = nullptr;
std::optional<Match> current;
// Whether there is a current match, for other threads.
std::atomic<bool> seated{false};
std::vector<wire::DiceMove> to_send;
bool autoplay = false;

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class T>
void write(void* object, std::size_t offset, T value) {
  std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

void* vtable_of(const char* type_name) {
  const std::size_t at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size,
                                 reinterpret_cast<std::uintptr_t>(natives.image), type_name);
  if (at == kcd2s::bridge::knpos) {
    log_line("DICE the vtable of %s is missing", type_name);
    return nullptr;
  }
  return natives.image + at;
}

// The target of the call (E8 rel32) at `code` + offset, or null.
void* call_target(void* code, std::size_t offset) {
  if (!code) {
    return nullptr;
  }
  const auto* at = static_cast<const std::uint8_t*>(code) + offset;
  if (at[0] != 0xE8) {
    return nullptr;
  }
  const auto relative = read<std::int32_t>(at, 1);
  return const_cast<std::uint8_t*>(at + 5 + relative);
}

template <class Fn>
bool hook(void* target, void* detour, Fn* original, const char* name) {
  if (!target) {
    log_line("DICE %s is missing", name);
    return false;
  }
  if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    *original = nullptr;
    log_line("DICE could not hook %s %p", name, target);
    return false;
  }
  return true;
}

// The match's model and context, read each time: the minigame makes its model when it starts.
void* model_of(const Match& match) {
  return match.minigame ? read<void*>(match.minigame, kMinigameModel) : nullptr;
}

void* context_of(const Match& match) {
  void* model = model_of(match);
  return model ? static_cast<std::uint8_t*>(model) + kModelContext : nullptr;
}

void* local_player(void* context) {
  return read<void*>(context, kContextLocal);
}

void* opponent_player(void* context) {
  return read<void*>(context, kContextOpponent);
}

void* die_at(void* player, std::size_t index) {
  const auto first = read<std::uint8_t*>(player, kPlayerDice);
  const auto last = read<std::uint8_t*>(player, kPlayerDice + 8);
  if (!first || index >= std::size_t(last - first) / 16) {
    return nullptr;
  }
  return read<void*>(first, index * 16);
}

// The dice of a player's model in one of the states, as a mask.
std::uint8_t dice_in(void* player, std::uint8_t state) {
  std::uint8_t mask = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (die && read<std::uint8_t>(die, kDieState) == state) {
      mask = std::uint8_t(mask | (1u << index));
    }
  }
  return mask;
}

std::uint8_t dice_thrown(void* player) {
  std::uint8_t mask = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (die && read<std::uint8_t>(die, kDieState) != kDieSetAside) {
      mask = std::uint8_t(mask | (1u << index));
    }
  }
  return mask;
}

void release(volatile long* control) {
  if (!control) {
    return;
  }
  if (InterlockedDecrement(control + 2) == 0) {
    slot_of<void(__fastcall*)(volatile long*)>(const_cast<long*>(control), 0)(control);
    if (InterlockedDecrement(control + 3) == 0) {
      slot_of<void(__fastcall*)(volatile long*)>(const_cast<long*>(control), 1)(control);
    }
  }
}

void drop_held(Match& match) {
  release(match.held.control);
  match.held = {};
}

void send(wire::DiceMove::Kind kind, std::uint8_t held) {
  if (!current) {
    return;
  }
  wire::DiceMove move;
  move.match = current->id;
  move.kind = kind;
  move.held = held;
  to_send.push_back(move);
  log_line("DICE match %u: the local player's move %u (dice %02x)", current->id, unsigned(kind),
           held);
}

// The server's faces of a badge's dice (a reroll's selected dice, the Might badge's die), in the
// order the game draws them; the local player's throw (autoplay) takes them.
void draw_roll(Match& match, void* player, const Roll& roll) {
  match.drawn_faces.clear();
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (roll.rolled & (1u << index)) {
      match.drawn_faces.push_back(std::uint32_t(roll.faces[index]) - 1);
    }
  }
  write<float>(player, kPlayerThrowFixing, 0.0f);
  write<float>(player, kPlayerBustFixing, 0.0f);
  void* context = context_of(match);
  if (context && player == local_player(context) && match.own_roll) {
    for (std::size_t index = 0; index < kDiceCount; ++index) {
      if (roll.rolled & (1u << index)) {
        match.own_roll->faces[index] = roll.faces[index];
      }
    }
    match.own_roll->rolled = std::uint8_t(match.own_roll->rolled | roll.rolled);
  }
}

// A player's badge's uses left (0 without a badge).
int badge_uses(void* player) {
  void* badge = read<void*>(player, kPlayerBadge);
  return badge ? read<int>(badge, kBadgeUses) : 0;
}

// The server's throw goes into the player's next-throw override.
void give_throw(Match& match, void* player, const Roll& roll) {
  const std::uint8_t thrown = dice_thrown(player);
  if (thrown != roll.rolled) {
    log_line("DICE match %u: the game throws dice %02x, the server threw %02x", match.id, thrown,
             roll.rolled);
  }
  // The game counts a die's faces from 0 (a one) to 5 (a six).
  int faces[kDiceCount] = {};
  int count = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (roll.rolled & (1u << index)) {
      faces[count++] = int(roll.faces[index]) - 1;
    }
  }
  write<float>(player, kPlayerThrowFixing, 0.0f);
  write<float>(player, kPlayerBustFixing, 0.0f);
  IntVector from{faces, faces + count, faces + count};
  assign_ints(static_cast<std::uint8_t*>(player) + kPlayerOverrideFaces, &from);
  write<std::uint8_t>(player, kPlayerOverride, 1);
  void* context = context_of(match);
  if (context && player == local_player(context)) {
    match.own_roll = roll;
    match.told_selection = 0;
  } else {
    match.their_selection = 0;
  }
}

// The dice of a throw that score (autoplay): all six of a straight, else every face thrown three
// times or more, the 1s and 5s, and the jokers (face 7, a joker die's joker side) with the face
// they make three of a kind or more of (the game counts a joker only with a real die of it).
std::uint8_t scoring_dice(const Roll& roll) {
  std::array<int, 8> counts{};
  int thrown = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (roll.rolled & (1u << index)) {
      ++counts[roll.faces[index] & 7];
      ++thrown;
    }
  }
  const int jokers = counts[wire::kDiceJoker];
  bool straight = thrown == int(kSixDice);
  for (int face = 1; face <= 6; ++face) {
    straight = straight && counts[face] == 1;
  }
  // The face the jokers join: the most thrown of those they make three of a kind of.
  int joined = 0;
  for (int face = 1; face <= 6; ++face) {
    const bool kind = counts[face] >= 1 && counts[face] + jokers >= 3;
    if (jokers > 0 && kind && (joined == 0 || counts[face] >= counts[joined])) {
      joined = face;
    }
  }
  std::uint8_t keep = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (!(roll.rolled & (1u << index))) {
      continue;
    }
    const int face = roll.faces[index];
    const bool joker = face == wire::kDiceJoker;
    const bool kept = straight || (!joker && counts[face] >= 3) || face == 1 || face == 5 ||
                      face == joined || (joker && joined != 0);
    if (kept) {
      keep = std::uint8_t(keep | (1u << index));
    }
  }
  return keep;
}

// The server ended the match before the game did: the loser gives up the game's match (the local
// player when nobody won).
void force_end(Match& match, wire::DiceResult result) {
  drop_held(match);
  // A minigame the game has already closed (its opponent gone, say) has no state to end.
  if (match.ended || !match.minigame ||
      !slot_of<MinigameRuns>(match.minigame, kMinigameRuns)(match.minigame)) {
    match.ended = true;
    return;
  }
  void* context = context_of(match);
  void* model = model_of(match);
  if (!context || !model) {
    return;
  }
  void* state = read<void*>(model, kModelState);
  void* loser = result == wire::DiceResult::Won ? opponent_player(context) : local_player(context);
  match.ended = true;
  if (state) {
    slot_of<GiveUp>(state, kStateGiveUp)(state, context, loser);
  }
}

bool natural_end(Match& match, void* context) {
  void* model = model_of(match);
  const int target = read<int>(model, kModelTarget);
  return read<int>(local_player(context), kPlayerScore) >= target ||
         read<int>(opponent_player(context), kPlayerScore) >= target;
}

// A player's dice for the log: each die's face (1 to 6, 7 the joker) and state (t on the table,
// s selected, a set aside).
void log_dice(Match& match, void* player) {
  char text[kDiceCount * 3 + 1] = {};
  std::size_t at = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (!die) {
      continue;
    }
    const std::uint8_t state = read<std::uint8_t>(die, kDieState);
    text[at++] = char('1' + read<int>(die, kDieFace));
    text[at++] = state == kDieOnTable ? 't' : state == kDieSelected ? 's' : 'a';
    text[at++] = ' ';
  }
  log_line("DICE match %u: dice %s", match.id, text);
}

// A switch into a throwing state waits for the server's throw.
void hold(Match& match, const SharedState* next, bool local, Thrown thrown) {
  match.held.state = next->state;
  match.held.control = next->control;
  if (match.held.control) {
    InterlockedIncrement(match.held.control + 2);
  }
  match.held_local = local;
  match.held_thrown = thrown;
}

// The dice the local player keeps of their throw. The game marks them selected; a pass or throw
// from the hint's button can set them aside before its state, and then they are the
// selection the server was told, or else the set-aside dice of this throw.
std::uint8_t kept_dice(Match& match, void* player) {
  const std::uint8_t selected = dice_in(player, kDieSelected);
  if (selected != 0 || !match.own_roll) {
    return selected;
  }
  const std::uint8_t thrown = match.own_roll->rolled;
  std::uint8_t kept = std::uint8_t(match.told_selection & thrown);
  if (kept == 0) {
    kept = std::uint8_t(dice_in(player, kDieSetAside) & thrown);
  }
  log_line("DICE match %u: no die selected; the kept dice of the throw are %02x", match.id, kept);
  return kept;
}

// The local player banks: with their Doppelganger or Warlord badge when the turn kept its factor
// a bank by the Resurrection badge after a bust was told as the badge's choice.
void send_pass(Match& match, void* player) {
  void* model = model_of(match);
  void* state = model ? read<void*>(model, kModelState) : nullptr;
  if (state && *static_cast<void**>(state) == vtables.check_bust) {
    return;
  }
  const bool badge = state && *static_cast<void**>(state) == vtables.player_turn &&
                     read<float>(state, kTurnBadgeFactor) > 1.0f;
  send(badge ? wire::DiceMove::Kind::BadgePass : wire::DiceMove::Kind::Pass,
       kept_dice(match, player));
}

std::int64_t __fastcall hooked_set_state(void* context, const SharedState* next) {
  if (!current || !next || !next->state || context != context_of(*current)) {
    return original_set_state(context, next);
  }
  Match& match = *current;
  void* kind = *static_cast<void**>(next->state);
  void* player = slot_of<StatePlayer>(next->state, kStatePlayer)(next->state);
  const bool local = player == local_player(context);
  log_line("DICE match %u: state %p of %s (%zu own throws, %zu of the other's waiting)", match.id,
           kind, local ? "the local player" : "the other player", match.mine.size(),
           match.theirs.size());
  match.ran = true;
  if (kind == vtables.scoring || kind == vtables.passing || kind == vtables.changing_value) {
    log_dice(match, player);
  }
  if (match.held.state) {
    // The game moved on while a throw waited (the player left the table): the throw is dropped.
    drop_held(match);
  }
  if (kind == vtables.scoring && local) {
    send(wire::DiceMove::Kind::Continue, kept_dice(match, player));
  } else if (kind == vtables.passing && local) {
    send_pass(match, player);
  } else if (kind == vtables.changing_value && local) {
    // A Transmutation or Balatro badge's die: the game changes it as the server does.
    send(wire::DiceMove::Kind::Badge, dice_in(player, kDieSelected));
  } else if (kind == vtables.rerolling || kind == vtables.extra_rolling) {
    // A badge's dice: the server deals their faces, which the game draws.
    if (local) {
      const bool reroll = kind == vtables.rerolling;
      send(wire::DiceMove::Kind::Badge, reroll ? dice_in(player, kDieSelected) : 0);
    }
    auto& rolls = local ? match.mine : match.theirs;
    if (rolls.empty()) {
      hold(match, next, local, Thrown::BadgeDice);
      return 0;
    }
    draw_roll(match, player, rolls.front());
    rolls.pop_front();
    const std::int64_t result = original_set_state(context, next);
    if (current) {
      current->drawn_faces.clear();
    }
    return result;
  } else if ((kind == vtables.ending || kind == vtables.end) && !match.ended) {
    match.ended = true;
    if (!natural_end(match, context) && !match.gave_up_sent) {
      match.gave_up_sent = true;
      send(wire::DiceMove::Kind::GaveUp, 0);
    }
  } else if (kind == vtables.rolling) {
    auto& rolls = local ? match.mine : match.theirs;
    if (rolls.empty()) {
      hold(match, next, local, Thrown::Dice);
      // The local player throws now: the server deals the throw, to both games at once.
      if (local) {
        send(wire::DiceMove::Kind::Throw, 0);
      }
      return 0;
    }
    give_throw(match, player, rolls.front());
    rolls.pop_front();
  }
  const std::int64_t result = original_set_state(context, next);
  const bool scores = kind == vtables.scoring || kind == vtables.passing;
  if (scores && !read<std::uint8_t>(next->state, 32)) {
    // The game could not score the selection (the state's Enter, + 32).
    log_line("DICE match %u: the game did not score the selection", match.id);
  }
  return result;
}

// The match's second player sits at the table's second seat: the minigame's player and opponent
// links swap once it has read them.
std::int64_t __fastcall hooked_table_links(void* minigame) {
  const std::int64_t result = original_table_links(minigame);
  if (current && minigame == current->minigame && current->seat == 1) {
    for (const auto& pair : kSeatPairs) {
      const auto player = read<std::uint32_t>(minigame, pair[0]);
      write<std::uint32_t>(minigame, pair[0], read<std::uint32_t>(minigame, pair[1]));
      write<std::uint32_t>(minigame, pair[1], player);
    }
    log_line("DICE match %u: the local player takes the table's second seat", current->id);
  }
  return result;
}

// Who throws first is the server's. The game checks the first player's passive badges, then the
// other's, then gives the first player the turn, the first player being always the local one
// (the context's slot 1); when the opponent throws first, the chain runs from the opponent.
bool __fastcall hooked_initial_next(void* state, void* context) {
  if (current && context == context_of(*current) && !current->first) {
    check_badges(context, opponent_player(context));
    return true;
  }
  return original_initial_next(state, context);
}

bool __fastcall hooked_check_next(void* state, void* context) {
  if (current && context == context_of(*current) && !current->first) {
    void* player = slot_of<StatePlayer>(state, kStatePlayer)(state);
    if (player == opponent_player(context)) {
      check_badges(context, local_player(context));
    } else {
      turn_of(context, opponent_player(context));
    }
    return true;
  }
  return original_check_next(state, context);
}

// The replica's turn: the other player's next step (a kept die selected, or the dice scored and
// thrown on or banked), in place of its AI's.
// One step of selecting exactly the dice of a mask on the replica's table (in its turn or its
// badge's die selection): a die selected or let go. False when they are selected already.
bool select_step(Match& match, void* state, std::uint8_t mask) {
  void* context = context_of(match);
  void* player = opponent_player(context);
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (!die) {
      continue;
    }
    const std::uint8_t die_state = read<std::uint8_t>(die, kDieState);
    const bool wanted = (mask & (1u << index)) != 0;
    if (wanted && die_state == kDieOnTable) {
      slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                                 true);
      return true;
    }
    if (!wanted && die_state == kDieSelected) {
      slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                                 false);
      return true;
    }
    if (wanted && die_state != kDieSelected) {
      log_line("DICE match %u: the other player's die %zu is not on the table", match.id, index);
    }
  }
  return false;
}

// The other player used their badge on dice of their throw: the replica does the same in its
// turn (the Might badge's die, the selected dice thrown again, or the badge's die selection
// begun for a Transmutation or Balatro badge, played on in play_badge_choice).
bool play_badge(Match& match, void* state, const Move& move) {
  void* context = context_of(match);
  if (match.opponent_badge == kExtraDice) {
    match.moves.pop_front();
    return slot_of<StateAction>(state, kStateExtraDie)(state, context);
  }
  if (match.opponent_badge == kRerollDice || match.opponent_badge == kRerollPips) {
    if (select_step(match, state, move.held)) {
      return true;
    }
    match.moves.pop_front();
    return slot_of<StateAction>(state, kStateReroll)(state, context);
  }
  if (match.opponent_badge == kSetDice || match.opponent_badge == kBalatro) {
    if (!match.badge_begun) {
      match.badge_begun = true;
      return slot_of<StateAction>(state, kStateBadgeSelect)(state, context);
    }
    return false;
  }
  log_line("DICE match %u: the other player's badge %d is not one to use", match.id,
           match.opponent_badge);
  match.moves.pop_front();
  return false;
}

// The replica's badge die selection (BadgeSelecting): the die selected, then the change
// confirmed.
void play_badge_choice(Match& match, void* state) {
  const Move move = match.moves.front();
  match.last_step_at = GetTickCount64();
  if (move.kind != wire::DiceState::Move::Badge || select_step(match, state, move.held)) {
    return;
  }
  match.moves.pop_front();
  match.badge_begun = false;
  slot_of<StateAction>(state, kStateBadgeConfirm)(state, context_of(match));
}

bool play_move(Match& match, void* state) {
  void* context = context_of(match);
  void* player = opponent_player(context);
  const Move move = match.moves.front();
  match.last_step_at = GetTickCount64();
  if (move.kind == wire::DiceState::Move::Badge) {
    return play_badge(match, state, move);
  }
  if (move.kind == wire::DiceState::Move::BustChoice) {
    return false;
  }
  if (select_step(match, state, move.held)) {
    return true;
  }
  match.moves.pop_front();
  if (move.kind == wire::DiceState::Move::Pass || move.kind == wire::DiceState::Move::BadgePass) {
    const bool badge = move.kind == wire::DiceState::Move::BadgePass;
    return slot_of<Pass>(state, kStatePass)(state, context, badge);
  }
  if (slot_of<MayContinue>(state, kStateMayContinue)(state, context, player)) {
    return slot_of<Continue>(state, kStateContinue)(state, context);
  }
  log_line("DICE match %u: the game would not let the other player continue", match.id);
  return false;
}

// The local player's turn as its PlayerTurn state, else null (autoplay).
void* local_turn(Match& match) {
  void* model = model_of(match);
  void* context = context_of(match);
  void* state = model ? read<void*>(model, kModelState) : nullptr;
  if (!state || *static_cast<void**>(state) != vtables.player_turn ||
      slot_of<StatePlayer>(state, kStatePlayer)(state) != local_player(context)) {
    return nullptr;
  }
  return state;
}

// The dice of a throw that make the local player's Formations badge's combination (tin the Cut
// 3+5, silver the Gallows 4-5-6, gold the Eye 1-3-5), a die of each face; none when one is
// missing (autoplay).
std::uint8_t formation_dice(const Roll& roll, int tier) {
  static const std::vector<int> kFormationFaces[] = {{3, 5}, {4, 5, 6}, {1, 3, 5}};
  if (tier < 0 || tier > 2) {
    return 0;
  }
  std::uint8_t dice = 0;
  for (const int face : kFormationFaces[tier]) {
    bool found = false;
    for (std::size_t index = 0; index < kDiceCount && !found; ++index) {
      const bool free = (roll.rolled & (1u << index)) && !(dice & (1u << index));
      if (free && roll.faces[index] == face) {
        dice = std::uint8_t(dice | (1u << index));
        found = true;
      }
    }
    if (!found) {
      return 0;
    }
  }
  return dice;
}

// The local player's dice on the table as the game shows them now (after a badge changed them).
void refresh_own_roll(Match& match, void* player) {
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (die && read<std::uint8_t>(die, kDieState) != kDieSetAside) {
      match.own_roll->faces[index] = std::uint8_t(read<int>(die, kDieFace) + 1);
      match.own_roll->rolled = std::uint8_t(match.own_roll->rolled | (1u << index));
    }
  }
}

// A die of the local player's throw autoplay uses a badge on: a joker die not showing its joker
// for a Balatro badge whose throw still scores with the joker, else a die that does not score
// (any die when all score).
std::optional<std::size_t> autoplay_badge_die(Match& match, std::uint8_t keep) {
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (!(match.own_roll->rolled & (1u << index))) {
      continue;
    }
    if (match.own_badge == kBalatro) {
      const bool joker_die = index < match.own_sides.size() &&
                             std::find(match.own_sides[index].begin(), match.own_sides[index].end(),
                                       std::uint8_t(6)) != match.own_sides[index].end();
      Roll changed = *match.own_roll;
      changed.faces[index] = wire::kDiceJoker;
      const bool still_scores = scoring_dice(changed) != 0;
      if (joker_die && match.own_roll->faces[index] != wire::kDiceJoker && still_scores) {
        return index;
      }
      continue;
    }
    if (!(keep & (1u << index))) {
      return index;
    }
  }
  if (match.own_badge == kBalatro) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (match.own_roll->rolled & (1u << index)) {
      return index;
    }
  }
  return std::nullopt;
}

// Autoplay uses the local player's badge once a throw when it may (the kit): the Might badge's
// die, a die thrown again (Fortune, Swap-out: the die selected, then thrown again a step later, as
// selecting switches the game's state), a die changed (Transmutation, Balatro: its selection begun
// here, played on in autoplay_badge_choice). True when it took the step.
bool autoplay_badge(Match& match, void* state, std::uint8_t keep) {
  void* context = context_of(match);
  void* player = local_player(context);
  if (match.autoplay_badge_step == 1) {
    match.autoplay_badge_step = 2;
    slot_of<StateAction>(state, kStateReroll)(state, context);
    return true;
  }
  if (match.autoplay_badge_step != 0 || badge_uses(player) <= 0 ||
      dice_in(player, kDieSelected) != 0) {
    return false;
  }
  if (match.own_badge == kExtraDice && !die_at(player, kSixDice)) {
    match.autoplay_badge_step = 2;
    slot_of<StateAction>(state, kStateExtraDie)(state, context);
    return true;
  }
  const bool reroll = match.own_badge == kRerollDice || match.own_badge == kRerollPips;
  const bool change = match.own_badge == kSetDice || match.own_badge == kBalatro;
  if (!reroll && !change) {
    return false;
  }
  const auto index = autoplay_badge_die(match, keep);
  if (!index) {
    return false;
  }
  if (change) {
    match.autoplay_badge_step = 2;
    slot_of<StateAction>(state, kStateBadgeSelect)(state, context);
    return true;
  }
  match.autoplay_badge_step = 1;
  void* die = die_at(player, *index);
  slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                             true);
  return true;
}

// Autoplay's badge die selection (BadgeSelecting): the die selected, then the change confirmed.
void autoplay_badge_choice(Match& match, void* state) {
  void* context = context_of(match);
  void* player = local_player(context);
  match.last_step_at = GetTickCount64();
  if (!match.own_roll) {
    return;
  }
  refresh_own_roll(match, player);
  const auto index = autoplay_badge_die(match, scoring_dice(*match.own_roll));
  if (index && dice_in(player, kDieSelected) == 0) {
    void* die = die_at(player, *index);
    slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                               true);
    return;
  }
  slot_of<StateAction>(state, kStateBadgeConfirm)(state, context);
}

// Autoplay's next step of the local player's turn: its badge used, a scoring die selected, or
// the dice scored and the turn thrown on or banked (with a Doppelganger or Warlord badge when it
// has uses left).
void autoplay_step(Match& match, void* state) {
  void* context = context_of(match);
  void* player = local_player(context);
  refresh_own_roll(match, player);
  const Roll roll = *match.own_roll;
  std::uint8_t keep = scoring_dice(roll);
  if (match.own_badge == kFormations) {
    keep = std::uint8_t(keep | formation_dice(roll, match.own_badge_tier));
  }
  match.last_step_at = GetTickCount64();
  if (autoplay_badge(match, state, keep)) {
    return;
  }
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if (!(keep & (1u << index))) {
      continue;
    }
    void* die = die_at(player, index);
    if (die && read<std::uint8_t>(die, kDieState) == kDieOnTable) {
      slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                                 true);
      return;
    }
  }
  match.own_roll.reset();
  int left = 0;
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    if ((roll.rolled & ~keep) & (1u << index)) {
      ++left;
    }
  }
  const bool throw_on = left == 0 || left >= 3;
  match.autoplay_badge_step = 0;
  if (throw_on && slot_of<MayContinue>(state, kStateMayContinue)(state, context, player)) {
    slot_of<Continue>(state, kStateContinue)(state, context);
    return;
  }
  const bool banking = match.own_badge == kDoubleTake || match.own_badge == kMultiplier;
  slot_of<Pass>(state, kStatePass)(state, context, banking && badge_uses(player) > 0);
}

// The other player's selection so far shows on the replica's dice in its turn.
void show_their_selection(Match& match, void* state) {
  void* context = context_of(match);
  void* player = opponent_player(context);
  for (std::size_t index = 0; index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (!die) {
      continue;
    }
    const std::uint8_t die_state = read<std::uint8_t>(die, kDieState);
    const bool wanted = (match.their_selection & (1u << index)) != 0;
    const bool selected = die_state == kDieSelected;
    if ((die_state == kDieOnTable || selected) && wanted != selected) {
      slot_of<SelectDie>(state, kStateSelectDie)(state, context, read<std::uint32_t>(die, kDieId),
                                                 wanted);
    }
  }
}

// Whether the minigame's current state is one of the local player's.
bool own_state(Match& match) {
  void* model = model_of(match);
  void* state = model ? read<void*>(model, kModelState) : nullptr;
  return state &&
         slot_of<StatePlayer>(state, kStatePlayer)(state) == local_player(context_of(match));
}

// The board camera (mode 1: above the thrower's board, looking down at it, then turned toward a
// die). For the other player's throw the game turned it toward a die off the board (it looked
// up at the roof or at the other player's face, live, from either seat), so there, but in the
// local player's own states, it looks straight down at the board.
// The board camera's turn for the other player's board: as the game's own board camera looks at
// the local player's board, from the local player's seat (facing the other player) down at it.
// Turned toward no die, the game keeps the camera's last turn, and in the match's first turn
// there was none (NaN: the other player watched the first turn from inside the thrower, live).
void look_down_at_board(void* minigame, const Match& match) {
  void* opponent = kcds::runtime::props::entity::by_id(match.opponent);
  if (!opponent) {
    return;
  }
  float position[3] = {};
  float rotation[4] = {};
  kcds::runtime::props::entity::world_pose(opponent, position, rotation);
  const float x = rotation[0];
  const float y = rotation[1];
  const float z = rotation[2];
  const float w = rotation[3];
  const float opponent_yaw = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
  const float yaw = opponent_yaw + kPi;
  const float half_yaw_sin = std::sin(yaw * 0.5f);
  const float half_yaw_cos = std::cos(yaw * 0.5f);
  const float half_pitch_sin = std::sin(kBoardLookPitch * 0.5f);
  const float half_pitch_cos = std::cos(kBoardLookPitch * 0.5f);
  // The turn about z (the seat's heading), then about x (the look down).
  float* turn = reinterpret_cast<float*>(static_cast<std::uint8_t*>(minigame) + kBoardCameraTurn);
  turn[0] = half_yaw_cos * half_pitch_sin;
  turn[1] = half_yaw_sin * half_pitch_sin;
  turn[2] = half_yaw_sin * half_pitch_cos;
  turn[3] = half_yaw_cos * half_pitch_cos;
}

std::vector<std::uint32_t> board_dice(const Match& match, void* player);

// The middle of the other player's dice that lie on the table (the thrower's view puts them
// there, dice/dice_tick.cpp: within kDieOnTableReach of the other player's body, shown), and
// that body's place; false while none does (still in the cup, at the world's origin).
constexpr float kDieOnTableReach = 1.5f;

bool their_dice_middle(const Match& match, float middle[3], float body[3]) {
  void* opponent = kcds::runtime::props::entity::by_id(match.opponent);
  if (!opponent) {
    return false;
  }
  kcds::runtime::props::entity::world_position(opponent, body);
  int lying = 0;
  middle[0] = middle[1] = middle[2] = 0.0f;
  for (const std::uint32_t id : board_dice(match, opponent_player(context_of(match)))) {
    void* die = kcds::runtime::props::entity::by_id(id);
    if (!die || kcds::runtime::props::entity::is_hidden(die)) {
      continue;
    }
    float at[3];
    kcds::runtime::props::entity::world_position(die, at);
    const float dx = at[0] - body[0];
    const float dy = at[1] - body[1];
    if (dx * dx + dy * dy > kDieOnTableReach * kDieOnTableReach) {
      continue;
    }
    for (int axis = 0; axis < 3; ++axis) {
      middle[axis] += at[axis];
    }
    ++lying;
  }
  if (lying == 0) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    middle[axis] /= float(lying);
  }
  return true;
}

// In the other player's turn the board camera frames their dice once they lie on the table:
// above them, a little toward the local player's side, looking down at them (the game's own
// camera turned toward a die looked at the wall behind the table on the other screen). Its
// place at + 1128, its turn at + 1140, turned about z (the heading, looking along +y), then
// about x (the look down), as look_down_at_board turns it.
constexpr std::size_t kBoardCameraPlace = 1128;
constexpr float kFrameHeight = 0.55f;
constexpr float kFrameBack = 0.3f;

bool frame_their_dice(void* minigame, const Match& match) {
  float middle[3];
  float body[3];
  if (!their_dice_middle(match, middle, body)) {
    return false;
  }
  // Away from the other player's body, toward the local player's seat.
  float away[2] = {middle[0] - body[0], middle[1] - body[1]};
  const float length = std::sqrt(away[0] * away[0] + away[1] * away[1]);
  if (length > 0.01f) {
    away[0] /= length;
    away[1] /= length;
  }
  auto* place = reinterpret_cast<float*>(static_cast<std::uint8_t*>(minigame) + kBoardCameraPlace);
  place[0] = middle[0] + away[0] * kFrameBack;
  place[1] = middle[1] + away[1] * kFrameBack;
  place[2] = middle[2] + kFrameHeight;
  const float dx = middle[0] - place[0];
  const float dy = middle[1] - place[1];
  const float dz = middle[2] - place[2];
  const float yaw = std::atan2(-dx, dy);
  const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
  const float half_yaw_sin = std::sin(yaw * 0.5f);
  const float half_yaw_cos = std::cos(yaw * 0.5f);
  const float half_pitch_sin = std::sin(pitch * 0.5f);
  const float half_pitch_cos = std::cos(pitch * 0.5f);
  float* turn = reinterpret_cast<float*>(static_cast<std::uint8_t*>(minigame) + kBoardCameraTurn);
  turn[0] = half_yaw_cos * half_pitch_sin;
  turn[1] = half_yaw_sin * half_pitch_sin;
  turn[2] = half_yaw_sin * half_pitch_cos;
  turn[3] = half_yaw_cos * half_pitch_cos;
  return true;
}

std::uint64_t __fastcall hooked_board_camera(void* minigame, int mode) {
  if (!current || minigame != current->minigame || mode != 1 || own_state(*current)) {
    return original_board_camera(minigame, mode);
  }
  const int die = read<int>(minigame, kBoardCameraDie);
  write<int>(minigame, kBoardCameraDie, -1);
  const std::uint64_t result = original_board_camera(minigame, mode);
  write<int>(minigame, kBoardCameraDie, die);
  if (!frame_their_dice(minigame, *current)) {
    look_down_at_board(minigame, *current);
  }
  return result;
}

// The entity ids of a player's dice on the board, in the order of the throw: the player's board
// (C_Dice + 1200 + 16 x the player's index, + 88 of the player's model), the board's dice at + 240
// and a die's entity id at + 48, as the game reads a die's place.
std::vector<std::uint32_t> board_dice(const Match& match, void* player) {
  constexpr std::size_t kBoards = 1200;
  constexpr std::size_t kBoardStride = 16;
  constexpr std::size_t kPlayerIndex = 88;
  constexpr std::size_t kBoardDice = 240;
  constexpr std::size_t kDieEntity = 48;
  std::vector<std::uint32_t> ids;
  const int index = player ? read<int>(player, kPlayerIndex) : -1;
  void* board = index == 0 || index == 1
                    ? read<void*>(match.minigame, kBoards + kBoardStride * std::size_t(index))
                    : nullptr;
  if (!board) {
    return ids;
  }
  const auto first = read<std::uint8_t*>(board, kBoardDice);
  const auto last = read<std::uint8_t*>(board, kBoardDice + 8);
  for (std::uint8_t* entry = first; entry && entry < last; entry += 16) {
    void* die = read<void*>(entry, 0);
    const std::uint32_t id = die ? read<std::uint32_t>(die, kDieEntity) : 0;
    if (id) {
      ids.push_back(id);
    }
  }
  return ids;
}

// A player's dice of the game's kinds show with their meshes: the game spawns every die as its
// plain die (dice_basic.cgf).
void show_dice_kinds(const Match& match, void* player, const std::vector<std::string>& meshes) {
  if (meshes.empty()) {
    return;
  }
  const std::vector<std::uint32_t> ids = board_dice(match, player);
  for (std::size_t index = 0; index < ids.size() && index < meshes.size(); ++index) {
    void* entity = kcds::runtime::props::entity::by_id(ids[index]);
    if (!entity) {
      continue;
    }
    const std::string shown =
        kcds::runtime::props::entity::mesh_file(entity, meshes[index].size() + 1);
    if (_stricmp(shown.c_str(), meshes[index].c_str()) == 0) {
      continue;
    }
    const bool loaded = kcds::runtime::props::entity::show_mesh(entity, meshes[index]);
    log_line("DICE match %u: die %zu shows %s: %s", match.id, index, meshes[index].c_str(),
             loaded ? "loaded" : "the game could not load it");
  }
}

// A dice table's "Play dice" (DiceInteractor:OnUsed sends its behaviour "dice:init"): the server
// is told the local player used a table (TableUse); the game's own reaction stays.
std::uint64_t __fastcall hooked_send_message(void* binding, void* script, std::uint64_t wuid,
                                             const char* type, const char* value) {
  if (type && std::strcmp(type, "dice:init") == 0) {
    wire::DiceMove move;
    move.match = current ? current->id : 0;
    move.kind = wire::DiceMove::Kind::TableUse;
    to_send.push_back(move);
    log_line("DICE the local player used a dice table");
  }
  return original_send_message(binding, script, wuid, type, value);
}

// A player's dice of the game's kinds count their sides as the kinds do: each die model's side
// values (C_DieModel + 0xa0, six ints; the plain die's are 0 to 5), so a joker side (6) is one of
// the die's own when the server deals it.
void give_dice_sides(void* player, const std::vector<std::array<std::uint8_t, 6>>& sides) {
  constexpr std::size_t kDieSides = 0xa0;
  for (std::size_t index = 0; index < sides.size() && index < kDiceCount; ++index) {
    void* die = die_at(player, index);
    if (!die) {
      continue;
    }
    for (std::size_t side = 0; side < 6; ++side) {
      const int value = sides[index][side];
      if (read<int>(die, kDieSides + side * sizeof(int)) != value) {
        write<int>(die, kDieSides + side * sizeof(int), value);
      }
    }
  }
}

// The replica's turn as its PlayerTurn state, else null.
void* opponent_turn(Match& match) {
  void* model = model_of(match);
  void* context = context_of(match);
  void* state = model ? read<void*>(model, kModelState) : nullptr;
  if (!state || *static_cast<void**>(state) != vtables.player_turn ||
      slot_of<StatePlayer>(state, kStatePlayer)(state) != opponent_player(context)) {
    return nullptr;
  }
  return state;
}

// The minigame asks the players' AIs to act when it is ready for a next step,
// and steps the state when none does, which in a turn is a pass. The replica's AI never acts:
// in its turn the other player's move is played, or the turn waits for it (handled, and the
// move is played in tick when it comes); in its other states the game steps on.
bool __fastcall hooked_ai_act(void* ai, void* model, void* player, void* state,
                              std::uint8_t* handled) {
  if (!current || model != model_of(*current)) {
    return original_ai_act(ai, model, player, state, handled);
  }
  Match& match = *current;
  *handled = 0;
  if (player != opponent_player(context_of(match))) {
    return false;
  }
  match.asked_at = GetTickCount64();
  match.asked_kind = state ? *static_cast<void**>(state) : nullptr;
  // The replica's badge die selection is played from the other player's move, not stepped on
  // (the AI is asked with the state of whichever player is on); a die held in it steps back.
  void* kind = state ? *static_cast<void**>(state) : nullptr;
  const bool selecting = kind == vtables.badge_selecting;
  if (selecting && slot_of<StatePlayer>(state, kStatePlayer)(state) == player &&
      current->badge_begun) {
    *handled = 1;
    return true;
  }
  if (state != opponent_turn(match)) {
    return false;
  }
  *handled = 1;
  if (match.moves.empty()) {
    match.waiting_move = true;
    return true;
  }
  return play_move(match, state);
}

// The game draws a die's face from its side weights: a badge's dice the server dealt
// take the server's faces.
std::uint32_t __fastcall hooked_draw_face(void* die_sides, void* random) {
  if (current && !current->drawn_faces.empty()) {
    const std::uint32_t side = current->drawn_faces.front();
    current->drawn_faces.pop_front();
    return side;
  }
  return original_draw_face(die_sides, random);
}

// The local player's choice after a bust with their Resurrection badge (0 bank, 1
// throw again, 2 the bust) goes to the server.
bool __fastcall hooked_bust_choice(void* model, char choice) {
  if (current && model == model_of(*current) && choice >= 0 && choice <= 2) {
    void* state = read<void*>(model, kModelState);
    void* context = context_of(*current);
    if (state && *static_cast<void**>(state) == vtables.check_bust &&
        slot_of<StatePlayer>(state, kStatePlayer)(state) == local_player(context)) {
      send(wire::DiceMove::Kind::BustChoice, std::uint8_t(choice));
    }
  }
  return original_bust_choice(model, choice);
}

// The Resurrection badge's check after a bust asks the replica's AI at once: the
// other player's choice comes from the server instead, played in tick.
char __fastcall hooked_check_bust_enter(void* state, void* context) {
  if (current && context == context_of(*current) && current->opponent_badge == kAntiBust) {
    void* player = slot_of<StatePlayer>(state, kStatePlayer)(state);
    if (player == opponent_player(context) && badge_uses(player) > 0) {
      log_line("DICE match %u: the other player's Resurrection badge waits for their choice",
               current->id);
      return 0;
    }
  }
  return original_check_bust_enter(state, context);
}

// The current state of the match's model, or null.
void* state_of(Match& match) {
  void* model = model_of(match);
  return model ? read<void*>(model, kModelState) : nullptr;
}

// The other player's next move, a step: in the replica's turn, its badge die selection, or the
// Resurrection badge's check after a bust (the choice also comes for the local player when the
// server took the bust for them).
void step_move(Match& match) {
  void* state = state_of(match);
  if (!state) {
    return;
  }
  void* kind = *static_cast<void**>(state);
  const Move& move = match.moves.front();
  if (move.kind == wire::DiceState::Move::BustChoice) {
    if (kind == vtables.check_bust) {
      const char choice = char(move.held);
      match.moves.pop_front();
      match.last_step_at = GetTickCount64();
      original_bust_choice(model_of(match), choice);
    }
    return;
  }
  void* player = slot_of<StatePlayer>(state, kStatePlayer)(state);
  if (player != opponent_player(context_of(match))) {
    return;
  }
  if (kind == vtables.badge_selecting) {
    play_badge_choice(match, state);
  } else if (state == opponent_turn(match)) {
    match.waiting_move = false;
    play_move(match, state);
  }
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  vtables.initial = vtable_of(".?AVC_InitialDiceGameState@playermodule@wh@@");
  vtables.badge_check = vtable_of(".?AVC_CheckPassiveBadgeDiceGameState@playermodule@wh@@");
  vtables.player_turn = vtable_of(".?AVC_PlayerTurnDiceGameState@playermodule@wh@@");
  vtables.rolling = vtable_of(".?AVC_RollingDiceGameState@playermodule@wh@@");
  vtables.scoring = vtable_of(".?AVC_ScoringDiceGameState@playermodule@wh@@");
  vtables.passing = vtable_of(".?AVC_PassingDiceGameState@playermodule@wh@@");
  vtables.ending = vtable_of(".?AVC_EndingDiceGameState@playermodule@wh@@");
  vtables.end = vtable_of(".?AVC_EndDiceGameState@playermodule@wh@@");
  vtables.ai = vtable_of(".?AVC_SimpleDiceAI@playermodule@wh@@");
  vtables.rerolling = vtable_of(".?AVC_ReRollingDiceGameState@playermodule@wh@@");
  vtables.extra_rolling = vtable_of(".?AVC_ExtraRollingDiceGameState@playermodule@wh@@");
  vtables.changing_value = vtable_of(".?AVC_ChangingDieValueDiceGameState@playermodule@wh@@");
  vtables.check_bust = vtable_of(".?AVC_CheckBustBadgeDiceGameState@playermodule@wh@@");
  vtables.badge_selecting = vtable_of(".?AVC_BadgeDiceSelectingDiceGameState@playermodule@wh@@");
  vtables.holding = vtable_of(".?AVC_HoldingDiceGameState@playermodule@wh@@");
  log_line("DICE states: player turn %p, rolling %p, scoring %p, passing %p, ending %p, end %p",
           vtables.player_turn, vtables.rolling, vtables.scoring, vtables.passing, vtables.ending,
           vtables.end);
  void* initial_next = vtables.initial ? static_cast<void**>(vtables.initial)[kStateNext] : nullptr;
  void* check_next =
      vtables.badge_check ? static_cast<void**>(vtables.badge_check)[kStateNext] : nullptr;
  check_badges = reinterpret_cast<StateFor>(call_target(initial_next, kInitialCheckCall));
  turn_of = reinterpret_cast<StateFor>(call_target(check_next, kCheckTurnCall));
  assign_ints =
      reinterpret_cast<AssignInts>(call_target(natives.throw_override, kOverrideAssignCall));
  void* ai_act = vtables.ai ? static_cast<void**>(vtables.ai)[kAiAct] : nullptr;
  void* check_bust_enter =
      vtables.check_bust ? static_cast<void**>(vtables.check_bust)[kStateEnter] : nullptr;
  if (!natives.local_minigame || !natives.start_with_score || !check_badges || !turn_of ||
      !assign_ints || !vtables.player_turn || !vtables.rolling || !vtables.scoring ||
      !vtables.passing || !vtables.ending || !vtables.end) {
    log_line("DICE natives are missing");
    return false;
  }
  return hook(natives.set_state, reinterpret_cast<void*>(&hooked_set_state), &original_set_state,
              "the model's SetState") &&
         hook(initial_next, reinterpret_cast<void*>(&hooked_initial_next), &original_initial_next,
              "the Initial state's step") &&
         hook(check_next, reinterpret_cast<void*>(&hooked_check_next), &original_check_next,
              "the passive badge check's step") &&
         hook(natives.table_links, reinterpret_cast<void*>(&hooked_table_links),
              &original_table_links, "the minigame's table links") &&
         hook(natives.board_camera, reinterpret_cast<void*>(&hooked_board_camera),
              &original_board_camera, "the minigame's board camera") &&
         hook(natives.send_message, reinterpret_cast<void*>(&hooked_send_message),
              &original_send_message, "the script binding SendMessageToEntity") &&
         hook(ai_act, reinterpret_cast<void*>(&hooked_ai_act), &original_ai_act,
              "the dice AI's turn") &&
         hook(natives.draw_face, reinterpret_cast<void*>(&hooked_draw_face), &original_draw_face,
              "a die's face drawn") &&
         hook(natives.bust_choice, reinterpret_cast<void*>(&hooked_bust_choice),
              &original_bust_choice, "the Resurrection badge's choice") &&
         hook(check_bust_enter, reinterpret_cast<void*>(&hooked_check_bust_enter),
              &original_check_bust_enter, "the Resurrection badge's check");
}

bool start(const Start& start) {
  if (current) {
    log_line("DICE match %u refused: match %u is still on", start.match, current->id);
    return false;
  }
  void* minigame = reinterpret_cast<LocalMinigame>(natives.local_minigame)();
  if (!minigame) {
    log_line("DICE match %u: no dice minigame for the local player", start.match);
    return false;
  }
  Match match;
  match.id = start.match;
  match.minigame = minigame;
  match.seat = start.seat;
  match.first = start.first;
  match.own_name = start.own_name;
  match.opponent_name = start.opponent_name;
  match.opponent = start.opponent;
  match.own_dice = start.own_dice;
  match.opponent_dice = start.opponent_dice;
  match.own_sides = start.own_sides;
  match.opponent_sides = start.opponent_sides;
  match.own_badge = start.own_badge;
  match.own_badge_tier = start.own_badge_tier;
  match.opponent_badge = start.opponent_badge;
  current = std::move(match);
  const std::uint16_t badge = kNoBadge;
  const bool started = reinterpret_cast<StartWithScore>(natives.start_with_score)(
      minigame, start.table, start.opponent, int(start.target), &badge, false);
  log_line("DICE match %u: %s at table %u against %u (target %u, %s first)", start.match,
           started ? "started" : "the game would not start it", start.table, start.opponent,
           start.target, start.first ? "local" : "opponent");
  if (!started) {
    current.reset();
  }
  return started;
}

void names(std::uint32_t match, const std::string& own, const std::string& opponent) {
  if (!current || current->id != match) {
    return;
  }
  current->own_name = own;
  current->opponent_name = opponent;
}

void roll(std::uint32_t match, bool mine, const std::array<std::uint8_t, wire::kDiceCount>& faces,
          std::uint8_t rolled) {
  if (!current || current->id != match) {
    return;
  }
  Roll one;
  one.faces = faces;
  one.rolled = rolled;
  (mine ? current->mine : current->theirs).push_back(one);
}

void opponent_move(std::uint32_t match, wire::DiceState::Move kind, std::uint8_t held) {
  if (!current || current->id != match) {
    return;
  }
  current->moves.push_back(Move{kind, held});
}

void opponent_select(std::uint32_t match, std::uint8_t selected) {
  if (!current || current->id != match) {
    return;
  }
  current->their_selection = selected;
}

void end(std::uint32_t match, wire::DiceEnd reason, wire::DiceResult result) {
  if (!current || current->id != match) {
    return;
  }
  Match& own = *current;
  if (reason == wire::DiceEnd::Won && !own.ended) {
    // A win the game reaches itself once it has played the last move: it is let end on its own.
    own.server_result = result;
    own.server_end_at = GetTickCount64();
    return;
  }
  force_end(own, result);
  log_line("DICE match %u over", match);
  current.reset();
}

bool at_table() {
  return seated.load();
}

std::uint32_t opponent_die(std::size_t index) {
  if (!current || !current->minigame) {
    return 0;
  }
  const std::vector<std::uint32_t> ids =
      board_dice(*current, opponent_player(context_of(*current)));
  return index < ids.size() ? ids[index] : 0;
}

void* match_minigame() {
  return current ? current->minigame : nullptr;
}

void tick() {
  seated.store(current.has_value());
  if (!current) {
    return;
  }
  Match& match = *current;
  const std::uint64_t now = GetTickCount64();
  if (match.server_result) {
    const bool runs = slot_of<MinigameRuns>(match.minigame, kMinigameRuns)(match.minigame);
    if (match.ended || !runs || now - match.server_end_at >= kEndGraceMilliseconds) {
      if (!match.ended && runs) {
        log_line("DICE match %u: the game did not reach the server's win", match.id);
      }
      force_end(match, *match.server_result);
      log_line("DICE match %u over", match.id);
      current.reset();
      seated.store(false);
      return;
    }
  }
  if (match.held.state) {
    auto& rolls = match.held_local ? match.mine : match.theirs;
    void* context = context_of(match);
    if (!rolls.empty() && context) {
      const SharedState held = match.held;
      match.held = {};
      void* player = slot_of<StatePlayer>(held.state, kStatePlayer)(held.state);
      if (match.held_thrown == Thrown::Dice) {
        give_throw(match, player, rolls.front());
      } else {
        draw_roll(match, player, rolls.front());
      }
      rolls.pop_front();
      original_set_state(context, &held);
      if (current) {
        current->drawn_faces.clear();
      }
      release(held.control);
      if (!current) {
        return;
      }
    }
  }
  // The other player's move goes on step by step, a die every kStepMilliseconds, also when the
  // game does not ask the replica's AI again.
  if (!match.moves.empty() && now - match.last_step_at >= kStepMilliseconds) {
    step_move(match);
  }
  // A state the game does not leave for a while is logged once. The replica's scoring and banking
  // step on by themselves when the game stopped asking its AI (it did, at times, after the other
  // player's badge changed a die).
  if (void* state = state_of(match)) {
    void* kind = *static_cast<void**>(state);
    const bool steps_on = kind == vtables.scoring || kind == vtables.passing;
    void* player = slot_of<StatePlayer>(state, kStatePlayer)(state);
    const bool replica = player == opponent_player(context_of(match));
    if (state != match.seen_state) {
      match.seen_state = state;
      match.seen_since = now;
      match.seen_logged = false;
    } else if (steps_on && replica && match.asked_at < match.seen_since &&
               now - match.seen_since >= kStepOnMilliseconds) {
      log_line("DICE match %u: the replica's state %p steps on by itself", match.id, kind);
      match.seen_since = now;
      slot_of<StateStep>(state, kStateNext)(state, context_of(match));
      return;
    } else if (!match.seen_logged && now - match.seen_since >= 8000) {
      match.seen_logged = true;
      log_line(
          "DICE match %u: still in state %p of %s (%zu moves, %zu own throws, %zu theirs; "
          "the replica's AI last asked %llu ms ago in %p)",
          match.id, kind, replica ? "the other player" : "the local player", match.moves.size(),
          match.mine.size(), match.theirs.size(),
          static_cast<unsigned long long>(now - match.asked_at), match.asked_kind);
    }
  }
  // Both players' dice of the game's kinds show with their meshes, four times a second (the
  // game spawns the board's dice as the match goes on).
  if (now - match.dice_shown_at >= kDiceKindsMilliseconds) {
    match.dice_shown_at = now;
    void* context = context_of(match);
    show_dice_kinds(match, local_player(context), match.own_dice);
    show_dice_kinds(match, opponent_player(context), match.opponent_dice);
    give_dice_sides(local_player(context), match.own_sides);
    give_dice_sides(opponent_player(context), match.opponent_sides);
  }
  // The other player's selection shows on the replica's dice as it comes, while no move of
  // theirs waits to be played; their thrown dice are watched from above, as the game's own turn
  // shows them (it did not always switch to that camera for the other player's turn).
  if (void* state = opponent_turn(match)) {
    if (match.moves.empty()) {
      show_their_selection(match, state);
    }
    // Put on again twice a second: the other player's dice come to lie on the table while the
    // turn goes on, and the game's camera turns toward one only then.
    constexpr std::uint64_t kCameraAgainMilliseconds = 500;
    if (state != match.watched_turn || read<int>(match.minigame, kCameraMode) != kBoardCamera ||
        now - match.watched_turn_at >= kCameraAgainMilliseconds) {
      match.watched_turn = state;
      match.watched_turn_at = now;
      hooked_board_camera(match.minigame, kBoardCamera);
    }
    // Every frame the dice glide: the camera stays on them.
    if (read<int>(match.minigame, kCameraMode) == kBoardCamera) {
      frame_their_dice(match.minigame, match);
    }
  }
  // The local player's selection goes to the server as it changes, for the other player's game.
  if (local_turn(match)) {
    const std::uint8_t selected = dice_in(local_player(context_of(match)), kDieSelected);
    if (selected != match.told_selection) {
      match.told_selection = selected;
      send(wire::DiceMove::Kind::Select, selected);
    }
  }
  // Autoplay (the kit): the local player's turn steps on like the other player's.
  if (autoplay && match.own_roll && now - match.last_step_at >= kStepMilliseconds) {
    if (void* state = local_turn(match)) {
      autoplay_step(match, state);
    } else if (void* state = state_of(match)) {
      void* kind = *static_cast<void**>(state);
      void* player = slot_of<StatePlayer>(state, kStatePlayer)(state);
      if (player == local_player(context_of(match)) && kind == vtables.badge_selecting) {
        autoplay_badge_choice(match, state);
      } else if (player == local_player(context_of(match)) && kind == vtables.check_bust) {
        // After a bust: thrown again the first time, banked after.
        match.last_step_at = now;
        const char choice = match.autoplay_bust_rethrown ? 0 : 1;
        match.autoplay_bust_rethrown = true;
        hooked_bust_choice(model_of(match), choice);
      }
    }
  }
  if (match.ran && natives.show_names &&
      (!match.own_name.empty() || !match.opponent_name.empty())) {
    natives.show_names(match.own_name, match.opponent_name);
  }
  if (match.ran && !slot_of<MinigameRuns>(match.minigame, kMinigameRuns)(match.minigame)) {
    // The game closed its minigame without its own end (the other player's body went, say): the
    // server's end is waited for a while, then the local player is said to have given up.
    if (!match.closed_at) {
      match.closed_at = now;
    }
    if (!match.ended && !match.gave_up_sent && now - match.closed_at >= kClosedGraceMilliseconds) {
      match.gave_up_sent = true;
      send(wire::DiceMove::Kind::GaveUp, 0);
    }
  }
}

std::vector<wire::DiceMove> take_moves() {
  return std::exchange(to_send, {});
}

void set_autoplay(bool on) {
  autoplay = on;
  log_line("DICE autoplay %s", on ? "on" : "off");
}

bool autoplay_on() {
  return autoplay;
}

namespace {
std::atomic<void*> seated_npc_skeleton{nullptr};
}  // namespace

void set_seated_npc(void* skeleton) {
  seated_npc_skeleton.store(skeleton);
}

bool seated_npc(void* skeleton) {
  return skeleton && skeleton == seated_npc_skeleton.load();
}

std::vector<std::uint32_t> own_table_things(std::uint32_t& match_id, std::uint32_t& cup,
                                            bool npc_opponent) {
  constexpr std::size_t kCup = 856;
  std::vector<std::uint32_t> things;
  if (!current || !current->minigame) {
    return things;
  }
  const bool own = own_state(*current);
  if (!own && !npc_opponent) {
    return things;
  }
  match_id = current->id;
  void* context = context_of(*current);
  things = board_dice(*current, own ? local_player(context) : opponent_player(context));
  cup = read<std::uint32_t>(current->minigame, kCup);
  return things;
}

}  // namespace kcds::runtime::dice
