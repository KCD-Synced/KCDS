#include "graves.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::graves {
namespace {

namespace entity = kcds::runtime::props::entity;
using kcd2s::integration::Grave;
using kcd2s::integration::GraveKind;
using Key = std::array<std::int64_t, 3>;

constexpr char kGraveClass[] = "Hole";
constexpr char kDepth[] = "nDepth";
constexpr char kUsable[] = "canBeUsed";
// How often the graves are read, in seconds.
constexpr double kReadInterval = 0.5;

struct Seen {
  // The game's nDepth at the last read.
  float depth = 0.0f;
  // The local player's digging has the soil open (told as kDigging).
  bool digging = false;
};

double next_read = 0.0;
std::map<Key, Seen> seen;
// The server's state for each grave it set, and the graves still to be put into it.
std::map<Key, bool> wanted;
std::map<Key, bool> to_apply;
// Another player digs these graves: their soil is open here (kDigging), and still to be opened or
// closed on the game's grave.
std::map<Key, bool> others_digging;
std::map<Key, bool> to_open;

Key key_of(const float position[3]) {
  return {std::llround(double(position[0]) * 100.0), std::llround(double(position[1]) * 100.0),
          std::llround(double(position[2]) * 100.0)};
}

Grave grave_of(GraveKind kind, const Key& key, bool dug) {
  Grave grave;
  grave.kind = kind;
  grave.position[0] = key[0];
  grave.position[1] = key[1];
  grave.position[2] = key[2];
  grave.dug = dug;
  return grave;
}

// As the game's handler sets a grave: dug out, nDepth 1 and the soil hidden; full, nDepth 0,
// usable again and the soil shown.
float apply(void* hole, bool dug) {
  const float depth = dug ? 1.0f : 0.0f;
  entity::set_script_number(hole, kDepth, depth);
  if (!dug) {
    entity::set_script_flag(hole, kUsable, true);
  }
  entity::hide(hole, dug);
  return depth;
}

}  // namespace

std::vector<Grave> frame(double now, const std::vector<Grave>& sets) {
  for (const Grave& set : sets) {
    const Key key{set.position[0], set.position[1], set.position[2]};
    if (set.kind == GraveKind::Digging) {
      others_digging[key] = set.dug;
      to_open[key] = set.dug;
      continue;
    }
    wanted[key] = set.dug;
    to_apply[key] = set.dug;
    // The server's state ends another player's digging.
    others_digging.erase(key);
    to_open.erase(key);
  }
  std::vector<Grave> out;
  if (now < next_read) {
    return out;
  }
  next_read = now + kReadInterval;
  entity::each_of_class(kGraveClass, [&](void* hole) {
    float position[3] = {0.0f, 0.0f, 0.0f};
    entity::world_position(hole, position);
    const Key key = key_of(position);
    float depth = 0.0f;
    if (!entity::script_number(hole, kDepth, depth)) {
      return;
    }
    const auto pending = to_apply.find(key);
    if (pending != to_apply.end()) {
      depth = apply(hole, pending->second);
      to_apply.erase(pending);
    }
    // Another player's digging: the soil opens or closes here as it does on their screen.
    const auto opening = to_open.find(key);
    if (opening != to_open.end()) {
      if (depth < 1.0f) {
        entity::hide(hole, opening->second);
      }
      to_open.erase(opening);
    }
    const auto known = seen.find(key);
    if (known == seen.end()) {
      if (seen.size() < kcd2s::integration::kMaxGraves) {
        seen[key] = Seen{depth};
        out.push_back(grave_of(GraveKind::Report, key, depth >= 1.0f));
      }
      return;
    }
    // Dug out since the last read, and not by the server: the local player's digging.
    const auto server = wanted.find(key);
    const bool server_dug = server != wanted.end() && server->second;
    if (known->second.depth < 1.0f && depth >= 1.0f && !server_dug) {
      out.push_back(grave_of(GraveKind::Dug, key, true));
    }
    // The local player's digging opens the soil before the grave is dug out; one that stops
    // short closes it again. Soil another player's digging opened is theirs.
    const auto other = others_digging.find(key);
    const bool others = other != others_digging.end() && other->second;
    const bool open = depth < 1.0f && entity::is_hidden(hole) && !others;
    if (open != known->second.digging) {
      known->second.digging = open;
      if (open || depth < 1.0f) {
        out.push_back(grave_of(GraveKind::Digging, key, open));
      }
    }
    known->second.depth = depth;
  });
  return out;
}

void reset() {
  next_read = 0.0;
  seen.clear();
  wanted.clear();
  to_apply.clear();
  others_digging.clear();
  to_open.clear();
}

}  // namespace kcds::runtime::graves
