#include "navmesh_holes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "kcd2s/integration/script_channel.hpp"
// Last: it brings Windows.h, whose min and max macros the headers above must not see.
#include "MinHook.h"

namespace kcds::runtime::world::navmesh_holes {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using kcds::game_client::resolve_unique;

// The game's own marking of the polygons in a box (its baked paths' pass): a polygon query with a
// filter of its own, then each polygon's flags read and written again (E8 at each offset).
constexpr char kMarkLoop[] =
    "49 8B 47 08 33 D2 48 8B 48 10 48 89 0D ?? ?? ?? ?? 48 8D 4D E0 E8 ?? ?? ?? ?? B8 FF FF 00 00 "
    "89 54 24 44 66 89 85 E8 00 00 00 4C 8D 4D E0 48 8D 44 24 44 C7 44 24 30 00 08 00 00 48 89 44 "
    "24 28 4C 8D 44 24 60 48 8D 54 24 70 48 89 74 24 20 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 44 "
    "24 44 85 C0 74 ?? 44 8B F0 0F 1F 40 00 66 66 66 0F 1F 84 00 00 00 00 00 8B 3E 4C 8D 4D A8 49 "
    "8B 0F 4C 8D 44 24 48 8B D7 E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B 47 08 4C 8D 44 24 40 8B D7 48 8B "
    "58 10 48 8B CB E8 ?? ?? ?? ?? 44 0F B6 44 24 40 8B D7 41 80 C8 08 48 8B CB E8";
constexpr int kFilterInitAt = 22;
constexpr int kQueryPolygonsAt = 87;
constexpr int kGetFlagsAt = 161;
constexpr int kSetFlagsAt = 181;
// The game's nearest-polygon query (its first argument a query whose first field is the navmesh).
constexpr char kNearestPolygon[] =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 28 FE FF FF 48 81 EC 98 02 00 00 0F 29 "
    "70 A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 70 01 00 00";

// The query filter's include flags (all, as the game's own marking sets them), and the walk flags
// its path requests ask for.
constexpr std::size_t kFilterInclude = 264;
constexpr std::size_t kFilterBytes = 512;
constexpr std::uint8_t kWalkFlags = 7;
constexpr std::uint32_t kStatusSuccess = 0x40000000u;
// The most polygons a box takes, the navmeshes kept, and how often blocks are laid again.
constexpr int kMostPolygons = 2048;
constexpr std::size_t kMostNavmeshes = 8;
constexpr auto kLayAgain = std::chrono::seconds(3);
constexpr std::size_t kMaxQueued = 1024;

using FilterInit = void*(__fastcall*)(void* filter, int mode);
using QueryPolygons = std::uint32_t(__fastcall*)(void* query, const float* center,
                                                 const float* half, const void* filter,
                                                 std::uint32_t* polygons, int* count, int most);
using GetFlags = std::uint32_t(__fastcall*)(void* navmesh, std::uint32_t polygon,
                                            std::uint8_t* flags);
using SetFlags = std::uint32_t(__fastcall*)(void* navmesh, std::uint32_t polygon,
                                            std::uint8_t flags);
using NearestPolygon = std::uint32_t(__fastcall*)(void* query, const float* center,
                                                  const float* half, const void* filter,
                                                  std::uint32_t* polygon, float* nearest);

Logger log_line = nullptr;
FilterInit filter_init = nullptr;
QueryPolygons query_polygons = nullptr;
GetFlags get_flags = nullptr;
SetFlags set_flags = nullptr;
NearestPolygon original_nearest = nullptr;
std::array<std::atomic<void*>, kMostNavmeshes> navmeshes{};

struct Order {
  bool block = false;
  std::uint32_t id = 0;
  float center[3] = {};
  float half[3] = {};
};

struct Block {
  float center[3] = {};
  float half[3] = {};
  // Each polygon's flags before the block, by navmesh and polygon.
  std::map<std::pair<void*, std::uint32_t>, std::uint8_t> before;
};

std::mutex queue_mutex;
std::vector<Order> queued;
std::map<std::uint32_t, Block> blocks;
std::chrono::steady_clock::time_point next_lay;

// The game's AI asks for the nearest polygon: the navmesh it asks of is kept.
std::uint32_t __fastcall hooked_nearest(void* query, const float* center, const float* half,
                                        const void* filter, std::uint32_t* polygon,
                                        float* nearest) {
  void* navmesh = query ? *static_cast<void**>(query) : nullptr;
  if (navmesh) {
    for (auto& known : navmeshes) {
      void* seen = known.load(std::memory_order_relaxed);
      if (seen == navmesh) {
        break;
      }
      if (!seen) {
        void* empty = nullptr;
        if (known.compare_exchange_strong(empty, navmesh) || empty == navmesh) {
          break;
        }
      }
    }
  }
  return original_nearest(query, center, half, filter, polygon, nearest);
}

// Takes the walk flags away from the polygons in the block's box, on every navmesh known.
void lay(Block& block) {
  alignas(16) std::uint8_t filter[kFilterBytes] = {};
  filter_init(filter, 0);
  *reinterpret_cast<std::uint16_t*>(filter + kFilterInclude) = 0xFFFF;
  static std::uint32_t polygons[kMostPolygons];
  for (auto& known : navmeshes) {
    void* navmesh = known.load();
    if (!navmesh) {
      continue;
    }
    void* query[2] = {navmesh, nullptr};
    int count = 0;
    query_polygons(query, block.center, block.half, filter, polygons, &count, kMostPolygons);
    for (int index = 0; index < count && index < kMostPolygons; ++index) {
      std::uint8_t flags = 0;
      if ((get_flags(navmesh, polygons[index], &flags) & kStatusSuccess) == 0 ||
          (flags & kWalkFlags) == 0) {
        continue;
      }
      block.before.emplace(std::make_pair(navmesh, polygons[index]), flags);
      set_flags(navmesh, polygons[index], std::uint8_t(flags & ~kWalkFlags));
    }
  }
}

// Gives the polygons their flags back.
void lift(Block& block) {
  for (const auto& [where, flags] : block.before) {
    set_flags(where.first, where.second, flags);
  }
  block.before.clear();
}

double number_at(const sdk::Value& list, std::size_t index) {
  if (list.tag() != sdk::Tag::Array || index >= list.as_array().size()) {
    return 0;
  }
  const sdk::Value& item = list.as_array()[index];
  if (item.tag() == sdk::Tag::Double) {
    return item.as_double();
  }
  return item.tag() == sdk::Tag::Int ? double(item.as_int()) : 0;
}

}  // namespace

