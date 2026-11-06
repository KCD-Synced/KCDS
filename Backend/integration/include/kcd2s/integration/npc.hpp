#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/combat.hpp"
#include "kcd2s/integration/damage.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/horse.hpp"

// Synced NPCs (phase 1). A resource spawns an NPC with
// Npc.create; the server holds it as a NetworkNpc entity (local/movement.hpp, player_schema), with
// the fields of NpcField below, all written by the server only. One player's client simulates the
// body (the simulator: the nearest player near enough, see the election below) and reports its
// state; every other client draws the body from that state.
//
//   NPC_STATE : the simulator's client -> server, on kNpcMotionChannel (10, unreliable, so a late
//               datagram of one NPC never drops another NPC's newer state; the sequence orders
//               them). The body state of an NPC it simulates: exactly the fields, checks and bytes
//               of HORSE_STATE (horse.hpp, BodyState and encode_body_state), with the NPC's id in
//               the body slot and the magic 'KNS1' (0x31534e4b, bytes 4b 4e 53 31):
//                 [u32 magic][u32 revision][u32 epoch][u32 source][u32 npc][u64 sequence]
//                 [f32 x3 position][f32 x4 rotation (x y z w)][f32 x3 velocity]
//                 [f32 x4 motion rotation][u8 motion type][u8 motion flags][u8 motion valid 0/1]
//                 [u32 mask][f32 x18 params][u16 n][n bytes animations, n <= 1220]
//               every number little-endian. ClientPeer::send_npc_state fills revision and epoch
//               and sends it.
//
// The server takes an NPC_STATE only when all of these hold:
//   - the sender's world and native gameplay are ready for the configured world revision
//     (WorldConfig::revision, as for horses);
//   - `source` is the sender's own player, and that player is the NPC's current `simulator`;
//   - the sender holds the NPC (it acknowledged the NPC's construct);
//   - `epoch` is the NPC's current `simulationEpoch`;
//   - `sequence` is greater than that of every state the server took since the epoch began;
//   - the values pass valid_npc (the horse checks: finite, bounded, near-unit quaternions);
//   - fewer than kMaxNpcStatesPerSecond states of this NPC were taken in the last second.
// It then writes `position`, `rotation`, `velocity` and `nativeState` (the received bytes,
// unchanged), so every client and a late joiner hold the same state.
//
// A replica uses `nativeState` only when it decodes (decode_npc_state) and its epoch equals
// `simulationEpoch`, its source equals `simulator` and its revision equals the world-config
// revision the replica's own client requested; otherwise it follows `position` and `rotation`.
//
// Simulator election (Server::elect_npc_simulators, every kNpcElectionSeconds and when a client
// leaves): a player may simulate an NPC when its client is connected and its world and gameplay
// are ready for the current revision, it owns its player, it holds the NPC and the NPC is in its
// scope (dimension and private
// visibility, as interest; the global world mixes with every world). The simulator is the nearest
// such player within kNpcSimulatorRange metres (straight-line distance). The current simulator
// keeps the NPC unless it is no longer such a player, it is more than kNpcSimulatorKeepRange away,
// or another such player within kNpcSimulatorRange is at least kNpcSimulatorCloser metres nearer.
// With nobody it is 0 and the NPC stays where it was last reported. Every change of simulator, and
// every teleport, raises `simulationEpoch` by one and empties `nativeState`, so a state sent
// before it is refused and no replica replays it.
namespace kcd2s::integration {

inline constexpr std::uint8_t kNpcMotionChannel = 10;
inline constexpr std::uint32_t kNpcStateMagic = 0x31534e4b;

enum NpcField : core::FieldId {
  NpcPosition = 0,
  NpcRotation = 1,
  // The soul GUID text the body is spawned against, or "" for none.
  NpcSoul = 2,
  // "NPC" or "NPC_Female".
  NpcActorClass = 3,
  NpcName = 4,
  NpcScriptId = 5,
  NpcDimension = 6,
  // The player entity whose client simulates the NPC, 0 for none.
  NpcSimulator = 7,
  // One up at every change of simulator and at every teleport.
  NpcEpoch = 8,
  NpcVelocity = 9,
  // The simulator's last accepted NPC_STATE, its bytes unchanged; "" since the epoch began.
  NpcNativeState = 10,
  // One up at every Npc.teleport: the simulator then puts the body at position and rotation.
  NpcTeleport = 11,
  NpcFrozen = 12,
  // 0 to kNpcMaxHealth: what every body of the NPC shows. The bodies take no health of their own
  // games; NPC_HIT reports it and the resource host decides (Npc.damage, npcDamage).
  NpcHealth = 13,
  // False from the NPC's death to its revive: every body of it dies by the game's own lethal
  // damage, and lies.
  NpcAlive = 14,
  // What the simulator's body wears (the outfit field form of appearance/outfit.hpp, "KO2" and
  // per piece its class, quality and health), "" before the simulator reported it. The game
  // dresses every body of a soul anew, so the replicas wear this one (NPC_OUTFIT).
  NpcOutfit = 15,
  // The clip a resource plays on the NPC (Npc.playAnimation): "<sequence>|<loop 0/1>|<clip>", ""
  // for none. The simulator plays it on its body; every replica replays the simulator's clips.
  NpcAnimation = 16,
  // What the NPC says (Npc.say): "<sequence>|<milliseconds>|<text>", "" for nothing. Every client
  // shows the text in the game's speech bubble over the NPC's head for that long.
  NpcSpeech = 17,
  // The character components a resource chose for the NPC (Npc.setAppearance):
  // "<body>|<head>|<hair>|<beard>", an empty name the soul's own; "" before any choice. Every
  // client builds its body of the NPC from them.
  NpcAppearance = 18,
  // A resource chose what the NPC wears and carries (Npc.setOutfit): every body of it wears
  // `outfit`, the simulator's too, and the simulator's NPC_OUTFIT is no longer taken.
  NpcOutfitFixed = 19,
  // One up at every revive (alive false -> true): a body made for an earlier life is dead, so
  // every client makes the NPC's body anew.
  NpcLife = 20,
  // The order a resource gave the NPC (Npc.moveTo and the others), "" for none (what hold() also
  // writes): "<sequence>|<kind>|<arguments...>", numbers in plain decimal, the sequence up by one
  // per order. The kinds and their arguments: moveTo x|y|z|speed|radius|pathfinding (a patrol's
  // legs are moveTo orders too, see npcs/npcs.cjs); follow targetId|speed|radius; flee
  // x|y|z|speed|radius; lookAt entity|targetId or point|x|y|z; attack targetId|weapon. Speed is
  // walk, jog or run; pathfinding auto, game or server; weapon melee or bow. The simulator carries
  // the order out with the game's own movement; every replica only keeps it.
  NpcIntent = 21,
  // The share of the attacks coming at the NPC that it parries, 0 to 1
  // (Npc.setCombatSkill({ defense })); kNpcDefenseGame, the default, leaves every defence to the
  // game's own combat brain. The simulator's body follows it.
  NpcDefense = 22,
  // What the NPC's blows on a player take, as a share of what the struck player's game works out
  // (Npc.setCombatSkill({ damage })), 0 to kNpcDamageMax; kNpcDamageGame, the default, is the
  // game's own damage. The struck player's game applies it.
  NpcDamage = 23,
  // The NPC's nameplate (Npc.nameplate and its setters): "" for none, else the player
  // tag's field (nameplates.hpp) "1<health 0|1>;<AARRGGBB>;<text>", the text drawn instead of the
  // NPC's name ("" draws the name). Every client draws it over the body as a player's.
  NpcNameplate = 24,
  // How the simulator moves the body (Npc.locomotion): false for the game's own movement and walk
  // animations, true for the body put along its way without them.
  NpcKinematic = 25,
  // How the simulator's dead body lies (NPC_POSE): "" while the NPC lives and until the simulator
  // reported it, else the report's pose (kNpcPoseHeader bytes of its entity's position and
  // rotation, then its ragdoll's parts). Every replica lays its own dead body out the same way
  // (the body lay in another pose on every screen).
  NpcRagdollPose = 26,
  // Whether the game's own brain runs the NPC (Npc.brain): false keeps the
  // simulator's body from thinking, perceiving or fighting on its own; only the server's
  // orders move it.
  NpcBrain = 27,
  // Whether a player can lock on to the NPC (Npc.targetable): false takes its
  // body out of the game's lock-on on every client.
  NpcTargetable = 28,
  // A dead animal offers neither the game's Search nor its Gut: not lootable, and
  // nothing left to harvest.
  NpcCarcassClosed = 29,
  // Whether players can fight the NPC (Npc.attackable): false drops their blows, shots
  // and horses' hits on every client before the game reacts to them.
  NpcAttackable = 30,
  // The style the NPC's mouth moves in (Npc.setSpeaking), "" while it does not. Every
  // client loops the style's face clip on its body, as for a player (Player.setSpeaking).
  NpcTalking = 31,
  // The NPC's facial expression (Npc.setExpression): "<fragment>|<tags>", "" for
  // the game's own face. Its simulator holds it on the body; the clip sync shows it to the others.
  NpcFace = 32,
  // Whether players can talk to the NPC (Npc.interactable): false takes the game's own Talk prompt
  // and its Chat and Reply prompts off its body on every client.
  NpcInteractable = 33,
  // The level's own person this NPC is (issue 016, server.json levelPeople): its GUID as the
  // placement catalog writes it (16 hex digits), "" for an NPC of a script. Every game then uses
  // the level's own entity of that person as the NPC's body instead of making one.
  NpcLevelGuid = 34
};

// A level person's GUID: 16 lowercase hex digits, or "" for none.
inline bool valid_npc_level_guid(const std::string& guid) {
  if (guid.empty()) {
    return true;
  }
  if (guid.size() != 16) {
    return false;
  }
  for (const char c : guid) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// The longest NpcNameplate field (bytes): the flags and colour, and the text
// (kMaxNameplateTextBytes).
inline constexpr std::size_t kNpcNameplateMax = 96;

inline bool valid_npc_nameplate(const std::string& field) {
  if (field.empty()) {
    return true;
  }
  if (field.size() > kNpcNameplateMax || field.size() < 12 || field[0] != '1') {
    return false;
  }
  if ((field[1] != '0' && field[1] != '1') || field[2] != ';' || field[11] != ';') {
    return false;
  }
  return field.find('\n') == std::string::npos;
}

// NpcDefense: the game's own defences (no setCombatSkill).
inline constexpr float kNpcDefenseGame = -1.0f;
// NpcDamage: the game's own damage, and the largest share a resource may give.
inline constexpr float kNpcDamageGame = 1.0f;
inline constexpr float kNpcDamageMax = 10.0f;

// Npc.setOutfit: at most this many pieces (clothing and weapons), each as the outfit field
// carries it ("KO2", then per piece its 16 class bytes, its quality and its health).
inline constexpr std::size_t kNpcOutfitPiecesMax = 32;
inline constexpr std::size_t kNpcOutfitPieceBytes = 19;

inline bool valid_npc_fixed_outfit(const std::string& outfit) {
  return outfit.size() >= 3 && outfit.compare(0, 3, "KO2") == 0 &&
         (outfit.size() - 3) % kNpcOutfitPieceBytes == 0 &&
         (outfit.size() - 3) / kNpcOutfitPieceBytes <= kNpcOutfitPiecesMax;
}

// An NpcAppearance text: four component names (appearance.hpp's rules) between three '|'.
inline bool valid_npc_appearance(const std::string& appearance) {
  return appearance.empty() || valid_component_names(appearance, 4);
}

// The longest NpcAnimation text.
inline constexpr std::size_t kNpcAnimationMax = 300;
// The longest NpcIntent text: 2 KB, so the order fits one snapshot (a patrol takes at most 64
// waypoints).
inline constexpr std::size_t kNpcIntentMax = 2048;
// The longest NpcSpeech text (UTF-8 bytes, with its sequence and duration).
inline constexpr std::size_t kNpcSpeechMax = 600;

// No control characters: a speech is one line of UTF-8 text.
inline bool valid_npc_speech(const std::string& speech) {
  if (speech.size() > kNpcSpeechMax) {
    return false;
  }
  for (const char c : speech) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

inline bool valid_npc_animation(const std::string& animation) {
  if (animation.size() > kNpcAnimationMax) {
    return false;
  }
  for (const char c : animation) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

// An NpcIntent text: at most kNpcIntentMax bytes without control characters.
inline bool valid_npc_intent(const std::string& intent) {
  if (intent.size() > kNpcIntentMax) {
    return false;
  }
  for (const char c : intent) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

//   NPC_OUTFIT : the simulator's client -> server, on the control channel (0), when what its
//               body wears changed: [u8 kNpcOutfitTag][u32 npc][u32 epoch][u32 revision]
//               [the outfit bytes, the rest, at most kNpcOutfitMax], little-endian. The server
//               takes it on the conditions of an NPC_STATE (world ready and revision, the
//               sender's player the NPC's current simulator, the current epoch), at most
//               kMaxNpcOutfitsPerSecond per NPC, and writes `outfit`.
inline constexpr std::uint8_t kNpcOutfitTag = 60;
inline constexpr std::size_t kNpcOutfitMax = 2048;
inline constexpr unsigned kMaxNpcOutfitsPerSecond = 4;

struct NpcOutfitReport {
  core::EntityId npc{};
  std::uint32_t epoch{};
  std::uint32_t revision{};
  std::string outfit;

  friend bool operator==(const NpcOutfitReport& a, const NpcOutfitReport& b) {
    return a.npc == b.npc && a.epoch == b.epoch && a.revision == b.revision && a.outfit == b.outfit;
  }
};

namespace npc_detail {

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

inline std::uint32_t get_u32(const std::vector<std::uint8_t>& in, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= std::uint32_t(in[at + i]) << (8 * i);
  }
  return value;
}

}  // namespace npc_detail

// Empty when the report cannot travel: no NPC or an outfit longer than kNpcOutfitMax.
inline std::vector<std::uint8_t> encode_npc_outfit(const NpcOutfitReport& report) {
  if (!report.npc || report.outfit.size() > kNpcOutfitMax) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcOutfitTag};
  npc_detail::put_u32(out, report.npc);
  npc_detail::put_u32(out, report.epoch);
  npc_detail::put_u32(out, report.revision);
  out.insert(out.end(), report.outfit.begin(), report.outfit.end());
  return out;
}

inline bool decode_npc_outfit(const std::vector<std::uint8_t>& in, NpcOutfitReport& report) {
  constexpr std::size_t kHeader = 13;
  if (in.size() < kHeader || in[0] != kNpcOutfitTag || in.size() - kHeader > kNpcOutfitMax) {
    return false;
  }
  NpcOutfitReport decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.epoch = npc_detail::get_u32(in, 5);
  decoded.revision = npc_detail::get_u32(in, 9);
  decoded.outfit.assign(in.begin() + kHeader, in.end());
  if (!decoded.npc) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

//   NPC_LOOK : the simulator's client -> server, on the control channel (0), once its body is
//             built: the character components the game chose for it ("<body>|<head>|<hair>|
//             <beard>", as NpcAppearance). [u8 kNpcLookTag][u32 npc][u32 epoch][u32 revision]
//             [the look, the rest], little-endian. The game picks a runtime body's face itself on
//             every client, so without it every client showed another face. The server
//             takes it on the conditions of an NPC_OUTFIT and writes NpcAppearance only while it is
//             still "" (a resource's Npc.setAppearance, or the first simulator's look, stays).
inline constexpr std::uint8_t kNpcLookTag = 106;

struct NpcLookReport {
  core::EntityId npc{};
  std::uint32_t epoch{};
  std::uint32_t revision{};
  std::string look;

  friend bool operator==(const NpcLookReport& a, const NpcLookReport& b) {
    return a.npc == b.npc && a.epoch == b.epoch && a.revision == b.revision && a.look == b.look;
  }
};

// Empty when the report cannot travel: no NPC or a look that is not a valid NpcAppearance.
inline std::vector<std::uint8_t> encode_npc_look(const NpcLookReport& report) {
  if (!report.npc || report.look.empty() || !valid_npc_appearance(report.look)) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcLookTag};
  npc_detail::put_u32(out, report.npc);
  npc_detail::put_u32(out, report.epoch);
  npc_detail::put_u32(out, report.revision);
  out.insert(out.end(), report.look.begin(), report.look.end());
  return out;
}

inline bool decode_npc_look(const std::vector<std::uint8_t>& in, NpcLookReport& report) {
  constexpr std::size_t kHeader = 13;
  if (in.size() <= kHeader || in[0] != kNpcLookTag) {
    return false;
  }
  NpcLookReport decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.epoch = npc_detail::get_u32(in, 5);
  decoded.revision = npc_detail::get_u32(in, 9);
  decoded.look.assign(in.begin() + kHeader, in.end());
  if (!decoded.npc || !valid_npc_appearance(decoded.look)) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

//   NPC_POSE : the simulator's client -> server, on the control channel (0), once its dead body
//             lies still: [u8 kNpcPoseTag][u32 npc][u32 epoch][u32 revision][the pose, the rest],
//             little-endian; the pose is [f32 x3 position][f32 x4 rotation (x y z w)] of the body's
//             entity and then its ragdoll's parts (the client's own ragdoll pose form), at most
//             kNpcPoseMax bytes. The server takes it on the conditions of an NPC_OUTFIT, only while
//             the NPC is dead, at most kMaxNpcPosesPerSecond per NPC, and writes NpcRagdollPose.
inline constexpr std::uint8_t kNpcPoseTag = 152;
inline constexpr std::size_t kNpcPoseHeader = 28;
inline constexpr std::size_t kNpcPoseMax = 4096;
inline constexpr unsigned kMaxNpcPosesPerSecond = 2;

struct NpcPoseReport {
  core::EntityId npc{};
  std::uint32_t epoch{};
  std::uint32_t revision{};
  std::string pose;

  friend bool operator==(const NpcPoseReport& a, const NpcPoseReport& b) {
    return a.npc == b.npc && a.epoch == b.epoch && a.revision == b.revision && a.pose == b.pose;
  }
};

// Whether a pose can be laid out: the entity's position and rotation, finite and bounded, and at
// least one part, at most kNpcPoseMax bytes.
inline bool valid_npc_pose(const std::string& pose) {
  if (pose.size() <= kNpcPoseHeader || pose.size() > kNpcPoseMax) {
    return false;
  }
  for (std::size_t at = 0; at < kNpcPoseHeader; at += 4) {
    float value = 0;
    std::memcpy(&value, pose.data() + at, sizeof(value));
    if (!std::isfinite(value) || std::abs(value) > 1e6f) {
      return false;
    }
  }
  return true;
}

// Empty when the report cannot travel: no NPC or a pose valid_npc_pose refuses.
inline std::vector<std::uint8_t> encode_npc_pose(const NpcPoseReport& report) {
  if (!report.npc || !valid_npc_pose(report.pose)) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcPoseTag};
  npc_detail::put_u32(out, report.npc);
  npc_detail::put_u32(out, report.epoch);
  npc_detail::put_u32(out, report.revision);
  out.insert(out.end(), report.pose.begin(), report.pose.end());
  return out;
}

inline bool decode_npc_pose(const std::vector<std::uint8_t>& in, NpcPoseReport& report) {
  constexpr std::size_t kHeader = 13;
  if (in.size() <= kHeader || in[0] != kNpcPoseTag) {
    return false;
  }
  NpcPoseReport decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.epoch = npc_detail::get_u32(in, 5);
  decoded.revision = npc_detail::get_u32(in, 9);
  decoded.pose.assign(in.begin() + kHeader, in.end());
  if (!decoded.npc || !valid_npc_pose(decoded.pose)) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

//   NPC_HIT : a client -> server, on the control channel (0), for health its game would have taken
//             off a body of an NPC. No body takes it: every body shows the server's
//             `health`, so the blow lands (its reaction plays) without the health.
//               [u8 kNpcHitTag][u32 npc][u32 revision][u32 attacker][f32 amount bits][u8 reason]
//               [i32 subpart], little-endian.
//             `attacker` is the sender's own player when its player dealt the blow (the game of the
//             one who struck prices it), or, from the NPC's simulator only, an NPC or 0 for none
//             (a fall, a fire, an animal). `reason` is the game's E_DamageReason (damage.hpp) and
//             `subpart` the body subpart the blow landed on, -1 for none. The server takes it when
//             the sender's world is ready at that revision, the sender holds the NPC, `amount` is
//             above 0 and at most kMaxDamage, a player attacker is within kNpcHitRange of the NPC,
//             and the sender sent fewer than kMaxNpcHitsPerSecond in the last second; the
//             resource host hears it as npcHit and decides what the NPC loses.
inline constexpr std::uint8_t kNpcHitTag = 64;
inline constexpr std::size_t kNpcHitBytes = 22;
inline constexpr float kNpcHitRange = 200;
inline constexpr unsigned kMaxNpcHitsPerSecond = 20;

struct NpcHit {
  core::EntityId npc{};
  std::uint32_t revision{};
  core::EntityId attacker{};
  float amount{};
  std::uint8_t reason{};
  std::int32_t subpart = -1;

  friend bool operator==(const NpcHit& a, const NpcHit& b) {
    return a.npc == b.npc && a.revision == b.revision && a.attacker == b.attacker &&
           a.amount == b.amount && a.reason == b.reason && a.subpart == b.subpart;
  }
};

// Empty when the hit cannot travel: no NPC, an amount out of range or an unknown reason.
inline std::vector<std::uint8_t> encode_npc_hit(const NpcHit& hit) {
  if (!hit.npc || !(hit.amount > 0.0f) || !(hit.amount <= kMaxDamage) ||
      hit.reason >= kDamageReasons || hit.subpart < -1) {
    return {};
  }
  std::uint32_t amount_bits = 0;
  std::memcpy(&amount_bits, &hit.amount, sizeof(amount_bits));
  std::vector<std::uint8_t> out = {kNpcHitTag};
  npc_detail::put_u32(out, hit.npc);
  npc_detail::put_u32(out, hit.revision);
  npc_detail::put_u32(out, hit.attacker);
  npc_detail::put_u32(out, amount_bits);
  out.push_back(hit.reason);
  npc_detail::put_u32(out, std::uint32_t(hit.subpart));
  return out;
}

inline bool decode_npc_hit(const std::vector<std::uint8_t>& in, NpcHit& hit) {
  if (in.size() != kNpcHitBytes || in[0] != kNpcHitTag) {
    return false;
  }
  NpcHit decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.revision = npc_detail::get_u32(in, 5);
  decoded.attacker = npc_detail::get_u32(in, 9);
  const std::uint32_t amount_bits = npc_detail::get_u32(in, 13);
  std::memcpy(&decoded.amount, &amount_bits, sizeof(amount_bits));
  decoded.reason = in[17];
  decoded.subpart = std::int32_t(npc_detail::get_u32(in, 18));
  if (!decoded.npc || !(decoded.amount > 0.0f) || !(decoded.amount <= kMaxDamage) ||
      decoded.reason >= kDamageReasons || decoded.subpart < -1) {
    return false;
  }
  hit = decoded;
  return true;
}

//   NPC_BLOW : a client -> server -> another client, on the control channel (0), for a fight
//              between an NPC and a player: a blow one game priced, played by the
//              game on the other side of it.
//                [u8 kNpcBlowTag][u32 npc][u32 revision][u32 player][u8 direction]
//                [kNpcBlowHitBytes: the game's HitInfo, portable as combat.hpp's reaction data]
//              little-endian. PlayerStruckNpc comes from the striking player's own client (whose
//              NPC body is a replica) and goes to the NPC's simulator: its game plays the blow on
//              the NPC's body (its hit reaction, and the AI learns who struck it; the health is
//              the server's, NPC_HIT). NpcStruckPlayer comes from the NPC's simulator and goes to
//              the struck player's own client: its game takes the blow (health, reaction,
//              playerDamage). The server takes it when the sender's world is ready at that
//              revision, the sender holds the NPC and it is alive, the player is the sender's
//              own (PlayerStruckNpc) or another player within kNpcHitRange while the sender
//              simulates the NPC (NpcStruckPlayer), the HitInfo passes valid_reaction_data, and
//              the sender sent fewer than kMaxNpcBlowsPerSecond in the last second. It passes the
//              bytes on with the recipient's revision.
inline constexpr std::uint8_t kNpcBlowTag = 65;
inline constexpr std::size_t kNpcBlowHitBytes = 160;
inline constexpr std::size_t kNpcBlowBytes = 14 + kNpcBlowHitBytes;
inline constexpr unsigned kMaxNpcBlowsPerSecond = 10;

enum class NpcBlowDirection : std::uint8_t {
  PlayerStruckNpc = 0,
  NpcStruckPlayer = 1,
};

struct NpcBlow {
  core::EntityId npc{};
  std::uint32_t revision{};
  core::EntityId player{};
  NpcBlowDirection direction = NpcBlowDirection::PlayerStruckNpc;
  std::array<std::uint8_t, kNpcBlowHitBytes> hit{};

  friend bool operator==(const NpcBlow& a, const NpcBlow& b) {
    return a.npc == b.npc && a.revision == b.revision && a.player == b.player &&
           a.direction == b.direction && a.hit == b.hit;
  }
};

// The HitInfo with its process-local fields cleared (combat.hpp portable_reaction_data).
inline std::array<std::uint8_t, kNpcBlowHitBytes> portable_npc_hit(const std::uint8_t* hit) {
  NativeReactionData data{};
  std::memcpy(data.data(), hit, kNpcBlowHitBytes);
  data = portable_reaction_data(data);
  std::array<std::uint8_t, kNpcBlowHitBytes> portable{};
  std::memcpy(portable.data(), data.data(), kNpcBlowHitBytes);
  return portable;
}

inline bool valid_npc_hit_bytes(const std::array<std::uint8_t, kNpcBlowHitBytes>& hit) {
  NativeReactionData data{};
  std::memcpy(data.data(), hit.data(), kNpcBlowHitBytes);
  return valid_reaction_data(data);
}

// Empty when the blow cannot travel: no NPC or player, or a HitInfo that is not portable.
inline std::vector<std::uint8_t> encode_npc_blow(const NpcBlow& blow) {
  if (!blow.npc || !blow.player || !valid_npc_hit_bytes(blow.hit)) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcBlowTag};
  npc_detail::put_u32(out, blow.npc);
  npc_detail::put_u32(out, blow.revision);
  npc_detail::put_u32(out, blow.player);
  out.push_back(std::uint8_t(blow.direction));
  out.insert(out.end(), blow.hit.begin(), blow.hit.end());
  return out;
}

inline bool decode_npc_blow(const std::vector<std::uint8_t>& in, NpcBlow& blow) {
  if (in.size() != kNpcBlowBytes || in[0] != kNpcBlowTag || in[13] > 1) {
    return false;
  }
  NpcBlow decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.revision = npc_detail::get_u32(in, 5);
  decoded.player = npc_detail::get_u32(in, 9);
  decoded.direction = NpcBlowDirection(in[13]);
  std::copy(in.begin() + 14, in.end(), decoded.hit.begin());
  if (!decoded.npc || !decoded.player || !valid_npc_hit_bytes(decoded.hit)) {
    return false;
  }
  blow = decoded;
  return true;
}

//   NPC_ENGAGE : a client -> server, on the control channel (0), when its player asks to lock on
//                to an NPC its client does not simulate: [u8 kNpcEngageTag][u32 npc]
//                [u32 revision], little-endian. A replica runs no AI, so the game does not offer
//                it as a target; the server makes the player the NPC's fighter
//                (kNpcFightSeconds), the NPC goes to that player's client, and there the game's
//                own lock-on and fight take over. The server takes it when the sender's world is
//                ready at that revision, it holds the NPC and the NPC is alive, its player is
//                within kNpcEngageRange of the NPC, and it sent fewer than
//                kMaxNpcEngagesPerSecond in the last second.
inline constexpr std::uint8_t kNpcEngageTag = 66;
inline constexpr std::size_t kNpcEngageBytes = 9;
inline constexpr float kNpcEngageRange = 6;
inline constexpr unsigned kMaxNpcEngagesPerSecond = 4;

struct NpcEngage {
  core::EntityId npc{};
  std::uint32_t revision{};

  friend bool operator==(const NpcEngage& a, const NpcEngage& b) {
    return a.npc == b.npc && a.revision == b.revision;
  }
};

// Empty without an NPC.
inline std::vector<std::uint8_t> encode_npc_engage(const NpcEngage& engage) {
  if (!engage.npc) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcEngageTag};
  npc_detail::put_u32(out, engage.npc);
  npc_detail::put_u32(out, engage.revision);
  return out;
}

inline bool decode_npc_engage(const std::vector<std::uint8_t>& in, NpcEngage& engage) {
  if (in.size() != kNpcEngageBytes || in[0] != kNpcEngageTag) {
    return false;
  }
  NpcEngage decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.revision = npc_detail::get_u32(in, 5);
  if (!decoded.npc) {
    return false;
  }
  engage = decoded;
  return true;
}

//   NPC_INTENT_DONE : the simulator's client -> server, on the control channel (0), when the
//                     order the NPC walks (its NpcIntent, "<sequence>|moveTo|...") ended:
//                     [u8 kNpcIntentDoneTag][u32 npc][u32 epoch][u32 sequence][u8 status],
//                     little-endian. `sequence` is the order's own number, the leading number of
//                     the intent text, so a report of an order that was replaced since is
//                     dropped by the resource host. `status` is how it ended: kNpcIntentReached,
//                     kNpcIntentBlocked (the walk found no way on) or kNpcIntentFailed. The server
//                     takes it on the conditions of an NPC_STATE (world ready, the sender's player
//                     the NPC's current simulator, the current epoch) while the NPC is alive, at
//                     most kMaxNpcIntentDonesPerSecond per NPC; the resource host hears it as
//                     npcIntentDone (patrols step on from it).
inline constexpr std::uint8_t kNpcIntentDoneTag = 67;
inline constexpr std::size_t kNpcIntentDoneBytes = 14;
inline constexpr unsigned kMaxNpcIntentDonesPerSecond = 4;
inline constexpr std::uint8_t kNpcIntentReached = 0;
inline constexpr std::uint8_t kNpcIntentBlocked = 1;
inline constexpr std::uint8_t kNpcIntentFailed = 2;

struct NpcIntentDone {
  core::EntityId npc{};
  std::uint32_t epoch{};
  std::uint32_t sequence{};
  std::uint8_t status = kNpcIntentReached;

  friend bool operator==(const NpcIntentDone& a, const NpcIntentDone& b) {
    return a.npc == b.npc && a.epoch == b.epoch && a.sequence == b.sequence && a.status == b.status;
  }
};

// Empty without an NPC or with a status other than the three above.
inline std::vector<std::uint8_t> encode_npc_intent_done(const NpcIntentDone& done) {
  if (!done.npc || done.status > kNpcIntentFailed) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcIntentDoneTag};
  npc_detail::put_u32(out, done.npc);
  npc_detail::put_u32(out, done.epoch);
  npc_detail::put_u32(out, done.sequence);
  out.push_back(done.status);
  return out;
}

inline bool decode_npc_intent_done(const std::vector<std::uint8_t>& in, NpcIntentDone& done) {
  if (in.size() != kNpcIntentDoneBytes || in[0] != kNpcIntentDoneTag || in[13] > kNpcIntentFailed) {
    return false;
  }
  NpcIntentDone decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.epoch = npc_detail::get_u32(in, 5);
  decoded.sequence = npc_detail::get_u32(in, 9);
  decoded.status = in[13];
  if (!decoded.npc) {
    return false;
  }
  done = decoded;
  return true;
}

//   NPC_ANIMATION_END : the simulator's client -> server, on the control channel (0), when the
//                       clip a resource plays on the NPC (its NpcAnimation, "<sequence>|<loop>|
//                       <clip>") is over without the resource ending it:
//                       [u8 kNpcAnimationEndTag][u32 npc][u32 epoch][u32 sequence][u8 reason],
//                       little-endian. `reason` is kNpcAnimationFinished (a one-shot reached its
//                       end), kNpcAnimationInterrupted (something else took the body's clip: a
//                       hit, a fall) or kNpcAnimationRefused (the body has no such clip). Taken on
//                       the conditions of an NPC_INTENT_DONE, at most kMaxNpcAnimationEndsPerSecond
//                       per NPC; the resource host raises npcAnimationEnd for the request whose
//                       sequence it names.
inline constexpr std::uint8_t kNpcAnimationEndTag = 147;
inline constexpr std::size_t kNpcAnimationEndBytes = 14;
inline constexpr unsigned kMaxNpcAnimationEndsPerSecond = 4;
inline constexpr std::uint8_t kNpcAnimationFinished = 0;
inline constexpr std::uint8_t kNpcAnimationInterrupted = 1;
inline constexpr std::uint8_t kNpcAnimationRefused = 2;

struct NpcAnimationEnd {
  core::EntityId npc{};
  std::uint32_t epoch{};
  std::uint32_t sequence{};
  std::uint8_t reason = kNpcAnimationFinished;

