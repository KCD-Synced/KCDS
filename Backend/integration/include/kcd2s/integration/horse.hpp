#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include "kcd2s/integration/chat.hpp"
#include "kcd2s/core/math.hpp"
#include "../../../local/animation.hpp"
namespace kcd2s::integration {
inline constexpr std::uint8_t kHorseMotionChannel = 8, kHorseMountChannel = 9;
enum HorseField : core::FieldId {
  HorsePosition = 0,
  HorseRotation = 1,
  // The player the server gave the horse (Horse.giveTo), 0 for nobody.
  HorseOwner = 5,
  HorseRider = 7,
  HorseSimulator = 8,
  HorseEpoch = 9,
  HorseNativeState = 10,
  // The components every client builds the body from, "<body>|<head>|<hair>|<tail>" (issue):
  // the server chooses them, so the horse looks the same everywhere.
  HorseLook = 11,
  // Bool: true = a lethal blow leaves the horse knocked out instead of dead; false is
  // the game's default, where the horse dies. The resource that spawned the horse chooses it.
  HorseKnockOut = 12,
  // The order a resource gave the horse (Horse.moveTo, patrol, hold), "" for none: the text that
  // npc.hpp NpcIntent describes, "<sequence>|<kind>|<arguments...>" with the kinds moveTo and
  // patrol. The simulator carries it out with the game's own movement; every replica only keeps it.
  HorseIntent = 13,
  // 0 to kHorseMaxHealth (float): what every body of the horse shows. The simulator
  // reports its native health (HORSE_HEALTH), a resource sets it (Horse.health); the server writes
  // it. A dead horse reads 0.
  HorseHealth = 14,
  // Int: kHorseAlive, kHorseKnockedOut or kHorseDead. Only the server writes it: a simulator's
  // report moves a living horse down (and a knocked-out one to dead), a resource moves it any way.
  HorseCondition = 15,
  // Int, one up at every wake and revive (a state of kHorseKnockedOut or kHorseDead back to
  // kHorseAlive): a body made for an earlier life is gone, so every client makes the horse's body
  // anew.
  HorseLife = 16,
  // Float, kg: the most the horse's saddlebags carry (Horse.maxCarryWeight); below 0
  // the game's own (its soul's inventory capacity).
  HorseMaxCarryWeight = 17,
  // Float: the horse's stamina (its soul's derived MaxStamina) and speed (as the inspect screen
  // shows it, 30 to 60; the game's horses about 35), a breed's or a resource's; below
  // 0 the game's own.
  HorseMaxStamina = 18,
  HorseSpeed = 19,
  // The tack the server chose for the horse (Horse.setTack), in the outfit field form
  // of appearance/outfit.hpp ("KO2" and per piece its class, quality and health, npc.hpp
  // valid_npc_fixed_outfit); "" leaves the body the game's own. Every body of the horse wears it.
  HorseGear = 20,
  // The status effects the server put on the horse (Horse.addBuff), in the form of
  // horse_buffs.hpp; "" for none. Every body of the horse carries them.
  HorseBuffs = 21,
  // The players besides its owner a resource lets ride the horse (Horse.allowRider): their
  // network ids as decimal text, comma-separated, at most kMaxHorseRiders; "" for none. Each of
  // them sees the game's plain mount instead of "mount and steal"; the saddlebags stay the owner's.
  HorseRiders = 22,
  // Int: the network id of the player or NPC leading the horse on a rope (Horse.follow with
  // rope: true), 0 for none. Every client draws the rope from the horse's head to
  // that body's right hand (riding/lead_rope.hpp).
  HorseLead = 23,
  // Float: how dirty the horse is, 0 clean to 1 (Horse.dirt, soil, clean), below 0
  // the game's own. Every body of the horse wears it on its coat and tack.
  HorseDirt = 24,
  // Float: the share of its pace the horse moves at in every gait (Horse.speedScale, 4.11
  // request 6), 0.1 to 2; below 0 the game's own.
  HorseSpeedScale = 25
};
inline constexpr std::size_t kMaxHorseRiders = 32;
inline constexpr float kHorseMaxHealth = 100;
// The values of HorseCondition (the field is no HorseState: that is the HORSE_STATE message).
inline constexpr std::int64_t kHorseAlive = 0, kHorseKnockedOut = 1, kHorseDead = 2;
// The body state a simulating client reports for a body it runs: a horse (HORSE_STATE below) or an
// NPC (NPC_STATE, npc.hpp). Both carry these fields with the same checks and bytes, each under a
// magic of its own and with the id of its body.
//   revision : the world-config revision of the sender (WorldConfig::revision). The server takes a
//              state only when it equals the revision it configured that client with, and a
//              replica uses a stored state only when it equals the revision its own client
//              requested, so nothing reported for an earlier world is replayed.
//   epoch    : the body's simulation epoch the sender held; the server refuses any other.
//   source   : the sender's own player (the body's simulator).
//   sequence : greater than every earlier state of this body from this simulator, from 1.
struct BodyState {
  std::uint32_t revision{}, epoch{};
  core::EntityId source{};
  std::uint64_t sequence{};
  core::Vec3 position{}, velocity{};
  core::Quat rotation{}, motion_rotation{};
  // motion_flags: 1 strafing allowed, 2 jumping, kBodyFighting the body fights (an NPC's).
  std::uint32_t motion_type{}, motion_flags{}, mask{};
  bool motion_valid{};
  std::array<float, 18> params{};
  std::string animations;
};
// The body is in a fight: its replicas follow its position and facing closely (an NPC's
// attack lagged for the other players).
inline constexpr std::uint32_t kBodyFighting = 4;
// The body holds the weapon it carries (an NPC's drawn for a fight): its replicas draw it too.
inline constexpr std::uint32_t kBodyArmed = 8;
// The simulator's body sits (an NPC at a dice table): its replicas switch their physics off.
inline constexpr std::uint32_t kBodySeated = 16;
// The simulator's body sits on that seat (issue 016: a bench, a chair; not a workbench it stands
// at): its replicas hold the game's sitting stance.
inline constexpr std::uint32_t kBodySitting = 32;
// HORSE_STATE: the rider's or simulator's client -> server on kHorseMotionChannel, magic 'KHS1'.
inline constexpr std::uint32_t kHorseStateMagic = 0x3153484b;
struct HorseState : BodyState {
  core::EntityId horse{};
};
struct HorseMount {
  std::uint32_t revision{}, epoch{};
  core::EntityId source{}, horse{};
  bool mount{};
};
inline bool horse_vec(core::Vec3 v, float bound) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::abs(v.x) <= bound &&
         std::abs(v.y) <= bound && std::abs(v.z) <= bound;
}
inline bool horse_quat(core::Quat q) {
  const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::isfinite(n) && n > .8f && n < 1.2f;
}
// Whether a body state carries values a game can take: ids and a sequence, finite and bounded
// vectors, quaternions near unit length, known motion values and decodable animations. `body` is
// the state's horse or NPC.
inline bool valid_body_state(const BodyState& e, core::EntityId body) {
  if (!e.source || !body || !e.sequence || !horse_vec(e.position, 1e6f) ||
      !horse_vec(e.velocity, 1000) || !horse_quat(e.rotation) || !horse_quat(e.motion_rotation) ||
      e.motion_type > 6 || e.motion_flags > 63 || (e.mask & ~0x1ffffu) ||
      !local::decode_animation(e.animations))
    return false;
  for (float f : e.params)
    if (!std::isfinite(f) || std::abs(f) > 1e6f)
      return false;
  return true;
}
inline bool valid_horse(const HorseState& e) {
  return valid_body_state(e, e.horse);
}
inline void horse_float(core::BitWriter& w, float f) {
  std::uint32_t x;
  std::memcpy(&x, &f, 4);
  w.write_bits(x, 32);
}
inline float horse_float(core::BitReader& r) {
  auto x = std::uint32_t(r.read_bits(32));
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}
inline void horse_vec(core::BitWriter& w, core::Vec3 v) {
  horse_float(w, v.x);
  horse_float(w, v.y);
  horse_float(w, v.z);
}
inline core::Vec3 horse_vec(core::BitReader& r) {
  core::Vec3 v;
  v.x = horse_float(r);
  v.y = horse_float(r);
  v.z = horse_float(r);
  return v;
}
inline void horse_quat(core::BitWriter& w, core::Quat q) {
  horse_float(w, q.x);
  horse_float(w, q.y);
  horse_float(w, q.z);
  horse_float(w, q.w);
}
inline core::Quat horse_quat(core::BitReader& r) {
  core::Quat q;
  q.x = horse_float(r);
  q.y = horse_float(r);
  q.z = horse_float(r);
  q.w = horse_float(r);
  return q;
}
// The bytes of a body state under `magic`, every number little-endian:
//   [u32 magic][u32 revision][u32 epoch][u32 source][u32 body][u64 sequence]
//   [f32 x3 position][f32 x4 rotation (x y z w)][f32 x3 velocity][f32 x4 motion rotation]
//   [u8 motion type][u8 motion flags][u8 motion valid 0/1][u32 mask][f32 x18 params]
//   [u16 n][n bytes animations, local/animation.hpp's form, n <= 1220]
inline std::vector<std::uint8_t> encode_body_state(std::uint32_t magic, const BodyState& e,
                                                   core::EntityId body) {
  core::BitWriter w;
  w.write_bits(magic, 32);
  w.write_bits(e.revision, 32);
  w.write_bits(e.epoch, 32);
  w.write_bits(e.source, 32);
  w.write_bits(body, 32);
  w.write_bits(e.sequence, 64);
  horse_vec(w, e.position);
  horse_quat(w, e.rotation);
  horse_vec(w, e.velocity);
  horse_quat(w, e.motion_rotation);
  w.write_bits(e.motion_type, 8);
  w.write_bits(e.motion_flags, 8);
  w.write_bits(e.motion_valid ? 1 : 0, 8);
  w.write_bits(e.mask, 32);
  for (auto f : e.params)
    horse_float(w, f);
  chat_string(w, e.animations);
  return w.bytes();
}
// False for other bytes than encode_body_state(magic, ...) writes, or values valid_body_state
// refuses. `body` receives the state's horse or NPC.
inline bool decode_body_state(std::uint32_t magic, const std::vector<std::uint8_t>& bytes,
                              BodyState& e, core::EntityId& body) {
  core::BitReader r(bytes);
  if (r.read_bits(32) != magic)
    return false;
  e.revision = std::uint32_t(r.read_bits(32));
  e.epoch = std::uint32_t(r.read_bits(32));
  e.source = std::uint32_t(r.read_bits(32));
  body = std::uint32_t(r.read_bits(32));
  e.sequence = r.read_bits(64);
  e.position = horse_vec(r);
  e.rotation = horse_quat(r);
  e.velocity = horse_vec(r);
  e.motion_rotation = horse_quat(r);
  e.motion_type = std::uint32_t(r.read_bits(8));
  e.motion_flags = std::uint32_t(r.read_bits(8));
  auto valid = r.read_bits(8);
  e.motion_valid = valid == 1;
  e.mask = std::uint32_t(r.read_bits(32));
  for (auto& f : e.params)
    f = horse_float(r);
  return valid <= 1 && chat_string(r, e.animations, 1220) && !r.overrun() && !r.bits_remaining() &&
         valid_body_state(e, body);
}
inline std::vector<std::uint8_t> encode_horse(const HorseState& e) {
  return encode_body_state(kHorseStateMagic, e, e.horse);
}
inline bool decode_horse(const std::vector<std::uint8_t>& bytes, HorseState& e) {
  return decode_body_state(kHorseStateMagic, bytes, e, e.horse);
}
inline std::vector<std::uint8_t> encode_horse_mount(const HorseMount& e) {
  core::BitWriter w;
  w.write_bits(0x314d484b, 32);
  w.write_bits(e.revision, 32);
  w.write_bits(e.epoch, 32);
  w.write_bits(e.source, 32);
  w.write_bits(e.horse, 32);
  w.write_bits(e.mount ? 1 : 0, 8);
  return w.bytes();
}
inline bool decode_horse_mount(const std::vector<std::uint8_t>& bytes, HorseMount& e) {
  core::BitReader r(bytes);
  if (r.read_bits(32) != 0x314d484b)
    return false;
  e.revision = std::uint32_t(r.read_bits(32));
  e.epoch = std::uint32_t(r.read_bits(32));
  e.source = std::uint32_t(r.read_bits(32));
  e.horse = std::uint32_t(r.read_bits(32));
  auto m = r.read_bits(8);
  e.mount = m == 1;
  return !r.overrun() && !r.bits_remaining() && m <= 1 && e.source && e.horse;
}

