#include "merged_grass.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "../architect/local_mode.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::terrain::grass {
namespace {

using kcds::game_client::resolve_unique;

// C3DEngine::OnExplosion: the terrain global (+ 24) and the ground's height at (x, y)
// (+ 39), called on that global.
constexpr char kTerrainHeight[] =
    "48 8B 05 ?? ?? ?? ?? 40 B6 01 39 98 70 03 00 00 75 03 40 8A F3 4C 8B 35 ?? ?? ?? ?? 0F 28 D7 "
    "49 8B CE 41 0F 28 C8 E8 ?? ?? ?? ??";
constexpr int kTerrainAt = 24;
constexpr int kTerrainEnd = 28;
constexpr int kHeightCall = 39;
constexpr int kHeightEnd = 43;
// C3DEngine::OnExplosion's road gathering (terrain_edits.cpp kRoadsTail): the render nodes of a
// type in a box (+ 23) and the game's free (+ 117).
constexpr char kNodesTail[] =
    "41 8D 51 0B F2 0F 11 44 24 ?? 89 45 ?? 48 C7 44 24 20 FF FF FF FF E8 ?? ?? ?? ?? 48 8B 44 24 "
    "?? 44 8B F3 4C 8B 44 24 ?? 49 2B C0 48 C1 F8 03 48 85 C0 74 ?? 48 8B F3 49 8B 0C 30 E8 ?? ?? "
    "?? ?? 48 8B 4C 24 ?? 48 8D 76 08 4C 8B 44 24 ?? 41 FF C6 49 2B C8 49 63 C6 48 C1 F9 03 48 3B "
    "C1 72 ?? 4D 85 C0 74 ?? 48 8B 54 24 ?? 49 8B C8 49 2B D0 48 83 E2 F8 E8 ?? ?? ?? ??";
constexpr int kNodesCall = 23;
constexpr int kNodesEnd = 27;
constexpr int kFreeCall = 117;
constexpr int kFreeEnd = 121;

// CMergedMeshRenderNode's streamed data copied into its instances (its entry: the
// state check at + 29 wants 4, streaming), called at the end of StreamAsyncOnComplete
// the cell's geometry is built from the instances after it.
constexpr char kDecode[] =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 55 41 56 41 57 48 83 EC 20 8B 41 "
    "6C 49 8B F0 48 8B E9 83 F8 04 0F 85";

// AddInstance's job manager load before it waits for the cell's job at + 624.
constexpr char kAddInstanceJobs[] =
    "48 8B 0D ?? ?? ?? ?? 48 8D 97 70 02 00 00 48 8B E8 41 B0 01 48 8B 01 FF 50 18";
constexpr int kJobsAt = 3;
constexpr int kJobsEnd = 7;
// The free of a group's spine buffers AddInstance calls: under the merged mesh
// lock, the pool's own free (slot 10 asks whether the pool holds it), else the heap's.
constexpr char kFreeSpines[] =
    "40 53 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 "
    "74 "
    "2F 48 8B 01 48 8B D3 FF 50 50 48 85 C0 74 21";
// AddInstance's 3D engine load before it registers the cell again (slot 38 at + 0x130).
constexpr char kAddInstanceEngine[] = "48 8B 0D ?? ?? ?? ?? F3 0F 11 8F A4 00 00 00 0F 28 CC";
constexpr int kEngineAt = 3;
constexpr int kEngineEnd = 7;

constexpr int kMergedMeshType = 23;
constexpr std::size_t kOrigin = 140;
// The cell's box: min x, y, z, max x, y, z.
constexpr std::size_t kCellBox = 164;
// How far above its ground a cell's box reaches (the grass's height).
constexpr float kGrassHeight = 2.0f;
constexpr std::size_t kChanged = 108;
constexpr std::size_t kVersion = 112;
constexpr std::size_t kHeaders = 240;
constexpr std::size_t kHeaderCount = 248;
constexpr std::size_t kHeaderSize = 128;
constexpr std::size_t kInstances = 0;
constexpr std::size_t kInstanceCount = 84;
// A group header's vegetation kind (the index AddInstance was given).
constexpr std::size_t kHeaderKind = 76;
constexpr std::size_t kInstanceSize = 16;
constexpr std::size_t kSizeA = 12;
constexpr std::size_t kSizeB = 14;
constexpr float kCell = 16.0f;
constexpr float kSteps = 65535.0f;
// The spacing of a shaped piece's height samples, and most cells prepared again per frame.
constexpr float kShapedStep = 0.5f;
constexpr std::size_t kCellsPerFrame = 8;
// The box a hide looks in reaches this far up and down.
constexpr float kTall = 2000.0f;
// Most headers and instances a node is trusted to hold (a sanity limit).
constexpr std::uint32_t kMostHeaders = 256;
constexpr std::uint32_t kMostInstances = 65535;

struct NodeList {
  std::uint8_t** begin = nullptr;
  std::uint8_t** end = nullptr;
  std::uint8_t** capacity = nullptr;
};

using NodesInBox = void(__fastcall*)(NodeList* out, int type, const float* box, std::int64_t none,
                                     std::int64_t all);
using FreeBlock = void(__fastcall*)(void* block, std::size_t bytes);
using HeightAt = float(__fastcall*)(void* terrain, float x, float y);

// A shaped piece of ground: its box (min x, y, max x, y) and the ground's height in it as it is
// after the edit, sampled every kShapedStep metres on the engine thread (the decode hook runs on
// the streaming thread and reads these instead of the terrain).
struct Shaped {
  float box[4] = {};
  int columns = 0;
  int rows = 0;
  std::vector<float> heights;
};

// An instance hidden: where, and its two size bytes as they were.
struct Shrunk {
  std::uint8_t* node = nullptr;
  std::uint32_t header = 0;
  std::uint32_t index = 0;
  std::uint8_t size_a = 0;
  std::uint8_t size_b = 0;
};

struct Hidden {
  float x = 0.0f;
  float y = 0.0f;
  float radius = 0.0f;
  std::vector<Shrunk> shrunk;
};

using WaitForJob = void(__fastcall*)(void* jobs, void* state, bool wait);
using FreeSpines = void(__fastcall*)(void* buffer);
using RegisterEntity = void(__fastcall*)(void* engine, void* node);

Logger log_line = nullptr;
void** job_manager = nullptr;
void** engine_global = nullptr;
FreeSpines free_spines = nullptr;
void** terrain_global = nullptr;
HeightAt height_at = nullptr;
NodesInBox nodes_in_box = nullptr;
FreeBlock free_block = nullptr;
// The ground edits' pieces, oldest first (read by the decode hook, under hidden_mutex), the box
// of the edit under way, and the boxes whose cells are still to be prepared again.
std::vector<Shaped> shaped;
float editing[6] = {};
bool editing_now = false;
std::vector<std::array<float, 6>> to_prepare;
// The hidden circles by id; read by the decode hook, which may run off the engine thread.
std::recursive_mutex hidden_mutex;
std::map<std::uint32_t, Hidden> hidden;
using Decode = void(__fastcall*)(void* node, void* stream, void* data);
Decode original_decode = nullptr;
// Whether grass moves with ground edits (before/after); hiding works either way.
bool follow = false;

bool ready() {
  return terrain_global && *terrain_global && height_at && nodes_in_box && free_block &&
         job_manager && *job_manager && free_spines && engine_global && *engine_global;
}

std::vector<std::uint8_t*> nodes(const float box[6]) {
  std::vector<std::uint8_t*> found;
  NodeList list;
  nodes_in_box(&list, kMergedMeshType, box, 0, -1);
  for (std::uint8_t** node = list.begin; node && node < list.end; ++node) {
    if (*node) {
      found.push_back(*node);
    }
  }
  if (list.begin) {
    const auto bytes = std::size_t(reinterpret_cast<std::uint8_t*>(list.capacity) -
                                   reinterpret_cast<std::uint8_t*>(list.begin));
    free_block(list.begin, bytes & ~std::size_t(7));
  }
  return found;
}

template <class T>
T& field(std::uint8_t* base, std::size_t offset) {
  return *reinterpret_cast<T*>(base + offset);
}

std::uint32_t header_count(std::uint8_t* node) {
  const std::uint32_t count = field<std::uint32_t>(node, kHeaderCount);
  return field<std::uint8_t*>(node, kHeaders) && count <= kMostHeaders ? count : 0;
}

std::uint8_t* header_of(std::uint8_t* node, std::uint32_t header) {
  return field<std::uint8_t*>(node, kHeaders) + std::size_t(header) * kHeaderSize;
}

// A header's instances and their count; null when they are not loaded.
std::uint8_t* instances_of(std::uint8_t* header, std::uint32_t& count) {
  std::uint8_t* instances = field<std::uint8_t*>(header, kInstances);
  count = field<std::uint32_t>(header, kInstanceCount);
  if (!instances || count > kMostInstances) {
    count = 0;
    return nullptr;
  }
  return instances;
}

// An instance's place in the world.
void place_of(std::uint8_t* node, std::uint8_t* instance, float place[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    const float origin = field<float>(node, kOrigin + std::size_t(axis) * 4);
    const float step = float(field<std::uint16_t>(instance, std::size_t(axis) * 2));
    place[axis] = origin + step / kSteps * kCell;
  }
}

// What CMergedMeshRenderNode::AddInstance does after it changes a cell's instances:
// it waits for the cell's jobs (the job manager's WaitForJob, slot 3, on the job states at + 624
// and + 632; the spines job's at + 640 is waited for too, it reads the buffers freed next), frees
// each group's spine buffers (+ 8 and + 16 of a 128-byte header), registers the
// cell with the 3D engine again (slot 38, RegisterEntity), and sets the cell's version + 1 and its
// state to 1, from which the game prepares it again.
constexpr std::size_t kJobStates[] = {624, 632, 640};
constexpr std::size_t kSpineBuffers[] = {8, 16};
constexpr std::uint32_t kPrepareAgain = 1;
constexpr std::uint32_t kReadyState = 5;
constexpr std::size_t kWaitForJob = 3;
constexpr std::size_t kRegisterEntity = 38;

// A cell's box (+ 164: min x, y, z, max x, y, z) reaches the shaped ground under it, as
// AddInstance grows it for a new instance: a box left under raised ground is hidden by the
// ground in front of it, and with it the whole cell. Engine thread.
void reach_shaped_ground(std::uint8_t* cell) {
  float* box = &field<float>(cell, kCellBox);
  std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
  for (const Shaped& piece : shaped) {
    const bool apart = piece.box[2] < box[0] || piece.box[0] > box[3] || piece.box[3] < box[1] ||
                       piece.box[1] > box[4];
    if (apart) {
      continue;
    }
    for (const float height : piece.heights) {
      box[2] = (std::min)(box[2], height);
      box[5] = (std::max)(box[5], height + kGrassHeight);
    }
  }
}

// A cell left while it was loading, tried again once it is ready (found again by its box: a
// cell may be gone by then).
struct Waiting {
  std::uint8_t* node = nullptr;
  std::array<float, 6> box{};
};
std::vector<Waiting> waiting;
constexpr std::size_t kMostWaiting = 256;

void changed(std::uint8_t* node) {
  // Only a ready cell (+ 108 is 5): one still loading takes the hidden circles and the shaped
  // ground in the decode hook anyway, and asking a loading cell to load again crashed the game's
  // stream reader (on a request already gone, 3 times while a project
  // replayed its ground edits).
  const std::uint32_t state = field<std::uint32_t>(node, kChanged);
  if (state != kReadyState) {
    const float* box = &field<float>(node, kCellBox);
    const bool known = std::any_of(waiting.begin(), waiting.end(),
                                   [&](const Waiting& entry) { return entry.node == node; });
    if (!known && waiting.size() < kMostWaiting) {
      waiting.push_back({node, {box[0], box[1], box[2], box[3], box[4], box[5]}});
    }
    return;
  }
  if (log_line) {
    log_line("GRASS cell %p prepared again", static_cast<void*>(node));
  }
  void* jobs = *job_manager;
  for (const std::size_t job_state : kJobStates) {
    kcds::game_client::slot<WaitForJob>(jobs, kWaitForJob)(jobs, node + job_state, true);
  }
  for (std::uint32_t header = 0; header < header_count(node); ++header) {
    std::uint8_t* group = header_of(node, header);
    for (const std::size_t buffer : kSpineBuffers) {
      void*& spines = field<void*>(group, buffer);
      if (spines) {
        free_spines(spines);
        spines = nullptr;
      }
    }
  }
  reach_shaped_ground(node);
  void* engine = *engine_global;
  kcds::game_client::slot<RegisterEntity>(engine, kRegisterEntity)(engine, node);
  field<std::uint64_t>(node, kVersion) += 1;
  field<std::uint32_t>(node, kChanged) = kPrepareAgain;
}

std::uint8_t* instance_at(std::uint8_t* node, std::uint32_t header, std::uint32_t index) {
  if (header >= header_count(node)) {
    return nullptr;
  }
  std::uint32_t count = 0;
  std::uint8_t* instances = instances_of(header_of(node, header), count);
  return instances && index < count ? instances + std::size_t(index) * kInstanceSize : nullptr;
}

// Shrinks every instance within the circle to nothing in the nodes given; the ones shrunk now.
std::vector<Shrunk> shrink(const std::vector<std::uint8_t*>& found, float x, float y, float radius,
                           bool mark = true) {
  std::vector<Shrunk> shrunk;
  for (std::uint8_t* node : found) {
    bool any = false;
    for (std::uint32_t header = 0; header < header_count(node); ++header) {
      std::uint32_t count = 0;
      std::uint8_t* instances = instances_of(header_of(node, header), count);
      for (std::uint32_t index = 0; instances && index < count; ++index) {
        std::uint8_t* instance = instances + std::size_t(index) * kInstanceSize;
        float place[3];
        place_of(node, instance, place);
        const float dx = place[0] - x;
        const float dy = place[1] - y;
        if (dx * dx + dy * dy > radius * radius) {
          continue;
        }
        if (instance[kSizeA] == 0 && instance[kSizeB] == 0) {
          continue;
        }
        shrunk.push_back({node, header, index, instance[kSizeA], instance[kSizeB]});
        instance[kSizeA] = 0;
        instance[kSizeB] = 0;
        any = true;
      }
    }
    if (any && mark) {
      changed(node);
    }
  }
  return shrunk;
}

void circle_box(float x, float y, float radius, float box[6]) {
  box[0] = x - radius;
  box[1] = y - radius;
  box[2] = -kTall;
  box[3] = x + radius;
  box[4] = y + radius;
  box[5] = kTall;
}

// The ground's height at (x, y) in a shaped piece (bilinear between its samples).
float ground_in(const Shaped& piece, float x, float y) {
  const float fx = std::clamp((x - piece.box[0]) / kShapedStep, 0.0f, float(piece.columns - 1));
  const float fy = std::clamp((y - piece.box[1]) / kShapedStep, 0.0f, float(piece.rows - 1));
  const int x0 = int(fx);
  const int y0 = int(fy);
  const int x1 = (std::min)(x0 + 1, piece.columns - 1);
  const int y1 = (std::min)(y0 + 1, piece.rows - 1);
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);
  const auto at = [&](int column, int row) {
    return piece.heights[std::size_t(row) * std::size_t(piece.columns) + std::size_t(column)];
  };
  const float near_row = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
  const float far_row = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
  return near_row + (far_row - near_row) * ty;
}