  friend bool operator==(const NpcAnimationEnd& a, const NpcAnimationEnd& b) {
    return a.npc == b.npc && a.epoch == b.epoch && a.sequence == b.sequence && a.reason == b.reason;
  }
};

// Empty without an NPC or with a reason other than the three above.
inline std::vector<std::uint8_t> encode_npc_animation_end(const NpcAnimationEnd& end) {
  if (!end.npc || end.reason > kNpcAnimationRefused) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcAnimationEndTag};
  npc_detail::put_u32(out, end.npc);
  npc_detail::put_u32(out, end.epoch);
  npc_detail::put_u32(out, end.sequence);
  out.push_back(end.reason);
  return out;
}

inline bool decode_npc_animation_end(const std::vector<std::uint8_t>& in, NpcAnimationEnd& end) {
  if (in.size() != kNpcAnimationEndBytes || in[0] != kNpcAnimationEndTag ||
      in[13] > kNpcAnimationRefused) {
    return false;
  }
  NpcAnimationEnd decoded;
  decoded.npc = npc_detail::get_u32(in, 1);
  decoded.epoch = npc_detail::get_u32(in, 5);
  decoded.sequence = npc_detail::get_u32(in, 9);
  decoded.reason = in[13];
  if (!decoded.npc) {
    return false;
  }
  end = decoded;
  return true;
}

