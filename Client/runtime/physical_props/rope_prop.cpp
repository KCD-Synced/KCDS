// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "rope_prop.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../native_ui/native_ui.hpp"

namespace kcds::runtime::physical_props {
namespace {

using game_client::slot;

// I3DEngine (see the header).
constexpr std::size_t kCreateRenderNode = 156;
constexpr int kRopeType = 15;
constexpr std::size_t kMaterialManager = 197;
constexpr std::size_t kLoadMaterial = 7;
// CRopeRenderNode.
constexpr std::size_t kSetMatrix = 16;
constexpr std::size_t kDephysicalize = 48;
constexpr std::size_t kHide = 68;
constexpr std::size_t kSetParams = 74;
constexpr std::size_t kGetParams = 75;
constexpr std::size_t kSetPoints = 76;
constexpr std::size_t kNodeMaterial = 232;
constexpr std::size_t kPhysics = 352;
// The node's parameter block (+ 592), 144 bytes, and the fields this sets in it.
constexpr std::size_t kBlockSize = 144;
// The block's fields by their names (the rope's serializer): + 0 "flags", + 4
// "radius", + 12 "anchor_radius", + 16 "num_seg", + 20 "num_sides", + 24 "texu", + 28 "texv",
// + 60 "ph_num_seg". The radius is half the thickness across (it was given the whole thickness
// before: every rope was drawn twice as thick, the whip's handle 52 mm).
constexpr std::size_t kBlockRadius = 4;
// How many sides its tube is drawn with (4 by default; four sides with the rope's
// twisted texture looked like a flat ribbon on a lash).
constexpr std::size_t kBlockSides = 20;
constexpr std::size_t kBlockPhysicsSegments = 60;
constexpr std::size_t kBlockMass = 68;
constexpr std::size_t kBlockFriction = 76;
constexpr std::size_t kBlockAirResistance = 100;
// A material's reference count.
constexpr std::size_t kAddRef = 2;
constexpr std::size_t kRelease = 3;
constexpr const char* kRopeMaterial =
    "Objects/manmade/task_specific_props/ropemaking/rope_editor_tool";
// CRopeEntity SetParams (slot 4) with pe_params_rope (type 13), by 4-byte index.
constexpr std::size_t kPhysicsSetParams = 4;
constexpr std::size_t kPhysicsAction = 7;
constexpr std::int32_t kRopeParams = 13;
constexpr std::uint32_t kActionAwake = 8;
constexpr std::size_t kRopeParamsWords = 64;
constexpr std::uint32_t kUnusedFloat = 0xFFBFFFFF;
constexpr std::uint32_t kUnusedInt = 0x80000000;
constexpr std::size_t kIntIndices[] = {4, 11, 28, 29, 30, 31, 32, 44, 48, 61, 62};
constexpr std::size_t kPointsPointer = 42;  // pPoints, 2 words
constexpr std::size_t kVelocitiesPointer = 46;
// GetParams (slot 5); nSegments, and the strides of pPoints and pVelocities.
constexpr std::size_t kPhysicsGetParams = 5;
constexpr std::size_t kSegmentCount = 29;
constexpr std::size_t kPointsStride = 44;
constexpr std::size_t kVelocitiesStride = 48;
constexpr int kMostPoints = 65;
constexpr std::size_t kTiedEntities = 50;   // pEntTiedTo[2], 4 words
constexpr std::size_t kLocalTiePoint = 54;  // 0: ptTiedTo is in the world
constexpr std::size_t kTiePoints = 55;      // ptTiedTo[2], 6 words
constexpr std::intptr_t kTiedToWorld = -10;
// pe_simulation_params (type 10) and the rope's own settings in pe_params_rope (type 13).
constexpr std::int32_t kSimulationParams = 10;
constexpr std::uint32_t kFlagsParams = 15;
constexpr std::size_t kSimulationWords = 32;
constexpr std::size_t kSimulationIntIndices[] = {1, 15, 16, 18};
constexpr std::size_t kSimulationDamping = 4;
constexpr std::size_t kSimulationGravity = 5;
constexpr std::size_t kRopeCollisionDistance = 3;
constexpr std::size_t kRopeFriction = 5;
constexpr std::size_t kRopeStiffness = 7;
constexpr std::size_t kRopeMass = 2;
constexpr std::size_t kRopeWind = 12;
constexpr std::size_t kRopeWindVariance = 15;
constexpr std::size_t kRopeAirResistance = 16;
constexpr std::size_t kRopeJointLimit = 19;
constexpr float kDegreesToRadians = 3.14159265f / 180.0f;

// The settings every rope takes on (set_rope_physics) and their version.
std::mutex physics_mutex;
RopePhysics shared_physics;
std::atomic<unsigned> physics_version{1};

void put_float(std::uint32_t* words, std::size_t index, float value) {
  std::memcpy(&words[index], &value, sizeof(value));
}

// pe_params_rope with every field unused (ints and pointers too).
std::array<std::uint32_t, kRopeParamsWords> unused_rope_params() {
  std::array<std::uint32_t, kRopeParamsWords> words;
  words.fill(kUnusedFloat);
  words[0] = kRopeParams;
  for (const std::size_t index : kIntIndices) {
    words[index] = kUnusedInt;
  }
  const std::intptr_t unused_pointer = -1;
  std::memcpy(&words[kPointsPointer], &unused_pointer, sizeof(unused_pointer));
  std::memcpy(&words[kVelocitiesPointer], &unused_pointer, sizeof(unused_pointer));
  std::memcpy(&words[kTiedEntities], &unused_pointer, sizeof(unused_pointer));
  std::memcpy(&words[kTiedEntities + 2], &unused_pointer, sizeof(unused_pointer));
  return words;
}

using CreateRenderNode = void*(__fastcall*)(void* engine, int type);
using SetMatrix = void(__fastcall*)(void* node, const float* matrix);
using GetParams = const std::uint8_t*(__fastcall*)(void* node);
using SetParams = void(__fastcall*)(void* node, const std::uint8_t* block);
using SetPoints = void(__fastcall*)(void* node, const float* points, int count);
using Hide = void(__fastcall*)(void* node, bool hide);
using Dephysicalize = void(__fastcall*)(void* node, bool keep);
using PhysicsCall = int(__fastcall*)(void* entity, const void* params, int thread_safe);
using GetManager = void*(__fastcall*)(void* engine);
using LoadMaterial = void*(__fastcall*)(void* manager, const char* name, bool make, bool fixed,
                                        unsigned flags, bool load);
using Counted = void(__fastcall*)(void* object);

template <class T>
void put(std::uint8_t* block, std::size_t at, T value) {
  std::memcpy(block + at, &value, sizeof(value));
}

// The game's rope material on the node, in place of the default one.
void dress(void* engine, void* node, const char* name) {
  void* manager = slot<GetManager>(engine, kMaterialManager)(engine);
  void* material = nullptr;
  if (manager) {
    auto load = slot<LoadMaterial>(manager, kLoadMaterial);
    material = load(manager, name, false, false, 0, true);
  }
  if (!material) {
    game_client::log("ROPE material %s not found", name);
    return;
  }
  void*& kept = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node) + kNodeMaterial);
  if (kept == material) {
    return;
  }
  void* old = kept;
  kept = material;
  slot<Counted>(material, kAddRef)(material);
  if (old) {
    slot<Counted>(old, kRelease)(old);
  }
}

