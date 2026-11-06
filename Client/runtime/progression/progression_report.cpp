#include "progression_report.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::progression {
namespace {

namespace integration = kcd2s::integration;

constexpr std::size_t kSkills = 1512;
constexpr std::size_t kStats = 1388;
constexpr std::size_t kPerkSet = 1880;
constexpr std::size_t kPerksBegin = 64;
constexpr std::size_t kPerksEnd = 72;
constexpr std::size_t kPerkId = 16;
constexpr std::size_t kGrantedPerks = 5;
constexpr std::size_t kPerkPoints = 88;
constexpr std::size_t kStatPoints = 16;
constexpr std::size_t kSkillPoints = 36;
constexpr std::size_t kMainPoints = 106;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

// A perk instance's vector of the perks it grants with it ({ begin, end } of instance pointers).
struct InstanceList {
  void* const* begin;
  void* const* end;
};
using GrantedPerks = const InstanceList*(__fastcall*)(const void* instance);

void add_instance(const void* instance, std::vector<integration::PerkId>& perks) {
  integration::PerkId id{};
  std::memcpy(id.data(), static_cast<const std::uint8_t*>(instance) + kPerkId, id.size());
  perks.push_back(id);
}

std::uint8_t level_of(std::int32_t level) {
  if (level < 0) {
    return 0;
  }
  if (level > integration::kLevelCap) {
    return integration::kLevelCap;
  }
  return std::uint8_t(level);
}

}  // namespace

bool read(void* soul, integration::Progression& progression) {
  if (!soul) {
    return false;
  }
  const auto* perk_set = static_cast<const std::uint8_t*>(soul) + kPerkSet;
  const auto* begin = field<void* const*>(perk_set, kPerksBegin);
  const auto* end = field<void* const*>(perk_set, kPerksEnd);
  if (!begin || end < begin || std::size_t(end - begin) > integration::kMaxPerks) {
    return false;
  }
  const void* points = field<const void*>(perk_set, kPerkPoints);
  integration::Progression read_now;
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    const integration::TrackId track = integration::kTracks[i];
    const std::size_t pair = (track.stat ? kStats : kSkills) + 8 * std::size_t(track.id);
    integration::TrackState& state = read_now.tracks[i];
    state.level = level_of(field<std::int32_t>(soul, pair));
    state.xp = std::uint32_t(field<std::int32_t>(soul, pair + 4));
    if (points) {
      const std::size_t counter =
          (track.stat ? kStatPoints : kSkillPoints) + 2 * std::size_t(track.id);
      state.points = field<std::uint16_t>(points, counter);
    }
  }
  if (points) {
    read_now.main_points = field<std::uint16_t>(points, kMainPoints);
  }
  read_now.perks.reserve(std::size_t(end - begin));
  for (auto it = begin; it != end; ++it) {
    if (!*it) {
      continue;
    }
    add_instance(*it, read_now.perks);
    auto granted =
        reinterpret_cast<GrantedPerks>((*static_cast<void* const* const*>(*it))[kGrantedPerks]);
    const InstanceList* children = granted(*it);
    if (!children || children->end < children->begin ||
        std::size_t(children->end - children->begin) > integration::kMaxPerks) {
      continue;
    }
    for (auto child = children->begin; child != children->end; ++child) {
      if (*child) {
        add_instance(*child, read_now.perks);
      }
    }
  }
  // A perk granted by two owned perks is listed once.
  integration::normalize_perks(read_now.perks);
  if (read_now.perks.size() > integration::kMaxPerks) {
    read_now.perks.resize(integration::kMaxPerks);
  }
  progression = std::move(read_now);
  return true;
}

}  // namespace kcds::runtime::progression