//   NPC_USE : a client -> server, on the control channel (0), when its player pressed the use key
//             looking at a server NPC's body: [u8 kNpcUseTag][u32 npc],
//             little-endian. The server takes it from a client whose world is ready and which
//             holds the NPC, at most kMaxNpcUsesPerSecond a second, and the resource host raises
//             npcInteract(npc, player) after its own range and `interactable` checks.
inline constexpr std::uint8_t kNpcUseTag = 148;
inline constexpr std::size_t kNpcUseBytes = 5;
inline constexpr unsigned kMaxNpcUsesPerSecond = 4;

struct NpcUse {
  core::EntityId npc{};
  core::EntityId player{};  // the server's: the sender's player
};

inline std::vector<std::uint8_t> encode_npc_use(core::EntityId npc) {
  if (!npc) {
    return {};
  }
  std::vector<std::uint8_t> out = {kNpcUseTag};
  npc_detail::put_u32(out, npc);
  return out;
}

inline bool decode_npc_use(const std::vector<std::uint8_t>& in, core::EntityId& npc) {
  if (in.size() != kNpcUseBytes || in[0] != kNpcUseTag) {
    return false;
  }
  npc = npc_detail::get_u32(in, 1);
  return npc != 0;
}

inline constexpr float kNpcMaxHealth = 100;
// The longest NPC name in UTF-8 bytes.
inline constexpr std::size_t kNpcNameMax = 128;
// The simulator election, in metres and seconds.
// A fight: a player whose blow on the NPC, or whom the NPC's blow struck, the server
// took (NPC_HIT, NPC_BLOW) is the NPC's fighter for this long after the last such blow. The
// election gives the NPC to its fighter's client while it may simulate it within
// kNpcSimulatorKeepRange, so that player's own game runs the whole fight (the NPC's attacks, the
// player's blocks and parries, the reactions and the damage) natively. Another player's blows do
// not take the fight over while the fighter's lasts.
inline constexpr double kNpcFightSeconds = 10;
// The witnesses of a fight: for kNpcWitnessSeconds after a player and an NPC fought
// (a blow either way, a player's hit), every other NPC within kNpcWitnessRadius of the fought NPC
// goes to the fighting player's client too, while it may simulate it within
// kNpcSimulatorKeepRange. The game's reactions to a fight (a scream, running away, calling the
// guards, the guards coming) come from the people who see it in that game; a person another game
// ran never heard of the fight. A pin and a fight of its own still go first.
inline constexpr double kNpcWitnessSeconds = 45;
inline constexpr float kNpcWitnessRadius = 30;
inline constexpr float kNpcSimulatorRange = 150;
inline constexpr float kNpcSimulatorKeepRange = 170;
inline constexpr float kNpcSimulatorCloser = 20;
inline constexpr double kNpcElectionSeconds = 0.2;
// NPC_STATEs the server takes per NPC per second; the rest are dropped.
inline constexpr unsigned kMaxNpcStatesPerSecond = 30;