// The chain's material (kChainMaterial), mounted from memory once (native_ui mount_files).
void mount_chain_material() {
  static bool mounted = false;
  if (mounted) {
    return;
  }
  const std::string text =
      "<Material MtlFlags=\"524544\" vertModifType=\"0\">\n"
      "  <SubMaterials>\n"
      "    <Material Name=\"chain\" MtlFlags=\"524416\" Shader=\"Illum\" "
      "GenMask=\"20000012002000\" StringGenMask=\"%NORMAL_MAP%SPECULAR_MAP\" "
      "SurfaceType=\"mat_metal\" Diffuse=\"1,1,1\" Specular=\"1,1,1\" Opacity=\"1\" "
      "Shininess=\"230\" vertModifType=\"0\" LayerAct=\"1\" CustomSortPriority=\"1\">\n"
      "      <Textures>\n"
      "        <Texture Map=\"Diffuse\" "
      "File=\"objects/intermediates/shared/textures/chain_a_diff.dds\">\n"
      "          <TexMod RotateW=\"90\" TexMod_bTexGenProjected=\"0\" TexMod_RotateType=\"1\" "
      "TexMod_TexGenType=\"0\" TileV=\"4\" />\n"
      "        </Texture>\n"
      "        <Texture Map=\"Bumpmap\" "
      "File=\"objects/intermediates/shared/textures/chain_a_ddna.dds\" />\n"
      "        <Texture Map=\"Specular\" "
      "File=\"objects/intermediates/shared/textures/chain_a_spec.dds\" />\n"
      "      </Textures>\n"
      "    </Material>\n"
      "  </SubMaterials>\n"
      "</Material>\n";
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  entries.emplace_back("kcds_props/chain.mtl", std::vector<std::uint8_t>(text.begin(), text.end()));
  mounted = native_ui::mount_files("_kcds/props/chain", "objects/", entries);
  game_client::log("ROPE chain material %s", mounted ? "mounted" : "could not be mounted");
}

}  // namespace

