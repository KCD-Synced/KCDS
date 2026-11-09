// Control-channel (channel 0, reliable-ordered) messages.
//   SET_OWNER   : server tells a client it owns entity E of type T at forced-state epoch EP.
//   FORCE_STATE : server bumped E's epoch to EP; the owner must adopt it and echo it upstream, so
//                 the server can drop owner input that still carries the stale (pre-force) epoch.
//   FORCE_XFORM : server authoritatively teleports E to a world position at epoch EP (§7 native
//                 force). Entity ids are never reused within a server session, so a command
//                 replayed after a reconnect names the old entity and can never match its
//                 replacement; non-finite coordinates are rejected at decode.
//   FORCE_ROT   : server authoritatively rotates E to a unit quaternion at epoch EP, with the same
//                 fencing as FORCE_XFORM; a non-finite or non-unit quaternion is rejected at
//                 decode.
#ifndef KCD2S_INTEGRATION_CONTROL_HPP
#define KCD2S_INTEGRATION_CONTROL_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"

namespace kcd2s::integration {

// Control-channel (channel 0) leading-byte namespace. 3 and 4 are already taken by WORLD_CONFIG and
// WORLD_READY (see world_config.hpp), so FORCE_XFORM must not reuse them -- a collision would make
// the client decode a teleport as a world config and silently drop it.
enum ControlKind : std::uint8_t {
  kCtrlSetOwner = 1,
  kCtrlForceState = 2,
  kCtrlForceTransform = 5,
  kCtrlForceRotation = 6
};

namespace detail {
inline void write_control_f32(core::BitWriter& w, float f) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &f, 4);
  w.write_bits(bits, 32);
}
inline float read_control_f32(core::BitReader& r) {
  const std::uint32_t bits = static_cast<std::uint32_t>(r.read_bits(32));
  float f = 0.0f;
  std::memcpy(&f, &bits, 4);
  return f;
}
}  // namespace detail

inline std::uint8_t control_kind(const std::vector<std::uint8_t>& p) {
  return p.empty() ? 0 : p[0];
}

inline std::vector<std::uint8_t> encode_set_owner(core::EntityId e, core::TypeId type,
                                                  std::uint8_t epoch) {
  core::BitWriter w;
  w.write_bits(kCtrlSetOwner, 8);
  w.write_varuint(e);
  w.write_bits(type, 32);
  w.write_bits(epoch, 8);
  return w.bytes();
}

inline bool decode_set_owner(const std::vector<std::uint8_t>& payload, core::EntityId& out_e,
                             core::TypeId& out_type, std::uint8_t& out_epoch) {
  core::BitReader r(payload);
  if (r.read_bits(8) != kCtrlSetOwner)
    return false;
  const std::uint64_t e = r.read_varuint();
  const std::uint64_t type = r.read_bits(32);
  const std::uint64_t epoch = r.read_bits(8);
  if (r.overrun())
    return false;
  out_e = static_cast<core::EntityId>(e);
  out_type = static_cast<core::TypeId>(type);
  out_epoch = static_cast<std::uint8_t>(epoch);
  return true;
}

inline std::vector<std::uint8_t> encode_force_state(core::EntityId e, std::uint8_t epoch) {
  core::BitWriter w;
  w.write_bits(kCtrlForceState, 8);
  w.write_varuint(e);
  w.write_bits(epoch, 8);
  return w.bytes();
}

inline bool decode_force_state(const std::vector<std::uint8_t>& payload, core::EntityId& out_e,
                               std::uint8_t& out_epoch) {
  core::BitReader r(payload);
  if (r.read_bits(8) != kCtrlForceState)
    return false;
  const std::uint64_t e = r.read_varuint();
  const std::uint64_t epoch = r.read_bits(8);
  if (r.overrun())
    return false;
  out_e = static_cast<core::EntityId>(e);
  out_epoch = static_cast<std::uint8_t>(epoch);
  return true;
}