bool inside(const Shaped& piece, float x, float y) {
  return x >= piece.box[0] && x <= piece.box[2] && y >= piece.box[1] && y <= piece.box[3];
}

// The ground's height samples of a piece, from the terrain as it is now. Engine thread.
void sample(Shaped& piece) {
  void* terrain = *terrain_global;
  piece.heights.resize(std::size_t(piece.columns) * std::size_t(piece.rows));
  for (int row = 0; row < piece.rows; ++row) {
    for (int column = 0; column < piece.columns; ++column) {
      const float x = piece.box[0] + float(column) * kShapedStep;
      const float y = piece.box[1] + float(row) * kShapedStep;
      piece.heights[std::size_t(row) * std::size_t(piece.columns) + std::size_t(column)] =
          height_at(terrain, x, y);
    }
  }
}

// The grass of a cell just decoded set on the shaped ground (the newest piece over each), its
// height kept within the cell. Under hidden_mutex. How many moved.
std::size_t settle(std::uint8_t* cell) {
  std::size_t moved = 0;
  const float origin_z = field<float>(cell, kOrigin + 8);
  float* box = &field<float>(cell, kCellBox);
  for (std::uint32_t header = 0; header < header_count(cell); ++header) {
    std::uint32_t count = 0;
    std::uint8_t* instances = instances_of(header_of(cell, header), count);
    for (std::uint32_t index = 0; instances && index < count; ++index) {
      std::uint8_t* instance = instances + std::size_t(index) * kInstanceSize;
      float place[3];
      place_of(cell, instance, place);
      for (auto piece = shaped.rbegin(); piece != shaped.rend(); ++piece) {
        if (!inside(*piece, place[0], place[1])) {
          continue;
        }
        const float ground = ground_in(*piece, place[0], place[1]);
        const float within = std::clamp((ground - origin_z) / kCell * kSteps, 0.0f, kSteps);
        field<std::uint16_t>(instance, 4) = std::uint16_t(std::lround(within));
        // The cell's box reaches the grass (a box left under raised ground is hidden by it).
        const float height = origin_z + within / kSteps * kCell;
        box[2] = (std::min)(box[2], height);
        box[5] = (std::max)(box[5], height + kGrassHeight);
        ++moved;
        break;
      }
    }
  }
  return moved;
}