//   HORSE_HEALTH : the simulator's client -> server, on the control channel (0), when the native
//                  health or state of the horse it runs changed:
//                  [u8 kHorseHealthTag][u32 horse][u32 epoch][f32 health][u8 state], little-endian.
//                  `health` is the horse's own soul health, 0 to kHorseMaxHealth; `state` is
//                  kHorseAlive, kHorseKnockedOut or kHorseDead (what the game's death or knock-out
//                  of the horse made it). A living horse reports a health above 0. The server takes
//                  it while the world is ready, from the horse's current simulator, in the current
//                  epoch, at most kMaxHorseHealthsPerSecond per horse, and only moves a horse down:
//                  alive to knocked out or dead, knocked out to dead. A horse that is not alive
//                  never goes back to alive from a report (Horse.wake and Horse.revive do that),
//                  and a dead one stays dead. A death is taken as a knock-out when the horse's
//                  HorseKnockOut is set. The resource host hears each change as horseHealth.
inline constexpr std::uint8_t kHorseHealthTag = 68;
inline constexpr std::size_t kHorseHealthBytes = 14;
inline constexpr unsigned kMaxHorseHealthsPerSecond = 4;

struct HorseHealthReport {
  core::EntityId horse{};
  std::uint32_t epoch{};
  float health{};
  std::uint8_t state = std::uint8_t(kHorseAlive);