void set_rope_physics(const RopePhysics& physics) {
  std::lock_guard<std::mutex> lock(physics_mutex);
  shared_physics = physics;
  physics_version.fetch_add(1);
}

RopePhysics rope_physics() {
  std::lock_guard<std::mutex> lock(physics_mutex);
  return shared_physics;
}

void RopeProp::set_own_physics(const RopePhysics& physics) {
  const bool same =
      own_physics_ && own_physics_->gravity == physics.gravity &&
      own_physics_->damping == physics.damping && own_physics_->mass == physics.mass &&
      own_physics_->air_resistance == physics.air_resistance &&
      own_physics_->bend_limit == physics.bend_limit && own_physics_->wind == physics.wind &&
      own_physics_->stiffness == physics.stiffness && own_physics_->friction == physics.friction &&
      own_physics_->thickness == physics.thickness && own_physics_->collide == physics.collide;
  if (!same) {
    own_physics_ = physics;
    ++own_version_;
  }
}

void RopeProp::apply_physics(void* physics) {
  RopePhysics settings;
  if (own_physics_) {
    settings = *own_physics_;
  } else {
    std::lock_guard<std::mutex> lock(physics_mutex);
    settings = shared_physics;
  }
  std::array<std::uint32_t, kSimulationWords> simulation;
  simulation.fill(kUnusedFloat);
  simulation[0] = kSimulationParams;
  for (const std::size_t index : kSimulationIntIndices) {
    simulation[index] = kUnusedInt;
  }
  put_float(simulation.data(), kSimulationDamping, settings.damping);
  put_float(simulation.data(), kSimulationGravity, 0.0f);
  put_float(simulation.data(), kSimulationGravity + 1, 0.0f);
  put_float(simulation.data(), kSimulationGravity + 2, -settings.gravity);
  slot<PhysicsCall>(physics, kPhysicsSetParams)(physics, simulation.data(), 0);
  auto rope = unused_rope_params();
  put_float(rope.data(), kRopeMass, settings.mass);
  put_float(rope.data(), kRopeAirResistance, settings.air_resistance);
  put_float(rope.data(), kRopeJointLimit, settings.bend_limit * kDegreesToRadians);
  put_float(rope.data(), kRopeWind, settings.wind);
  put_float(rope.data(), kRopeWind + 1, 0.0f);
  put_float(rope.data(), kRopeWind + 2, 0.0f);
  put_float(rope.data(), kRopeWindVariance, settings.wind * 0.3f);
  if (settings.stiffness > 0.0f) {
    put_float(rope.data(), kRopeStiffness, settings.stiffness);
  }
  put_float(rope.data(), kRopeFriction, settings.friction);
  put_float(rope.data(), kRopeCollisionDistance, settings.thickness * 0.5f);
  slot<PhysicsCall>(physics, kPhysicsSetParams)(physics, rope.data(), 0);
  // Collisions: pe_params_flags (type 15: flags [1], flagsOR [2], flagsAND [3],
  // unused 0x80000000, the physical entity's shared SetParams) turns rope_collides
  // and rope_collides_with_terrain (0x200000 | 0x400000, CRopeEntity::SetParams) on or off.
  const std::uint32_t with_bodies = 0x200000u;
  const std::uint32_t with_ground = 0x400000u;
  const std::uint32_t collides = with_bodies | with_ground;
  // The start's speed from how far its tie point moved (rope_findiff_attached_vel, 0x01):
  // CRopeEntity::Step then gives the tied vertex (point now - point before) / step
  // time; without it a rope tied to the world takes the world's speed, zero, and a lash moved by
  // the hand drags along instead of carrying the swing (its tip never outran the hand).
  // The game zeroes speeds above 0.3 x the world's top speed (world +352) as a teleport.
  const std::uint32_t moving_tie = 0x01u;
  std::uint32_t flags[4] = {kFlagsParams, kUnusedInt, kUnusedInt, kUnusedInt};
  if (settings.collide == 1) {
    flags[2] = collides | moving_tie;
  } else if (settings.collide == 2) {
    flags[2] = with_ground | moving_tie;
    flags[3] = ~with_bodies;
  } else {
    flags[2] = moving_tie;
    flags[3] = ~collides;
  }
  slot<PhysicsCall>(physics, kPhysicsSetParams)(physics, flags, 0);
}