// Every hidden circle's grass in a cell just decoded (its instances fresh from the level data,
// before its geometry is built): their sizes to nothing.
void __fastcall hooked_decode(void* node, void* stream, void* data) {
  original_decode(node, stream, data);
  auto* cell = static_cast<std::uint8_t*>(node);
  std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
  if (!cell || (hidden.empty() && shaped.empty())) {
    return;
  }
  const std::size_t moved = shaped.empty() ? 0 : settle(cell);
  std::size_t count = 0;
  for (const auto& [id, entry] : hidden) {
    count += shrink({cell}, entry.x, entry.y, entry.radius, false).size();
  }
  if ((count > 0 || moved > 0) && log_line) {
    log_line("GRASS cell %p loaded: %zu instance(s) hidden, %zu on the shaped ground", node, count,
             moved);
  }
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  // The grass moves with ground edits (the cells load again, a few a frame, and the decode hook
  // sets their grass on the sampled ground: grass on the raised ground, 5
  // project replays of 107 edits without a fault). KCDS_ARCHITECT_GRASS_FOLLOW=0 turns it off.
  wchar_t follow_value[4] = {};
  const DWORD follow_length =
      GetEnvironmentVariableW(L"KCDS_ARCHITECT_GRASS_FOLLOW", follow_value, 4);
  follow = !(follow_length > 0 && follow_length < 4 && follow_value[0] == L'0');
  // Hiding the grass carpet is World Architect's alone (its local mode) and can be turned off
  // (KCDS_ARCHITECT_GRASS=0). A hidden circle's cells are prepared again the way AddInstance
  // does it (changed()): they load their data again and the decode hook hides the grass in them
  // (the ground bare under the circle, the grass back with show).
  wchar_t grass[4] = {};
  const DWORD grass_length = GetEnvironmentVariableW(L"KCDS_ARCHITECT_GRASS", grass, 4);
  const bool turned_off = grass_length > 0 && grass_length < 4 && grass[0] == L'0';
  if (!architect::local_mode::enabled() || turned_off) {
    if (log_line) {
      log_line("GRASS merged meshes off (%s)",
               turned_off ? "KCDS_ARCHITECT_GRASS=0" : "not World Architect's local mode");
    }
    return false;
  }
  terrain_global =
      static_cast<void**>(resolve_unique("GrassTerrain", kTerrainHeight, kTerrainAt, kTerrainEnd));
  height_at = reinterpret_cast<HeightAt>(
      resolve_unique("GrassHeightAt", kTerrainHeight, kHeightCall, kHeightEnd));
  nodes_in_box = reinterpret_cast<NodesInBox>(
      resolve_unique("GrassNodesInBox", kNodesTail, kNodesCall, kNodesEnd));
  free_block = reinterpret_cast<FreeBlock>(
      resolve_unique("GrassFreeBlock", kNodesTail, kFreeCall, kFreeEnd));
  job_manager =
      static_cast<void**>(resolve_unique("GrassJobManager", kAddInstanceJobs, kJobsAt, kJobsEnd));
  free_spines = reinterpret_cast<FreeSpines>(resolve_unique("GrassFreeSpines", kFreeSpines));
  engine_global =
      static_cast<void**>(resolve_unique("GrassEngine", kAddInstanceEngine, kEngineAt, kEngineEnd));
  void* decode = resolve_unique("GrassDecode", kDecode);
  const bool hooked = decode &&
                      MH_CreateHook(decode, reinterpret_cast<void*>(&hooked_decode),
                                    reinterpret_cast<void**>(&original_decode)) == MH_OK &&
                      MH_EnableHook(decode) == MH_OK;
  const bool found = terrain_global && height_at && nodes_in_box && free_block && job_manager &&
                     free_spines && engine_global && hooked;
  if (log_line && !found) {
    log_line(
        "GRASS found: terrain %d height %d nodes %d free %d jobs %d spines %d engine %d decode %d",
        terrain_global != nullptr, height_at != nullptr, nodes_in_box != nullptr,
        free_block != nullptr, job_manager != nullptr, free_spines != nullptr,
        engine_global != nullptr, hooked);
  }
  if (log_line) {
    log_line("GRASS merged meshes %s (follows ground edits: %s)",
             found ? "installed" : "unavailable", follow ? "yes" : "no");
  }
  return found;
}