  friend bool operator==(const HorseHealthReport& a, const HorseHealthReport& b) {
    return a.horse == b.horse && a.epoch == b.epoch && a.health == b.health && a.state == b.state;
  }
};

// What taking a HORSE_HEALTH changed: the health and state the horse had, and the ones it has
// now, for the resource host (horseHealth).
struct HorseHealthChange {
  core::EntityId horse{};
  float previous_health{}, health{};
  std::int64_t previous_state{}, state{};
};

// What a horse's health and state can be together: a health from 0 to kHorseMaxHealth, a known
// state, and a living horse with some health.
inline bool valid_horse_health(float health, std::uint8_t state) {
  if (!(health >= 0.0f) || !(health <= kHorseMaxHealth) || state > kHorseDead) {
    return false;
  }
  return state != kHorseAlive || health > 0.0f;
}

namespace horse_detail {

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

}  // namespace horse_detail

// Empty without a horse or with a health and state that do not go together.
inline std::vector<std::uint8_t> encode_horse_health(const HorseHealthReport& report) {
  if (!report.horse || !valid_horse_health(report.health, report.state)) {
    return {};
  }
  std::uint32_t health_bits = 0;
  std::memcpy(&health_bits, &report.health, sizeof(health_bits));
  std::vector<std::uint8_t> out = {kHorseHealthTag};
  horse_detail::put_u32(out, report.horse);
  horse_detail::put_u32(out, report.epoch);
  horse_detail::put_u32(out, health_bits);
  out.push_back(report.state);
  return out;
}