RopeProp::~RopeProp() {
  remove();
}

bool RopeProp::make(int segments, float length, const float start[3]) {
  return make(segments, length, start, nullptr);
}

bool RopeProp::make(int segments, float length, const float start[3], const char* material) {
  return make(segments, length, start, material, 0.02f);
}

bool RopeProp::make(int segments, float length, const float start[3], const char* material,
                    float thickness) {
  return make(segments, length, start, material, thickness, 0);
}

bool RopeProp::make(int segments, float length, const float start[3], const char* material,
                    float thickness, int sides) {
  remove();
  material_ = material ? material : kRopeMaterial;
  if (material_ == kChainMaterial) {
    mount_chain_material();
  }
  void* engine = game_client::engine_3d_slot ? *game_client::engine_3d_slot : nullptr;
  if (!engine) {
    return false;
  }
  node_ = slot<CreateRenderNode>(engine, kCreateRenderNode)(engine, kRopeType);
  if (!node_) {
    game_client::log("ROPE: the engine made no rope node");
    return false;
  }
  const float matrix[12] = {1, 0, 0, start[0], 0, 1, 0, start[1], 0, 0, 1, start[2]};
  slot<SetMatrix>(node_, kSetMatrix)(node_, matrix);
  dress(engine, node_, material_.c_str());
  std::array<std::uint8_t, kBlockSize> block{};
  std::memcpy(block.data(), slot<GetParams>(node_, kGetParams)(node_), kBlockSize);
  put(block.data(), kBlockRadius, 0.5f * thickness);
  if (sides >= 3) {
    put(block.data(), kBlockSides, std::min(sides, 16));
  }
  put(block.data(), kBlockPhysicsSegments, segments);
  put(block.data(), kBlockMass, 0.5f);
  put(block.data(), kBlockFriction, 0.5f);
  put(block.data(), kBlockAirResistance, 0.1f);
  slot<SetParams>(node_, kSetParams)(node_, block.data());
  // Local to the matrix: from the start straight down.
  const float points[6] = {0, 0, 0, 0, 0, -length};
  slot<SetPoints>(node_, kSetPoints)(node_, points, 2);
  segments_ = segments;
  length_ = length;
  return true;
}

void RopeProp::hold(const float start[3]) {
  hold(start, nullptr);
}