void before(const float box[6]) {
  editing_now = ready() && follow;
  if (editing_now) {
    std::copy(box, box + 6, editing);
  }
}

int after() {
  if (!editing_now || !ready()) {
    editing_now = false;
    return 0;
  }
  editing_now = false;
  Shaped piece;
  piece.box[0] = editing[0];
  piece.box[1] = editing[1];
  piece.box[2] = editing[3];
  piece.box[3] = editing[4];
  piece.columns = int(std::ceil((piece.box[2] - piece.box[0]) / kShapedStep)) + 1;
  piece.rows = int(std::ceil((piece.box[3] - piece.box[1]) / kShapedStep)) + 1;
  if (piece.columns < 2 || piece.rows < 2 || piece.columns > 1024 || piece.rows > 1024) {
    return 0;
  }
  sample(piece);
  {
    std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
    // The older pieces this edit overlaps take the ground as it is now.
    for (Shaped& older : shaped) {
      const bool apart = older.box[2] < piece.box[0] || older.box[0] > piece.box[2] ||
                         older.box[3] < piece.box[1] || older.box[1] > piece.box[3];
      if (!apart) {
        sample(older);
      }
    }
    shaped.push_back(std::move(piece));
  }
  std::array<float, 6> box{};
  std::copy(editing, editing + 6, box.begin());
  to_prepare.push_back(box);
  return 1;
}

