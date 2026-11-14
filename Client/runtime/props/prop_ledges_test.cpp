// Fence ledges in the game's ledge manager (props/prop_ledges.hpp), on a manager made here.
#include "prop_ledges.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace ledges = kcds::runtime::props::ledges;

constexpr std::size_t kLedgeBytes = 16;
constexpr std::size_t kMarkerBytes = 28;

// The manager as the game keeps it: its ledge and marker arrays and their counts.
struct Manager {
  std::uint8_t* ledges = nullptr;
  std::uint8_t* markers = nullptr;
  std::uint32_t ledge_count = 0;
  std::uint32_t marker_count = 0;
};

Manager manager;
std::vector<std::uint8_t> ledge_store;
std::vector<std::uint8_t> marker_store;
int rebuilds = 0;
// The game's services (+ 0xE0 the game) and the game (+ 0x110 the manager).
std::uint8_t services[0x100] = {};
std::uint8_t game[0x120] = {};

void* get_services() {
  return services;
}

// As the game's resize: a new array (its content not kept) and its count.
void __fastcall resize_ledges(void* given, unsigned count, std::int64_t) {
  auto* made = static_cast<Manager*>(given);
  ledge_store.assign(std::size_t(count) * kLedgeBytes, 0xCD);
  made->ledges = ledge_store.empty() ? nullptr : ledge_store.data();
  made->ledge_count = count;
}

void __fastcall resize_markers(void* given, unsigned count, std::int64_t) {
  auto* made = static_cast<Manager*>(given);
  marker_store.assign(std::size_t(count) * kMarkerBytes, 0xCD);
  made->markers = marker_store.empty() ? nullptr : marker_store.data();
  made->marker_count = count;
}

void __fastcall rebuild_grid(void*) {
  ++rebuilds;
}

// A level with `count` ledges of two markers each; each entry's bytes are its index plus `seed`.
void load_level(std::uint32_t count, std::uint8_t seed) {
  resize_ledges(&manager, count, 0);
  resize_markers(&manager, count * 2, 0);
  for (std::size_t at = 0; at < ledge_store.size(); ++at) {
    ledge_store[at] = std::uint8_t(at / kLedgeBytes + seed);
  }
  for (std::size_t at = 0; at < marker_store.size(); ++at) {
    marker_store[at] = std::uint8_t(at / kMarkerBytes + seed);
  }
}

bool level_kept(std::uint32_t count, std::uint8_t seed) {
  for (std::size_t at = 0; at < std::size_t(count) * kLedgeBytes; ++at) {
    if (manager.ledges[at] != std::uint8_t(at / kLedgeBytes + seed)) {
      return false;
    }
  }
  for (std::size_t at = 0; at < std::size_t(count) * 2 * kMarkerBytes; ++at) {
    if (manager.markers[at] != std::uint8_t(at / kMarkerBytes + seed)) {
      return false;
    }
  }
  return true;
}

template <class T>
T read(const std::uint8_t* base, std::size_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(T));
  return value;
}

void start(std::uint32_t level_ledges) {
  void* manager_address = &manager;
  void* game_address = game;
  std::memcpy(services + 0xE0, &game_address, sizeof(void*));
  std::memcpy(game + 0x110, &manager_address, sizeof(void*));
  ledges::Natives natives;
  natives.services = get_services;
  natives.resize_ledges = reinterpret_cast<void*>(resize_ledges);
  natives.resize_markers = reinterpret_cast<void*>(resize_markers);
  natives.rebuild_grid = reinterpret_cast<void*>(rebuild_grid);
  ledges::install(natives, nullptr);
  ledges::reset();
  load_level(level_ledges, 1);
  rebuilds = 0;
}

void check_marker(kcd2s::testkit::Context& kcd2s_ctx, std::uint32_t index, const float at[3],
                  float facing_x, float facing_y) {
  const std::uint8_t* marker = manager.markers + std::size_t(index) * kMarkerBytes;
  KCD2S_CHECK(read<float>(marker, 0) == at[0]);
  KCD2S_CHECK(read<float>(marker, 4) == at[1]);
  KCD2S_CHECK(read<float>(marker, 8) == at[2]);
  KCD2S_CHECK(read<float>(marker, 12) == facing_x);
  KCD2S_CHECK(read<float>(marker, 16) == facing_y);
  KCD2S_CHECK(read<float>(marker, 20) == 0.0f);
  KCD2S_CHECK_EQ(read<std::int32_t>(marker, 24), 1);
}