void RopeProp::hold(const float start[3], const float* end) {
  if (!node_) {
    return;
  }
  void* physics = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node_) + kPhysics);
  if (!physics) {
    return;
  }
  // The settings when they changed or the rope has a new physical entity: its own ones count
  // apart from the shared ones (an own change is told by the high bit).
  const unsigned version = own_physics_ ? (own_version_ | 0x80000000u) : physics_version.load();
  if (version != applied_version_ || physics != applied_to_) {
    apply_physics(physics);
    applied_version_ = version;
    applied_to_ = physics;
  }
  auto words = unused_rope_params();
  std::memcpy(&words[kTiedEntities], &kTiedToWorld, sizeof(kTiedToWorld));
  words[kLocalTiePoint] = 0;
  std::memcpy(&words[kTiePoints], start, sizeof(float) * 3);
  // The end: tied to the world at its point, or untied once it is let go (a null
  // pEntTiedTo[1] unties, CRopeEntity::SetParams; -1 would leave it as it is).
  if (end) {
    std::memcpy(&words[kTiedEntities + 2], &kTiedToWorld, sizeof(kTiedToWorld));
    std::memcpy(&words[kTiePoints + 3], end, sizeof(float) * 3);
    end_tied_ = true;
  } else if (end_tied_) {
    const std::intptr_t untied = 0;
    std::memcpy(&words[kTiedEntities + 2], &untied, sizeof(untied));
    end_tied_ = false;
  }
  slot<PhysicsCall>(physics, kPhysicsSetParams)(physics, words.data(), 0);
  const std::uint32_t awake[3] = {kActionAwake, 1, kUnusedFloat};
  slot<PhysicsCall>(physics, kPhysicsAction)(physics, awake, 0);
}

std::vector<float> RopeProp::points() const {
  std::vector<float> out;
  if (!node_) {
    return out;
  }
  void* physics = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node_) + kPhysics);
  if (!physics) {
    return out;
  }
  auto words = unused_rope_params();
  if (!slot<PhysicsCall>(physics, kPhysicsGetParams)(physics, words.data(), 0)) {
    return out;
  }
  const int count = std::int32_t(words[kSegmentCount]) + 1;
  const std::int32_t stride = std::int32_t(words[kPointsStride]);
  const std::uint8_t* first = nullptr;
  std::memcpy(&first, &words[kPointsPointer], sizeof(first));
  if (!first || count < 2 || count > kMostPoints || stride < 12) {
    return out;
  }
  out.resize(std::size_t(count) * 3);
  for (int index = 0; index < count; ++index) {
    std::memcpy(&out[std::size_t(index) * 3], first + std::size_t(index) * stride,
                sizeof(float) * 3);
  }
  return out;
}

bool RopeProp::set_points(const std::vector<float>& points) {
  // As many points as the rope has now (the game's own count, which the copy runs over).
  if (!node_ || points.empty() || points.size() != this->points().size()) {
    return false;
  }
  void* physics = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node_) + kPhysics);
  if (!physics) {
    return false;
  }
  const std::vector<float> still(points.size(), 0.0f);
  auto words = unused_rope_params();
  const float* place = points.data();
  const float* velocity = still.data();
  std::memcpy(&words[kPointsPointer], &place, sizeof(place));
  words[kPointsStride] = sizeof(float) * 3;
  std::memcpy(&words[kVelocitiesPointer], &velocity, sizeof(velocity));
  words[kVelocitiesStride] = sizeof(float) * 3;
  slot<PhysicsCall>(physics, kPhysicsSetParams)(physics, words.data(), 0);
  return true;
}

void RopeProp::remove() {
  if (!node_) {
    return;
  }
  slot<Dephysicalize>(node_, kDephysicalize)(node_, false);
  slot<Hide>(node_, kHide)(node_, true);
  node_ = nullptr;
  segments_ = 0;
  length_ = 0.0f;
  applied_to_ = nullptr;
  end_tied_ = false;
}

}  // namespace kcds::runtime::physical_props