int hide(std::uint32_t id, float x, float y, float radius) {
  if (!ready() || !(radius > 0.0f)) {
    return 0;
  }
  float box[6];
  circle_box(x, y, radius, box);
  std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
  Hidden& entry = hidden[id];
  entry.x = x;
  entry.y = y;
  entry.radius = radius;
  // The loaded cells' grass shrinks now (and their spines are built again); cells loaded later
  // are shrunk by the decode hook.
  std::vector<Shrunk> shrunk = shrink(nodes(box), x, y, radius);
  const std::size_t count = shrunk.size();
  entry.shrunk.insert(entry.shrunk.end(), shrunk.begin(), shrunk.end());
  if (log_line) {
    log_line("GRASS hide %u: %zu instance(s) within %.1f m of %.1f %.1f", id, count, double(radius),
             double(x), double(y));
  }
  return int(count);
}

int show(std::uint32_t id) {
  if (!ready()) {
    return 0;
  }
  std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
  const auto found = hidden.find(id);
  if (found == hidden.end()) {
    return 0;
  }
  const Hidden entry = found->second;
  hidden.erase(found);
  // Only cells still loaded get their sizes back (a cell loaded again has its level data).
  float box[6];
  circle_box(entry.x, entry.y, entry.radius, box);
  const std::vector<std::uint8_t*> loaded = nodes(box);
  std::vector<std::uint8_t*> touched;
  int restored = 0;
  for (const Shrunk& item : entry.shrunk) {
    if (std::find(loaded.begin(), loaded.end(), item.node) == loaded.end()) {
      continue;
    }
    std::uint8_t* instance = instance_at(item.node, item.header, item.index);
    if (!instance) {
      continue;
    }
    instance[kSizeA] = item.size_a;
    instance[kSizeB] = item.size_b;
    ++restored;
    if (std::find(touched.begin(), touched.end(), item.node) == touched.end()) {
      touched.push_back(item.node);
    }
  }
  for (std::uint8_t* node : touched) {
    changed(node);
  }
  if (log_line) {
    log_line("GRASS show %u: %d instance(s) back in %zu cell(s)", id, restored, touched.size());
  }
  return restored;
}

