#include "horse_buffs.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "kcd2s/integration/horse_buffs.hpp"
#include "player_buffs.hpp"

namespace kcds::runtime::buffs::horses {
namespace {

namespace integration = kcd2s::integration;

struct Added {
  integration::Guid buff{};
  std::uint64_t instance = 0;
  double at = 0;
  bool holds = true;  // false: it ended within kHoldSeconds, not added again
};

struct Horse {
  void* soul = nullptr;
  std::string field;
  std::vector<integration::Guid> wanted;
  std::vector<Added> added;
  double next_check = 0;
  bool reported_bad = false;
};

std::unordered_map<std::uint32_t, Horse> horses;
Logger log = nullptr;

Added* find(Horse& horse, const integration::Guid& buff) {
  for (auto& entry : horse.added) {
    if (entry.buff == buff) {
      return &entry;
    }
  }
  return nullptr;
}

bool wanted(const Horse& horse, const integration::Guid& buff) {
  return std::find(horse.wanted.begin(), horse.wanted.end(), buff) != horse.wanted.end();
}

// A new field: the buffs that left it come off, the rest is checked now.
void take_field(std::uint32_t id, Horse& horse, const std::string& field) {
  std::vector<integration::Guid> parsed;
  if (!integration::parse_horse_buffs_field(field, parsed)) {
    if (!horse.reported_bad && log) {
      log("HORSE buffs horse=%u: a field it cannot read, kept the last one", id);
    }
    horse.reported_bad = true;
    horse.field = field;
    return;
  }
  horse.field = field;
  horse.wanted = std::move(parsed);
  horse.reported_bad = false;
  for (auto it = horse.added.begin(); it != horse.added.end();) {
    if (!wanted(horse, it->buff)) {
      if (it->instance) {
        remove_instance(it->instance);
      }
      it = horse.added.erase(it);
    } else {
      it->holds = true;
      ++it;
    }
  }
  horse.next_check = 0;
}

}  // namespace

void set_logger(Logger logger) {
  log = logger;
}

void apply(std::uint32_t id, void* soul, const std::string& field, double now) {
  if (!soul) {
    return;
  }
  auto& horse = horses[id];
  if (horse.soul != soul) {
    // A body made anew carries nothing of the old one's.
    horse.soul = soul;
    horse.added.clear();
    horse.next_check = 0;
  }
  if (field != horse.field) {
    take_field(id, horse, field);
  }
  if (now < horse.next_check) {
    return;
  }
  horse.next_check = now + 1;
  const std::vector<std::uint64_t> carried = instance_ids(soul);
  for (const auto& buff : horse.wanted) {
    Added* entry = find(horse, buff);
    if (entry && entry->instance &&
        std::find(carried.begin(), carried.end(), entry->instance) != carried.end()) {
      continue;
    }
    if (entry && !entry->holds) {
      continue;
    }
    if (entry && now - entry->at < kHoldSeconds) {
      // Added a moment ago and already gone: it acts once.
      entry->holds = false;
      continue;
    }
    const std::uint64_t instance = add_to_soul(soul, buff);
    if (!entry) {
      horse.added.push_back(Added{buff});
      entry = &horse.added.back();
    }
    entry->instance = instance;
    entry->at = now;
    if (log) {
      log("HORSE buff horse=%u %s %s", id, integration::guid_text(buff).c_str(),
          instance ? "added" : "refused");
    }
  }
}

void forget(std::uint32_t id) {
  horses.erase(id);
}

}  // namespace kcds::runtime::buffs::horses