const float kFrom[3] = {0.0f, 0.0f, 1.0f};
const float kTo[3] = {2.0f, 0.0f, 1.0f};

}  // namespace

KCD2S_TEST(prop_ledges, adds_one_ledge_each_way_after_the_levels) {
  start(3);
  ledges::set(7, kFrom, kTo);
  ledges::frame();
  KCD2S_CHECK_EQ(manager.ledge_count, 5u);
  KCD2S_CHECK_EQ(manager.marker_count, 10u);
  KCD2S_CHECK(level_kept(3, 1));
  KCD2S_CHECK_EQ(rebuilds, 1);
  for (std::uint32_t way = 0; way < 2; ++way) {
    const std::uint8_t* ledge = manager.ledges + std::size_t(3 + way) * kLedgeBytes;
    KCD2S_CHECK_EQ(read<std::uint32_t>(ledge, 0), 0u);
    KCD2S_CHECK_EQ(read<std::uint16_t>(ledge, 4), std::uint16_t(6 + way * 2));
    KCD2S_CHECK_EQ(read<std::uint16_t>(ledge, 6), std::uint16_t(2));
    KCD2S_CHECK_EQ(read<std::uint16_t>(ledge, 8), std::uint16_t(0x20f));
    KCD2S_CHECK_EQ(read<std::uint16_t>(ledge, 10), std::uint16_t(0x1));
    KCD2S_CHECK(read<float>(ledge, 12) == 0.1f);
  }
  // The first runs from -> to and faces its left (+y); the second runs back and faces -y.
  check_marker(kcd2s_ctx, 6, kFrom, 0.0f, 1.0f);
  check_marker(kcd2s_ctx, 7, kTo, 0.0f, 1.0f);
  check_marker(kcd2s_ctx, 8, kTo, 0.0f, -1.0f);
  check_marker(kcd2s_ctx, 9, kFrom, 0.0f, -1.0f);
}

KCD2S_TEST(prop_ledges, builds_again_only_when_something_changed) {
  start(2);
  ledges::set(7, kFrom, kTo);
  ledges::frame();
  ledges::frame();
  KCD2S_CHECK_EQ(rebuilds, 1);
  // Moved: the prop's ledge is replaced, not added.
  const float moved_to[3] = {0.0f, 3.0f, 1.0f};
  ledges::set(7, kFrom, moved_to);
  ledges::frame();
  KCD2S_CHECK_EQ(rebuilds, 2);
  KCD2S_CHECK_EQ(manager.ledge_count, 4u);
  KCD2S_CHECK(level_kept(2, 1));
  check_marker(kcd2s_ctx, 5, moved_to, -1.0f, 0.0f);
  // Taken away: only the level's are left.
  ledges::remove(7);
  ledges::frame();
  KCD2S_CHECK_EQ(rebuilds, 3);
  KCD2S_CHECK_EQ(manager.ledge_count, 2u);
  KCD2S_CHECK_EQ(manager.marker_count, 4u);
  KCD2S_CHECK(level_kept(2, 1));
}

KCD2S_TEST(prop_ledges, adds_them_again_after_a_level_loaded_again) {
  start(3);
  ledges::set(7, kFrom, kTo);
  ledges::set(8, kTo, kFrom);
  ledges::frame();
  KCD2S_CHECK_EQ(manager.ledge_count, 7u);
  // The level loads its own arrays again (a save loaded): ours go after them again.
  load_level(4, 40);
  ledges::frame();
  KCD2S_CHECK_EQ(rebuilds, 2);
  KCD2S_CHECK_EQ(manager.ledge_count, 8u);
  KCD2S_CHECK_EQ(manager.marker_count, 16u);
  KCD2S_CHECK(level_kept(4, 40));
  const std::uint8_t* first_ours = manager.ledges + std::size_t(4) * kLedgeBytes;
  KCD2S_CHECK_EQ(read<std::uint16_t>(first_ours, 4), std::uint16_t(8));
}

KCD2S_TEST(prop_ledges, leaves_the_manager_alone_without_fences) {
  start(3);
  ledges::frame();
  KCD2S_CHECK_EQ(rebuilds, 0);
  KCD2S_CHECK_EQ(manager.ledge_count, 3u);
  KCD2S_CHECK(level_kept(3, 1));
}