std::vector<std::pair<int, int>> kinds_near(float x, float y, float radius) {
  std::vector<std::pair<int, int>> found;
  if (!ready() || !(radius > 0.0f)) {
    return found;
  }
  float box[6];
  circle_box(x, y, radius, box);
  for (std::uint8_t* node : nodes(box)) {
    for (std::uint32_t header = 0; header < header_count(node); ++header) {
      std::uint8_t* group = header_of(node, header);
      std::uint32_t count = 0;
      std::uint8_t* instances = instances_of(group, count);
      int within_circle = 0;
      for (std::uint32_t index = 0; instances && index < count; ++index) {
        std::uint8_t* instance = instances + std::size_t(index) * kInstanceSize;
        float place[3];
        place_of(node, instance, place);
        const float dx = place[0] - x;
        const float dy = place[1] - y;
        if (dx * dx + dy * dy <= radius * radius &&
            (instance[kSizeA] != 0 || instance[kSizeB] != 0)) {
          ++within_circle;
        }
      }
      if (within_circle == 0) {
        continue;
      }
      const int kind = field<int>(group, kHeaderKind);
      auto same = std::find_if(found.begin(), found.end(), [&](const std::pair<int, int>& entry) {
        return entry.first == kind;
      });
      if (same == found.end()) {
        found.emplace_back(kind, within_circle);
      } else {
        same->second += within_circle;
      }
    }
  }
  return found;
}