struct NpcState : BodyState {
  core::EntityId npc{};
  // What the simulator's body holds in its hands (issue 016: a housemaid's broom, a board, a
  // spoon its brain gave it), as an animation props field (animation_props.hpp: each hand's item
  // class on that hand's slot), "" for empty hands. Every replica hangs the same items, and as
  // it travels in every state the server keeps for the NPC, a player who joins late sees them.
  std::string hands;
};

// The longest hands text: two props, each an item class on a hand slot or a mesh's path with its
// place on the hand joint.
inline constexpr std::size_t kNpcHandsMax = 480;

inline bool valid_npc_hands(const std::string& hands) {
  if (hands.size() > kNpcHandsMax) {
    return false;
  }
  for (const char c : hands) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

inline bool valid_npc(const NpcState& e) {
  return valid_body_state(e, e.npc) && valid_npc_hands(e.hands);
}

// NPC_STATE: the body state, then the hands text and its length in two bytes (little endian) at
// the very end, so the body state is read as it always was.
inline std::vector<std::uint8_t> encode_npc_state(const NpcState& e) {
  if (!valid_npc_hands(e.hands)) {
    return {};
  }
  std::vector<std::uint8_t> bytes = encode_body_state(kNpcStateMagic, e, e.npc);
  if (bytes.empty()) {
    return bytes;
  }
  bytes.insert(bytes.end(), e.hands.begin(), e.hands.end());
  bytes.push_back(std::uint8_t(e.hands.size() & 0xFF));
  bytes.push_back(std::uint8_t(e.hands.size() >> 8));
  return bytes;
}

inline bool decode_npc_state(const std::vector<std::uint8_t>& bytes, NpcState& e) {
  if (bytes.size() < 2) {
    return false;
  }
  const std::size_t length =
      std::size_t(bytes[bytes.size() - 2]) | (std::size_t(bytes[bytes.size() - 1]) << 8);
  if (length > kNpcHandsMax || length + 2 > bytes.size()) {
    return false;
  }
  const std::size_t body_end = bytes.size() - 2 - length;
  const std::vector<std::uint8_t> body(bytes.begin(), bytes.begin() + std::ptrdiff_t(body_end));
  NpcState decoded;
  if (!decode_body_state(kNpcStateMagic, body, decoded, decoded.npc)) {
    return false;
  }
  decoded.hands.assign(bytes.begin() + std::ptrdiff_t(body_end), bytes.end() - 2);
  if (!valid_npc_hands(decoded.hands)) {
    return false;
  }
  e = std::move(decoded);
  return true;
}

// What a resource spawns an NPC with (Npc.create).
struct NpcSpawn {
  core::Vec3 position{};
  core::Quat rotation{};
  std::string soul;
  std::string actor_class = "NPC";
  std::string name;
  std::uint32_t dimension = 0xffffffffu;
  bool frozen = false;
};

// A soul is a GUID text (8-4-4-4-12 hex digits, either case) or "" for none.
inline bool valid_npc_soul(const std::string& soul) {
  if (soul.empty()) {
    return true;
  }
  Guid parsed{};
  return parse_guid(soul, parsed);
}

// The game's animal classes the server spawns (the resource host's ANIMAL_CLASSES).
// An animal's body is its own class on every client, its simulator's running its AI, the others
// replaying it (native_npcs.inl), as for horses.
inline constexpr std::array<const char*, 15> kNpcAnimalClasses = {
    "Boar",        "CattleBull", "CattleCow",  "Dog",         "Hare",
    "Pig",         "Raven",      "RedDeerDoe", "RedDeerStag", "RoeDeerBuck",
    "RoeDeerHind", "SheepEwe",   "SheepRam",   "WildDog",     "Wolf",
};

inline bool npc_animal_class(const std::string& actor_class) {
  return std::find(kNpcAnimalClasses.begin(), kNpcAnimalClasses.end(), actor_class) !=
         kNpcAnimalClasses.end();
}

inline bool valid_npc_class(const std::string& actor_class) {
  return actor_class == "NPC" || actor_class == "NPC_Female" || npc_animal_class(actor_class);
}

// At most kNpcNameMax bytes of UTF-8 without control characters.
inline bool valid_npc_name(const std::string& name) {
  if (name.size() > kNpcNameMax) {
    return false;
  }
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

inline bool valid_npc_spawn(const NpcSpawn& spawn) {
  return horse_vec(spawn.position, 1e6f) && horse_quat(spawn.rotation) &&
         valid_npc_soul(spawn.soul) && valid_npc_class(spawn.actor_class) &&
         valid_npc_name(spawn.name);
}

// A change of an NPC's simulator, for the resource host (npcSimulatorChange).
struct NpcSimulatorChange {
  core::EntityId npc{};
  core::EntityId simulator{};  // 0 for none
  core::EntityId previous{};   // 0 for none
  std::int64_t epoch{};

  friend bool operator==(const NpcSimulatorChange& a, const NpcSimulatorChange& b) {
    return a.npc == b.npc && a.simulator == b.simulator && a.previous == b.previous &&
           a.epoch == b.epoch;
  }
};

}  // namespace kcd2s::integration