bool install(Logger log) {
  log_line = log;
  filter_init = reinterpret_cast<FilterInit>(
      resolve_unique("NavmeshFilterInit", kMarkLoop, kFilterInitAt, kFilterInitAt + 4));
  query_polygons = reinterpret_cast<QueryPolygons>(
      resolve_unique("NavmeshQueryPolygons", kMarkLoop, kQueryPolygonsAt, kQueryPolygonsAt + 4));
  get_flags = reinterpret_cast<GetFlags>(
      resolve_unique("NavmeshGetPolyFlags", kMarkLoop, kGetFlagsAt, kGetFlagsAt + 4));
  set_flags = reinterpret_cast<SetFlags>(
      resolve_unique("NavmeshSetPolyFlags", kMarkLoop, kSetFlagsAt, kSetFlagsAt + 4));
  void* nearest = resolve_unique("NavmeshNearestPolygon", kNearestPolygon);
  const bool found = filter_init && query_polygons && get_flags && set_flags && nearest &&
                     MH_CreateHook(nearest, reinterpret_cast<void*>(&hooked_nearest),
                                   reinterpret_cast<void**>(&original_nearest)) == MH_OK &&
                     MH_EnableHook(nearest) == MH_OK;
  if (log_line) {
    log_line("NAVMESH holes %s", found ? "installed" : "unavailable");
  }
  if (!found) {
    filter_init = nullptr;
  }
  return found;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "localNavmesh") {
    return false;
  }
  Order order;
  const sdk::Value* kind = integration::script_field(message, "kind");
  const sdk::Value* id = integration::script_field(message, "id");
  if (!kind || kind->tag() != sdk::Tag::Str || !id ||
      (id->tag() != sdk::Tag::Int && id->tag() != sdk::Tag::Double)) {
    return true;
  }
  order.id = std::uint32_t(id->tag() == sdk::Tag::Int ? id->as_int() : id->as_double());
  order.block = kind->as_str() == "block";
  if (order.block) {
    const sdk::Value* low = integration::script_field(message, "min");
    const sdk::Value* high = integration::script_field(message, "max");
    if (!low || !high) {
      return true;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double a = number_at(*low, axis);
      const double b = number_at(*high, axis);
      order.center[axis] = float((a + b) / 2);
      order.half[axis] = float((b > a ? b - a : a - b) / 2);
    }
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(order);
  }
  return true;
}

void block(std::uint32_t id, const float center[3], const float half[3]) {
  Order order;
  order.block = true;
  order.id = id;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    order.center[axis] = center[axis];
    order.half[axis] = half[axis];
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(order);
  }
}

void unblock(std::uint32_t id) {
  Order order;
  order.id = id;
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(order);
  }
}

// A level has a navmesh per agent size (people, horses, ...), and the one the AI asked first need
// not be the people's: every navmesh known is asked, and a point is off the navmesh only when none
// has a walkable polygon near it.
Nearest nearest_walkable(const float center[3], const float half[3], float point[3]) {
  if (!filter_init || !original_nearest) {
    return Nearest::Unknown;
  }
  alignas(16) std::uint8_t filter[kFilterBytes] = {};
  filter_init(filter, 0);
  *reinterpret_cast<std::uint16_t*>(filter + kFilterInclude) = kWalkFlags;
  bool asked = false;
  for (auto& known : navmeshes) {
    void* navmesh = known.load();
    if (!navmesh) {
      continue;
    }
    asked = true;
    void* query[2] = {navmesh, nullptr};
    std::uint32_t polygon = 0;
    float nearest[3] = {};
    const std::uint32_t status = original_nearest(query, center, half, filter, &polygon, nearest);
    if ((status & kStatusSuccess) != 0 && polygon != 0) {
      std::copy(nearest, nearest + 3, point);
      return Nearest::Found;
    }
  }
  return asked ? Nearest::None : Nearest::Unknown;
}

void frame(bool in_world) {
  if (!in_world || !filter_init) {
    return;
  }
  std::vector<Order> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  for (const Order& order : orders) {
    auto found = blocks.find(order.id);
    if (found != blocks.end()) {
      lift(found->second);
      blocks.erase(found);
    }
    if (order.block) {
      Block& block = blocks[order.id];
      std::copy(order.center, order.center + 3, block.center);
      std::copy(order.half, order.half + 3, block.half);
      lay(block);
      if (log_line) {
        log_line("NAVMESH block %u at %.1f %.1f: %zu polygon(s)", order.id, order.center[0],
                 order.center[1], block.before.size());
      }
    }
  }
  // Tiles that came in since carry the level's own flags again.
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_lay) {
    next_lay = now + kLayAgain;
    for (auto& [id, block] : blocks) {
      lay(block);
    }
  }
}

void reset() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queued.clear();
  }
  blocks.clear();
  for (auto& known : navmeshes) {
    known.store(nullptr);
  }
}

}  // namespace kcds::runtime::world::navmesh_holes
