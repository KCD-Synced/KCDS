#include "kettles.hpp"

#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kcds::runtime::inventory {
namespace {

// How far from a pot's trigger a player eats from it, in metres.
constexpr float kReach = 4.0f;
constexpr std::size_t kGuidLength = 36;
constexpr std::size_t kMaxKettles = 4096;

KettleNatives natives;
std::mutex kettles_mutex;
// Trigger entity -> its fireplace's GUID.
std::unordered_map<std::uint32_t, std::string> kettles;

// Whether text at `at` is a dashed GUID (8-4-4-4-12 hex digits).
bool guid_at(const char* at) {
  for (std::size_t i = 0; i < kGuidLength; ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    const char c = at[i];
    if (dash ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

}  // namespace

void install_kettles(const KettleNatives& given) {
  natives = given;
}

std::string fireplace_of_name(const char* name) {
  const char* open = name ? std::strrchr(name, '[') : nullptr;
  const char* close = open ? std::strchr(open, ']') : nullptr;
  if (!close || std::size_t(close - open) <= kGuidLength) {
    return {};
  }
  // The last GUID inside the brackets: the fireplace's.
  for (const char* at = close - kGuidLength; at > open; --at) {
    if (guid_at(at)) {
      std::string guid(at, kGuidLength);
      for (char& c : guid) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
      }
      return guid;
    }
  }
  return {};
}

void kettle_spawned(std::uint32_t entity, const char* name) {
  std::string fireplace = fireplace_of_name(name);
  if (fireplace.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(kettles_mutex);
  if (kettles.size() < kMaxKettles) {
    kettles[entity] = std::move(fireplace);
  }
}

std::optional<std::string> pot_at_player() {
  float player[3] = {};
  if (!natives.entity_position || !natives.player_position || !natives.player_position(player)) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(kettles_mutex);
  std::optional<std::string> nearest;
  float best = kReach * kReach;
  std::vector<std::uint32_t> gone;
  for (const auto& [entity, fireplace] : kettles) {
    float at[3] = {};
    if (!natives.entity_position(entity, at)) {
      gone.push_back(entity);  // its part of the level unloaded
      continue;
    }
    const float dx = at[0] - player[0];
    const float dy = at[1] - player[1];
    const float dz = at[2] - player[2];
    const float distance = dx * dx + dy * dy + dz * dz;
    if (distance <= best) {
      best = distance;
      nearest = fireplace;
    }
  }
  for (const std::uint32_t entity : gone) {
    kettles.erase(entity);
  }
  return nearest;
}

}  // namespace kcds::runtime::inventory