// A FORCE_XFORM may end with a flags byte: kForceCorrection marks the server putting the owner's
// own player back where its last accepted step ended (the movement check), which the owner may
// blend in when it is small instead of snapping. A command without the byte is a plain teleport.
inline constexpr std::uint8_t kForceCorrection = 1;

inline std::vector<std::uint8_t> encode_force_transform(core::EntityId e, std::uint8_t epoch,
                                                        const core::Vec3& pos,
                                                        bool correction = false) {
  core::BitWriter w;
  w.write_bits(kCtrlForceTransform, 8);
  w.write_varuint(e);
  w.write_bits(epoch, 8);
  detail::write_control_f32(w, pos.x);
  detail::write_control_f32(w, pos.y);
  detail::write_control_f32(w, pos.z);
  if (correction) {
    w.write_bits(kForceCorrection, 8);
  }
  return w.bytes();
}

// Returns false on a malformed/truncated payload OR non-finite coordinates: a teleport to NaN/Inf
// is never a legitimate authoritative command and must not reach the engine transform path.
inline bool decode_force_transform(const std::vector<std::uint8_t>& payload, core::EntityId& out_e,
                                   std::uint8_t& out_epoch, core::Vec3& out_pos,
                                   bool* out_correction = nullptr) {
  core::BitReader r(payload);
  if (r.read_bits(8) != kCtrlForceTransform)
    return false;
  const std::uint64_t e = r.read_varuint();
  const std::uint64_t epoch = r.read_bits(8);
  core::Vec3 pos;
  pos.x = detail::read_control_f32(r);
  pos.y = detail::read_control_f32(r);
  pos.z = detail::read_control_f32(r);
  if (r.overrun())
    return false;
  bool correction = false;
  if (r.bits_remaining() >= 8) {
    correction = (r.read_bits(8) & kForceCorrection) != 0;
  }
  // EntityId is 32-bit; a varuint that does not fit is malformed and is rejected rather than
  // silently truncated into a different (possibly live) id.
  if (e > 0xffffffffull)
    return false;
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
    return false;
  out_e = static_cast<core::EntityId>(e);
  out_epoch = static_cast<std::uint8_t>(epoch);
  out_pos = pos;
  if (out_correction) {
    *out_correction = correction;
  }
  return true;
}

// A forced rotation must be a finite unit quaternion; the tolerance absorbs float rounding of a
// normalized value but rejects anything that would scale or shear the entity's world matrix.
inline bool valid_force_rotation(const core::Quat& q) {
  if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
    return false;
  const float length = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::fabs(length - 1.0f) <= 1e-3f;
}

inline std::vector<std::uint8_t> encode_force_rotation(core::EntityId e, std::uint8_t epoch,
                                                       const core::Quat& rotation) {
  core::BitWriter w;
  w.write_bits(kCtrlForceRotation, 8);
  w.write_varuint(e);
  w.write_bits(epoch, 8);
  detail::write_control_f32(w, rotation.x);
  detail::write_control_f32(w, rotation.y);
  detail::write_control_f32(w, rotation.z);
  detail::write_control_f32(w, rotation.w);
  return w.bytes();
}

inline bool decode_force_rotation(const std::vector<std::uint8_t>& payload, core::EntityId& out_e,
                                  std::uint8_t& out_epoch, core::Quat& out_rotation) {
  core::BitReader r(payload);
  if (r.read_bits(8) != kCtrlForceRotation)
    return false;
  const std::uint64_t e = r.read_varuint();
  const std::uint64_t epoch = r.read_bits(8);
  core::Quat rotation;
  rotation.x = detail::read_control_f32(r);
  rotation.y = detail::read_control_f32(r);
  rotation.z = detail::read_control_f32(r);
  rotation.w = detail::read_control_f32(r);
  if (r.overrun() || e > 0xffffffffull || !valid_force_rotation(rotation))
    return false;
  out_e = static_cast<core::EntityId>(e);
  out_epoch = static_cast<std::uint8_t>(epoch);
  out_rotation = rotation;
  return true;
}

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_CONTROL_HPP
