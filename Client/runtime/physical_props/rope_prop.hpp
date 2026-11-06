// A rope through the game's own rope (Asset/Animation Studio): drawn and
// simulated by the game, its start tied to a point that moves (a hand, a whip's tip).
//
// WHGame 1.5.5:
//   - the 3D engine's CreateRenderNode (I3DEngine slot 156) makes a CRopeRenderNode
//     for type 15 (848 bytes). The node draws the rope and makes its physics
//     itself: slot 16 SetMatrix, 74 SetParams (a block at + 592), 75 GetParams, 76 SetPoints
//     (points local to the matrix; registers the node with the 3D engine and physicalizes it,
// a physical rope, type 7, when the block's mass is above 0), 48 Dephysicalize,
//     68 Hide;
//   - the block (+ 592), as Physicalize copies it into the physics' pe_params_rope: + 4 radius,
//     + 60 physics segments, + 68 mass, + 76 friction, + 100 air resistance;
//   - its material: a smart pointer at + 232 (the constructor, puts the default one
//     there), set as the game's smart pointer does (AddRef slot 2, Release slot 3),
//     with the game's material of its editor's rope tool from the material manager (I3DEngine
//     slot 197, LoadMaterial slot 7);
//   - the node's physical entity at + 352 is a CRopeEntity; its SetParams
//     (slot 4) takes pe_params_rope (type 13, the CryEngine 5 layout): the start tied
//     to the world (pEntTiedTo[0] -10, bytes 200) at a point (ptTiedTo[0], bytes 220). Unused
//     floats are 0xFFBFFFFF, unused ints 0x80000000, unused pointers -1. It is woken each time
//     (pe_action_awake, type 8, through Action, slot 7): asleep it does not move.
//
//
// How the ropes move (set_rope_physics, every rope and whip lash; the studio's Physics details):
//   - pe_simulation_params (type 10, SetParams' own reading): damping [4],
//     gravity [5..7] (also the free-fall gravity);
//   - pe_params_rope (type 13): collision distance [3], mass [2], friction [5], stiffness [7],
//     wind [12..14], wind variance [15], air resistance [16], joint limit [19] (how far a joint
//     may bend, radians; 0 for any).
//
// Engine thread, but for set_rope_physics (any thread).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::physical_props {

struct RopePhysics {
  // m/s², downwards.
  float gravity = 9.81f;
  // How fast its swinging dies down.
  float damping = 0.3f;
  // kg, the whole rope.
  float mass = 0.5f;
  // Drag in the air.
  float air_resistance = 0.1f;
  // Degrees a joint may bend; 0 for any (a limp rope).
  float bend_limit = 0.0f;
  // m/s along the world's x.
  float wind = 0.0f;
  // How hard it resists being stretched (pe_params_rope stiffness [7]); 0 for the game's own.
  float stiffness = 0.0f;
  // How it slides on what it lies on (friction [5]).
  float friction = 0.5f;
  // Metres across: it collides this far round its line (collision distance [3], half of it).
  float thickness = 0.02f;
  // What it collides with (pe_params_flags rope_collides, rope_collides_with_terrain): 1 bodies,
  // props and the ground, 2 the ground only (a lash then swings past the body holding it
  // instead of wrapping round it), 0 nothing (it falls through everything).
  int collide = 1;
  // A whip's lash only (whip_lash.hpp WhipLashSettings): how much it takes its posed shape
  // instead of moving by physics (0 physics, 1 the shape: no gravity), and that shape's bend
  // along its length, upwards (curl) and sideways (sway), degrees from the handle to the tip.
  float shape = 0.0f;
  float curl = 0.0f;
  float sway = 0.0f;
};

// A chain's look: a material of KCDS's own, mounted from memory the first time a
// chain is made (the game's chains_a.mtl starts with a NoDraw proxy, which a rope draws with):
// the game's chain textures (objects/intermediates/shared/textures/chain_a_diff and _ddna,
// Data/IPL_Objects-part0.pak) mapped along the rope as its own rope material maps its texture.
inline constexpr const char* kChainMaterial = "objects/kcds_props/chain";

// The settings every rope takes on from its next hold().
void set_rope_physics(const RopePhysics& physics);

// The settings set_rope_physics gave last (the studio's Physics mode).
RopePhysics rope_physics();

class RopeProp {
 public:
  RopeProp() = default;
  RopeProp(const RopeProp&) = delete;
  RopeProp& operator=(const RopeProp&) = delete;
  ~RopeProp();

  // A rope of `segments` physics segments and `length` metres hanging from `start` (the one
  // there was goes first). False when the game made none.
  bool make(int segments, float length, const float start[3]);

  // The same drawn with another of the game's materials (its path without the extension), such
  // as a chain's; null for the rope's own.
  bool make(int segments, float length, const float start[3], const char* material);

  // The same `thickness` metres across as drawn (2 cm for the others; a whip's lash is thinner).
  bool make(int segments, float length, const float start[3], const char* material,
            float thickness);

  // The same drawn as a tube of `sides` sides (3 to 16; the game's own 4 below 3): a whip's
  // leather is round, eight sides.
  bool make(int segments, float length, const float start[3], const char* material, float thickness,
            int sides);

  // Ties its start to this world point and wakes it; nothing without a rope.
  void hold(const float start[3]);

  // The same, its end also tied to the world point `end`; null lets a tied end go.
  void hold(const float start[3], const float* end);

  // Its points now, in the world, start first (segments + 1 of them, x y z each), as the game
  // simulates them: CRopeEntity::GetParams (slot 5) for pe_params_rope gives
  // nSegments (bytes 116) and pPoints (168) with its stride (176), the rope's own segments (152
  // bytes each, the place first). Empty without a rope. Recording a simulation into keys.
  std::vector<float> points() const;

  // Puts its points there (in the world, as many as points() gives) and stops them (velocities
  // 0): CRopeEntity::SetParams copies pPoints (bytes 168, stride 176) and
  // pVelocities (184, stride 192) into its segments, nSegments (116) left unused so its length
  // stays. False without a rope or with another number of points. Playing keys back.
  bool set_points(const std::vector<float>& points);

  // Its own settings in place of the shared ones (set_rope_physics) from its next hold(): a
  // player's prop on a server takes the settings it was given. Unchanged settings
  // are not put on it again.
  void set_own_physics(const RopePhysics& physics);

  // Takes it away.
  void remove();

  bool made() const {
    return node_ != nullptr;
  }

  int segments() const {
    return segments_;
  }

  float length() const {
    return length_;
  }

 private:
  // The settings (its own, else set_rope_physics's) on its physical entity.
  void apply_physics(void* physics);

  void* node_ = nullptr;
  // Its own settings (set_own_physics) and how many times they changed.
  std::optional<RopePhysics> own_physics_;
  unsigned own_version_ = 0;
  // The material it is drawn with.
  std::string material_;
  int segments_ = 0;
  float length_ = 0.0f;
  // The settings' version and the physical entity they were put on.
  unsigned applied_version_ = 0;
  void* applied_to_ = nullptr;
  // Whether its end is tied (so letting it go unties it once).
  bool end_tied_ = false;
};

}  // namespace kcds::runtime::physical_props