inline bool decode_horse_health(const std::vector<std::uint8_t>& in, HorseHealthReport& report) {
  if (in.size() != kHorseHealthBytes || in[0] != kHorseHealthTag) {
    return false;
  }
  HorseHealthReport decoded;
  decoded.horse = horse_detail::get_u32(in, 1);
  decoded.epoch = horse_detail::get_u32(in, 5);
  const std::uint32_t health_bits = horse_detail::get_u32(in, 9);
  std::memcpy(&decoded.health, &health_bits, sizeof(health_bits));
  decoded.state = in[13];
  if (!decoded.horse || !valid_horse_health(decoded.health, decoded.state)) {
    return false;
  }
  report = decoded;
  return true;
}

//   HORSE_STAMINA : the simulator's client -> server, on the control channel (0), when the stamina
//                   of the horse it runs changed by at least kHorseStaminaReportStep (a server
//                   team's request: Horse.stamina):
//                   [u8 kHorseStaminaTag][u32 horse][u32 epoch][f32 stamina], little-endian.
//                   `stamina` is its soul's current stamina, 0 to kHorseMaxStaminaReport. The
//                   server takes it while the world is ready, from the horse's current simulator,
//                   in the current epoch, at most kMaxHorseStaminasPerSecond per horse; the
//                   resource host hears it as horseStamina.
inline constexpr std::uint8_t kHorseStaminaTag = 82;
inline constexpr std::size_t kHorseStaminaBytes = 13;
inline constexpr unsigned kMaxHorseStaminasPerSecond = 2;
inline constexpr float kHorseStaminaReportStep = 1.0f;
inline constexpr float kHorseMaxStaminaReport = 10000.0f;

