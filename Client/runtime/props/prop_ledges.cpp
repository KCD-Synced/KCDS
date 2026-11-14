#include "prop_ledges.hpp"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace kcds::runtime::props::ledges {
namespace {

constexpr std::size_t kServicesGame = 0xE0;
constexpr std::size_t kGameLedgeManager = 0x110;
constexpr std::size_t kLedges = 0;
constexpr std::size_t kMarkers = 8;
constexpr std::size_t kLedgeCount = 16;
constexpr std::size_t kMarkerCount = 20;
constexpr std::size_t kLedgeBytes = 16;
constexpr std::size_t kMarkerBytes = 28;
// A fence's ledge as the level's one-sided ones are: its side kept in the grid and vaulted
// (0x20f; the level's fences have 0x22f, whose 0x20 adds the other side), the other side unused
// (1). The other side of a ledge of ours vaulted the player back to where they came from: a fence
// has two ledges instead, one for each way over it.
constexpr std::uint16_t kSideFlags = 0x20f;
constexpr std::uint16_t kUnusedSideFlags = 0x1;
constexpr std::uint32_t kLedgesPerFence = 2;
// How far from each end a vault may not start: the level's 0.5 m left only the middle 0.6 m of a
// 1.6 m fence piece (the level's own fence ledges run along many pieces).
constexpr float kCornerEndAdjust = 0.1f;
// The most ledges the manager's u16 marker indices reach.
constexpr std::uint32_t kMostMarkers = 65535;

using Resize = void(__fastcall*)(void* manager, unsigned count, std::int64_t unused);
using Rebuild = void(__fastcall*)(void* manager);

struct Ledge {
  std::array<float, 3> from{};
  std::array<float, 3> to{};
};

Natives natives;
Logger log_line = nullptr;
bool installed = false;
std::map<std::uint32_t, Ledge> wanted;
bool dirty = false;
// The level's own ledges and markers before ours, and how many of ours the arrays hold now.
std::uint32_t base_ledges = 0;
std::uint32_t base_markers = 0;
std::uint32_t added = 0;
bool based = false;

template <class T>
T& field(void* base, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
}

void* manager() {
  void* services = natives.services ? natives.services() : nullptr;
  void* game = services ? field<void*>(services, kServicesGame) : nullptr;
  return game ? field<void*>(game, kGameLedgeManager) : nullptr;
}

// The manager's arrays made of the level's own entries and ours, and its grid built again.
void apply(void* ledger) {
  const std::uint32_t ledges_now = field<std::uint32_t>(ledger, kLedgeCount);
  const std::uint32_t markers_now = field<std::uint32_t>(ledger, kMarkerCount);
  // The arrays hold the level's and ours as last made, or the level loaded its own again.
  if (!based || ledges_now != base_ledges + added || markers_now != base_markers + added * 2) {
    base_ledges = ledges_now;
    base_markers = markers_now;
    added = 0;
    based = true;
  }
  const std::uint32_t fences = std::uint32_t(wanted.size());
  const std::uint32_t count = fences * kLedgesPerFence;
  if (base_markers + count * 2 > kMostMarkers) {
    if (log_line) {
      log_line("LEDGES %u fence ledges are more than the manager holds", fences);
    }
    return;
  }
  // The resize frees the arrays: the level's entries are kept aside first.
  std::vector<std::uint8_t> level_ledges(std::size_t(base_ledges) * kLedgeBytes);
  std::vector<std::uint8_t> level_markers(std::size_t(base_markers) * kMarkerBytes);
  if (!level_ledges.empty()) {
    std::memcpy(level_ledges.data(), field<std::uint8_t*>(ledger, kLedges), level_ledges.size());
  }
  if (!level_markers.empty()) {
    std::memcpy(level_markers.data(), field<std::uint8_t*>(ledger, kMarkers), level_markers.size());
  }
  reinterpret_cast<Resize>(natives.resize_ledges)(ledger, base_ledges + count, 0);
  reinterpret_cast<Resize>(natives.resize_markers)(ledger, base_markers + count * 2, 0);
  auto* ledges = field<std::uint8_t*>(ledger, kLedges);
  auto* markers = field<std::uint8_t*>(ledger, kMarkers);
  if ((base_ledges + count > 0 && !ledges) || (base_markers + count * 2 > 0 && !markers)) {
    added = 0;
    return;
  }
  if (!level_ledges.empty()) {
    std::memcpy(ledges, level_ledges.data(), level_ledges.size());
  }
  if (!level_markers.empty()) {
    std::memcpy(markers, level_markers.data(), level_markers.size());
  }
  std::uint32_t index = 0;
  for (const auto& [id, ledge] : wanted) {
    // One ledge each way: its facing (the left of its line) points to the side the player vaults
    // from, so the second, running the other way, serves the other side.
    for (std::uint32_t way = 0; way < kLedgesPerFence; ++way) {
      const std::array<float, 3>& start = way == 0 ? ledge.from : ledge.to;
      const std::array<float, 3>& end = way == 0 ? ledge.to : ledge.from;
      std::uint8_t* entry = ledges + std::size_t(base_ledges + index) * kLedgeBytes;
      const std::uint32_t first = base_markers + index * 2;
      std::memset(entry, 0, kLedgeBytes);
      field<std::uint16_t>(entry, 4) = std::uint16_t(first);
      field<std::uint16_t>(entry, 6) = 2;
      field<std::uint16_t>(entry, 8) = kSideFlags;
      field<std::uint16_t>(entry, 10) = kUnusedSideFlags;
      field<float>(entry, 12) = kCornerEndAdjust;
      const float dx = end[0] - start[0];
      const float dy = end[1] - start[1];
      const float length = std::sqrt(dx * dx + dy * dy);
      const std::array<float, 3> facing =
          length > 0.0f ? std::array<float, 3>{-dy / length, dx / length, 0.0f}
                        : std::array<float, 3>{1.0f, 0.0f, 0.0f};
      for (std::uint32_t point = 0; point < 2; ++point) {
        std::uint8_t* marker = markers + std::size_t(first + point) * kMarkerBytes;
        const std::array<float, 3>& at = point == 0 ? start : end;
        std::memcpy(marker, at.data(), sizeof(float) * 3);
        std::memcpy(marker + 12, facing.data(), sizeof(float) * 3);
        field<std::int32_t>(marker, 24) = 1;
      }
      ++index;
    }
  }
  added = count;
  reinterpret_cast<Rebuild>(natives.rebuild_grid)(ledger);
  if (log_line) {
    log_line("LEDGES %u fence(s), %u ledges after the level's %u", fences, count, base_ledges);
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed =
      natives.services && natives.resize_ledges && natives.resize_markers && natives.rebuild_grid;
  if (log_line) {
    log_line("LEDGES fence vaulting %s", installed ? "installed" : "unavailable");
  }
  return installed;
}

void set(std::uint32_t id, const float from[3], const float to[3]) {
  if (!installed) {
    return;
  }
  Ledge ledge;
  std::memcpy(ledge.from.data(), from, sizeof(float) * 3);
  std::memcpy(ledge.to.data(), to, sizeof(float) * 3);
  wanted[id] = ledge;
  dirty = true;
  if (log_line) {
    log_line("LEDGES prop %u: from %.2f %.2f %.2f to %.2f %.2f %.2f", id, from[0], from[1], from[2],
             to[0], to[1], to[2]);
  }
}

void remove(std::uint32_t id) {
  if (wanted.erase(id) > 0) {
    dirty = true;
  }
}

void frame() {
  if (!installed) {
    return;
  }
  void* ledger = manager();
  if (!ledger) {
    return;
  }
  // The level loaded its arrays again (a new world, a save): ours are added again after them.
  if (based && added > 0 &&
      (field<std::uint32_t>(ledger, kLedgeCount) != base_ledges + added ||
       field<std::uint32_t>(ledger, kMarkerCount) != base_markers + added * 2)) {
    dirty = true;
  }
  if (!dirty) {
    return;
  }
  dirty = false;
  apply(ledger);
}

void reset() {
  wanted.clear();
  dirty = false;
  based = false;
  added = 0;
}

}  // namespace kcds::runtime::props::ledges