void frame() {
  if (!ready()) {
    return;
  }
  // The cells that were loading, once ready (each still there), a few a frame.
  constexpr std::size_t kWaitingPerFrame = 16;
  std::size_t looked = 0;
  for (std::size_t index = 0; index < waiting.size() && looked < kWaitingPerFrame; ++looked) {
    const Waiting entry = waiting[index];
    const std::vector<std::uint8_t*> there = nodes(entry.box.data());
    if (std::find(there.begin(), there.end(), entry.node) == there.end()) {
      waiting.erase(waiting.begin() + std::ptrdiff_t(index));
      continue;
    }
    if (field<std::uint32_t>(entry.node, kChanged) == kReadyState) {
      waiting.erase(waiting.begin() + std::ptrdiff_t(index));
      changed(entry.node);
      continue;
    }
    ++index;
  }
  if (to_prepare.empty()) {
    return;
  }
  // The shaped ground's cells load again (the decode hook sets their grass on it), a few a frame:
  // an opened project replays all its edits at once.
  std::vector<std::uint8_t*> cells;
  while (!to_prepare.empty() && cells.size() < kCellsPerFrame) {
    for (std::uint8_t* node : nodes(to_prepare.back().data())) {
      if (std::find(cells.begin(), cells.end(), node) == cells.end()) {
        cells.push_back(node);
      }
    }
    to_prepare.pop_back();
  }
  for (std::uint8_t* node : cells) {
    changed(node);
  }
}

void reset() {
  editing_now = false;
  to_prepare.clear();
  waiting.clear();
  std::lock_guard<std::recursive_mutex> lock(hidden_mutex);
  hidden.clear();
  shaped.clear();
}

}  // namespace kcds::runtime::terrain::grass