struct HorseStaminaReport {
  core::EntityId horse{};
  std::uint32_t epoch{};
  float stamina{};

  friend bool operator==(const HorseStaminaReport& a, const HorseStaminaReport& b) {
    return a.horse == b.horse && a.epoch == b.epoch && a.stamina == b.stamina;
  }
};

// Empty without a horse or with a stamina out of its range.
inline std::vector<std::uint8_t> encode_horse_stamina(const HorseStaminaReport& report) {
  if (!report.horse || !(report.stamina >= 0.0f) || !(report.stamina <= kHorseMaxStaminaReport)) {
    return {};
  }
  std::uint32_t stamina_bits = 0;
  std::memcpy(&stamina_bits, &report.stamina, sizeof(stamina_bits));
  std::vector<std::uint8_t> out = {kHorseStaminaTag};
  horse_detail::put_u32(out, report.horse);
  horse_detail::put_u32(out, report.epoch);
  horse_detail::put_u32(out, stamina_bits);
  return out;
}

inline bool decode_horse_stamina(const std::vector<std::uint8_t>& in, HorseStaminaReport& report) {
  if (in.size() != kHorseStaminaBytes || in[0] != kHorseStaminaTag) {
    return false;
  }
  HorseStaminaReport decoded;
  decoded.horse = horse_detail::get_u32(in, 1);
  decoded.epoch = horse_detail::get_u32(in, 5);
  const std::uint32_t stamina_bits = horse_detail::get_u32(in, 9);
  std::memcpy(&decoded.stamina, &stamina_bits, sizeof(stamina_bits));
  if (!decoded.horse || !(decoded.stamina >= 0.0f) ||
      !(decoded.stamina <= kHorseMaxStaminaReport)) {
    return false;
  }
  report = decoded;
  return true;
}

// What taking a HORSE_STAMINA changed, for the resource host (horseStamina).
struct HorseStaminaChange {
  core::EntityId horse{};
  float stamina{};
};
}  // namespace kcd2s::integration
