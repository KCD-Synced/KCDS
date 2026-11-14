#include "placed_carts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../props/prop_entity.hpp"
#include "cart_prefabs.hpp"

namespace kcds::runtime::carts::placed {
namespace {

using kcd2s::integration::CartKind;
using kcd2s::integration::CartOrder;
using kcd2s::integration::CartPace;
using kcd2s::integration::CartReport;
using kcd2s::integration::CartReportKind;
using kcd2s::integration::CartSeat;

constexpr std::size_t kSeatCount = kcd2s::integration::kCartSeatNames.size();
constexpr float kPi = 3.14159265f;

// The driver's reports: at most this often, and only when the cart moved or turned this much or
// its pace changed.
constexpr double kReportSeconds = 0.1;
constexpr float kReportMeters = 0.2f;
constexpr float kReportRadians = 0.05f;
// Below this speed (metres a second) the cart stands; up to the second it walks, above it trots.
constexpr float kStandingSpeed = 0.3f;
constexpr float kWalkingSpeed = 2.5f;
// How fast another driver's cart closes on the server's pose (share per second).
constexpr float kFollowRate = 8.0f;
// A passenger let down without a climb stands this far beside their seat.
constexpr float kStepDownMeters = 1.6f;
// A body climbing on walks to its clip's start (start_walk). Within the near distance of a point of
// its way it goes on to the next. The last bit it goes in this time (as the game's own
// PositionAdjustAlignBone step of these clips brings a body to its place); one that got stuck goes
// the rest in the longer time. From further than the far distance (a script seated them) it is put
// there.
constexpr float kWalkNear = 0.3f;
// How long at least a body that has climbed on takes to come to rest on its seat from where its
// clip ended (a longer way along the bench goes at kBenchMoveSpeed).
constexpr float kSettleSeconds = 0.5f;
constexpr float kClimbApproachSeconds = 0.25f;
constexpr float kClimbApproachStuckSeconds = 1.0f;
constexpr float kClimbApproachStuck = 0.5f;
constexpr float kClimbApproachFarthest = 25.0f;
// How long a walk may take, and how long the body may gain nothing before it counts as stuck.
constexpr double kWalkLongest = 14.0;
constexpr double kWalkStuckSeconds = 1.5;
constexpr float kWalkStuckMeters = 0.1f;
// How fast the walking body turns (degrees a second), and from how far off its way it stops to
// turn.
constexpr float kWalkTurnRate = 240.0f;
constexpr float kWalkTurnFirst = 0.7f;
// The way round a cart (metres from its middle and behind its origin).
constexpr float kWalkRoundSide = 1.5f;
constexpr float kWalkRoundBack = -1.3f;
// The seated driver's paces (metres a second): a walk, a trot with the sprint up to the cart's
// top speed, and a slower backing up.
constexpr float kDriveWalkSpeed = 2.0f;
constexpr float kDriveTrotSpeed = 3.4f;
constexpr float kDriveReverseSpeed = 1.0f;
// How fast the seated driver's cart gains and loses speed (metres a second, each second).
constexpr float kDriveAcceleration = 1.5f;
constexpr float kDriveBraking = 3.0f;
// The steering: the cart turns as a wagon with its front axle turned at most this far (radians,
// a bend of about 9 m: at 0.6 rad the horses, 5 m ahead of the back axle, went sideways faster than
// forwards, live), and how fast the axle turns there and back (radians a second).
constexpr float kSteerRadians = 0.26f;
constexpr float kSteerInRate = 0.35f;
constexpr float kSteerOutRate = 0.6f;
// Below this speed (metres a second) a cart shows no steering of its own.
constexpr float kSteerShownSpeed = 0.3f;
// How far a harnessed horse looks into the bend, as a share of the front axle's turn: more and it
// walks into the pole, which is one mesh with the axles.
constexpr float kHorseSteerShare = 0.8f;
// A ray for the ground (the terrain) under the axles starts this high above the cart's height,
// and one under a horse this high above the cart's front.
constexpr float kGroundRayAbove = 2.0f;
constexpr float kHorseRayAbove = 1.5f;
// How quickly the horses' walking speed follows the cart's (share per second).
constexpr float kVelocityRate = 6.0f;
// The seated driver's cart looks this far past where it goes this frame for something in its way,
// at these heights and across this half width.
constexpr float kBlockMargin = 0.3f;
constexpr float kBlockHeights[] = {0.6f, 1.4f};
constexpr float kBlockHalfWidth = 0.8f;
// Horses pull a cart up or down no steeper ground than this under its axles (radians, 28
// degrees): led onto a rocky slope, the cart stood on end.
constexpr float kSteepestPitch = 0.5f;

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// A part of the cart: a mesh at an offset from the cart's origin (the prefab's frame: +y is the
// cart's front, z = 0 the ground), turned (x, y, z, w) and scaled. The first part is the root; a
// wheel rolls as the cart goes.
struct Part {
  std::string model;
  Vec3 offset;
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  Vec3 scale{1.0f, 1.0f, 1.0f};
  bool wheel = false;
};

// A seat: where its player's body is held and which way it faces from the cart's front (radians,
// to the left).
struct Seat {
  bool present = false;
  Vec3 offset;
  float yaw = 0.0f;
};

struct Blueprint {
  const char* name = "";
  std::vector<Part> parts;
  std::array<Seat, kSeatCount> seats;
  // How far ahead of the origin the driver of a cart without horses leads it.
  float hitch = 0.0f;
  // The driver walks (beside the horses, or at the hitch of a cart without them) rather than
  // sitting, and where they walk in the cart's frame.
  bool driver_walks = false;
  Vec3 lead;
  // Where its horses stand (on the ground, facing the cart's front); none for a cart without.
  std::vector<Vec3> horses;
  // Where its front and back wheels touch the ground along the cart (y), for its height and pitch.
  float front_axle = 0.0f;
  float back_axle = 0.0f;
  // How far ahead (its horses' heads) and behind (its back) it reaches along the cart (y).
  float front_reach = 0.0f;
  float back_reach = 0.0f;
};

// Everyone but the driver sits in one pose (carts/carts.cjs SEAT_POSES). Measured live: its hips
// show 0.27 m behind where the body is held and 0.58 m above it. The b wagons: the covered body's
// walls lean out, 0.57 m from the middle at hip height and 0.72 m at the head, the left one 0.05 m
// nearer, so a passenger's hips stay 0.15 m off them, and the two sides are staggered along the
// benches so knees do not meet. The front plank with its backrest lies 2.6 m to 2.9 m along the
// cart, 1.38 m up, the footboard 0.9 m up from 3.0 m: the driver's and the co-driver's hips are
// over the plank's middle (2.72 m). The front seats stay about 0.8 m apart: at 0.56 m and 0.7 m a
// player seated there often did not see the other, at 0.8 m they did (live; why is not known yet).
const std::array<Seat, kSeatCount> kWagonBSeats = {{
    {true, {-0.40f, 2.9f, 0.97f}, 0.0f},       // driver
    {true, {0.40f, 3.07f, 0.95f}, 0.0f},       // bench
    {true, {0.17f, 0.55f, 0.88f}, kPi / 2},    // rightBack
    {true, {-0.13f, 0.85f, 0.88f}, -kPi / 2},  // leftBack
    {true, {0.17f, 1.85f, 0.88f}, kPi / 2},    // rightFront
    {true, {-0.13f, 2.15f, 0.88f}, -kPi / 2},  // leftFront
    {true, {0.17f, 1.2f, 0.88f}, kPi / 2},     // rightMiddle
    {true, {-0.13f, 1.5f, 0.88f}, -kPi / 2},   // leftMiddle
}};

// The open b body (wagon_b_body, wagon_b_body_empty): the front plank, and two shelves at the
// back of its sides (about 1.3 m up from 0.2 m to 0.9 m along it, seen live: a passenger at the
// covered body's places sat in its side) for two passengers facing across.
const std::array<Seat, kSeatCount> kOpenBSeats = {{
    {true, {-0.40f, 2.9f, 0.97f}, 0.0f},       // driver
    {true, {0.40f, 3.07f, 0.95f}, 0.0f},       // bench
    {true, {0.20f, 0.55f, 0.83f}, kPi / 2},    // rightBack
    {true, {-0.20f, 0.55f, 0.86f}, -kPi / 2},  // leftBack
    {false, {}, 0.0f},                         // rightFront
    {false, {}, 0.0f},                         // leftFront
    {false, {}, 0.0f},                         // rightMiddle
    {false, {}, 0.0f},                         // leftMiddle
}};

// The front plank only (an a body, a loaded wagon).
const std::array<Seat, kSeatCount> kFrontSeats = {{
    {true, {-0.40f, 2.9f, 0.97f}, 0.0f},  // driver
    {true, {0.40f, 3.07f, 0.95f}, 0.0f},  // bench
    {false, {}, 0.0f},                    // rightBack
    {false, {}, 0.0f},                    // leftBack
    {false, {}, 0.0f},                    // rightFront
    {false, {}, 0.0f},                    // leftFront
    {false, {}, 0.0f},                    // rightMiddle
    {false, {}, 0.0f},                    // leftMiddle
}};

// The a body's chest at the front (seen live): the driver and the co-driver side by side on it,
// their hips over its front half and their feet on the slats in front of it.
const std::array<Seat, kSeatCount> kChestSeats = {{
    {true, {-0.40f, 2.9f, 0.92f}, 0.0f},  // driver
    {true, {0.36f, 3.03f, 0.92f}, 0.0f},  // bench
    {false, {}, 0.0f},                    // rightBack
    {false, {}, 0.0f},                    // leftBack
    {false, {}, 0.0f},                    // rightFront
    {false, {}, 0.0f},                    // leftFront
    {false, {}, 0.0f},                    // rightMiddle
    {false, {}, 0.0f},                    // leftMiddle
}};

// A driver who walks (a c body, whose bed covers the axle's plank; a cart): nobody sits.
const std::array<Seat, kSeatCount> kWalkingSeats = {{
    {true, {}, 0.0f},   // driver
    {false, {}, 0.0f},  // bench
    {false, {}, 0.0f},  // rightBack
    {false, {}, 0.0f},  // leftBack
    {false, {}, 0.0f},  // rightFront
    {false, {}, 0.0f},  // leftFront
    {false, {}, 0.0f},  // rightMiddle
    {false, {}, 0.0f},  // leftMiddle
}};

// How each seat's climb moves the body (Libs/Tables/action/actor_anim_action.xml of its WagonIn and
// WagonOut in carts/carts.cjs CLIMB_ON and CLIMB_OFF: relative_movement): metres right, ahead and
// up of where it started, how far it turned to the left (radians) and how long it takes. The clip
// moves the body itself (0.92 m up without a cart).
struct Climb {
  float right = 0.0f;
  float ahead = 0.0f;
  float up = 0.0f;
  float turn = 0.0f;
  float seconds = 0.0f;
};
const std::array<Climb, kSeatCount> kClimbs = {{
    {0.365f, 1.325f, 0.921f, kPi / 2, 7.267f},    // driver: WagonIn driver
    {-0.511f, 1.014f, 0.921f, -kPi / 2, 5.233f},  // bench: player+codriver
    {0.187f, 1.333f, 0.921f, kPi / 2, 3.967f},    // rightBack: player+rightBack
    {-0.501f, 1.084f, 0.921f, -1.658f, 4.933f},   // leftBack
    {0.629f, 2.335f, 0.919f, 1.482f, 6.967f},     // rightFront
    {-0.493f, 2.226f, 0.918f, -kPi / 2, 8.0f},    // leftFront
    {0.629f, 2.335f, 0.919f, 1.482f, 6.967f},     // rightMiddle: rightFront
    {-0.493f, 2.226f, 0.918f, -kPi / 2, 8.0f},    // leftMiddle: leftFront
}};
// Where a bench's clip ends along the cart, its start being behind the cart's back, the place the
// clips start from (kBoardBackY): the game's own seats on these wagons. A rider whose seat lies
// elsewhere on the bench moves along it to their seat after climbing on, and back before climbing
// off (settle_left, scoot_left); the clips' own way must not be bent: placing the body while a clip
// moves it stops the clip's movement (a climb down stayed on the spot).
constexpr std::array<float, kSeatCount> kClipEndX = {0.0f,  0.0f,   0.167f, -0.61f,
                                                     0.40f, -0.51f, 0.40f,  -0.51f};
constexpr std::array<float, kSeatCount> kClipEndY = {0.0f, 0.0f,  0.55f, 0.26f,
                                                     1.6f, 1.45f, 1.6f,  1.45f};
// The one place everyone bound for the benches gets on: the middle of the back. From it the clips
// go up the middle of the floor, between the benches, and sit down at the end: moved to end on a
// seat instead, they walked through the other bench.
constexpr float kBoardBackX = -0.02f;
constexpr float kBoardBackY = -0.78f;
// A body on its way down reaches its clip's start in this time, while the clip has hardly begun.
constexpr float kClimbOffApproachSeconds = 0.4f;
// The floor of a b wagon lies as high above its ground as the clips rise (rays: 0.915 m against
// 0.921 m), so a climb starts at the height of the cart's own ground, not the terrain's there.
constexpr float kBenchMoveSpeed = 0.5f;
constexpr float kScootSeconds = 1.2f;

// The seats of a body; a loaded wagon keeps the front plank's only.
std::array<Seat, kSeatCount> seats_of(const prefabs::Prefab& prefab) {
  switch (prefab.body) {
    case prefabs::Body::CoveredB:
      return prefab.loaded ? kFrontSeats : kWagonBSeats;
    case prefabs::Body::OpenB:
      return prefab.loaded ? kFrontSeats : kOpenBSeats;
    case prefabs::Body::A:
      return kChestSeats;
    default:
      return kWalkingSeats;
  }
}

// The horses stand where the game's own wagon character (objects/manmade/vehicles/wagons/
// wagon_b_bench.cdf, the same wagon: its wheels 2.47 m apart) has its bones animal_horse_L and
// animal_horse_R, 1.30 m further ahead in this frame (its front wheels at 1.33 m, ours at 2.63 m).
// Every wagon prefab stands on the same axle place (Dev/tools/carts/build_prefabs.py moves it
// there), so the wagon chassis below fits them all.
Blueprint blueprint_from(const prefabs::Prefab& prefab) {
  Blueprint blueprint;
  blueprint.name = prefab.name;
  for (std::size_t i = 0; i < prefab.part_count; ++i) {
    const prefabs::Part& source = prefab.parts[i];
    Part part;
    part.model = source.model;
    part.offset = {source.position[0], source.position[1], source.position[2]};
    part.rotation = {source.rotation[0], source.rotation[1], source.rotation[2],
                     source.rotation[3]};
    part.scale = {source.scale[0], source.scale[1], source.scale[2]};
    part.wheel = source.wheel;
    blueprint.parts.push_back(part);
  }
  blueprint.seats = seats_of(prefab);
  if (prefab.chassis == prefabs::Chassis::Wagon) {
    // A walking driver goes before the left horse's head, the reins in hand (beside it the horse
    // stood in their way when they turned towards it, live).
    blueprint.driver_walks = prefab.body == prefabs::Body::C;
    blueprint.lead = {-0.8f, 6.9f, 0.0f};
    blueprint.hitch = 4.4f;
    blueprint.horses = {{-0.52f, 5.08f, 0.0f}, {0.50f, 5.06f, 0.0f}};
    blueprint.front_axle = 2.63f;
    blueprint.back_axle = 0.17f;
    blueprint.front_reach = 6.6f;
    blueprint.back_reach = -0.7f;
  } else {
    // Its one horse between the shafts (rays on the cart: its shafts 0.64 m to 0.75 m up from
    // 1.2 m to 2.4 m ahead, 0.37 m from the middle; its front board 1.43 m up at 0.95 m), its
    // rear before the front board; the cart rests on its wheels and the horse (the ground there
    // gives its pitch). Nobody sits on it: its driver leads the horse by the reins, walking
    // before its head.
    blueprint.driver_walks = true;
    blueprint.lead = {-0.4f, 3.9f, 0.0f};
    blueprint.hitch = 2.6f;
    blueprint.horses = {{0.0f, 2.3f, 0.0f}};
    blueprint.front_axle = 2.3f;
    blueprint.back_axle = -0.12f;
    blueprint.front_reach = 3.7f;
    blueprint.back_reach = -1.2f;
  }
  return blueprint;
}

// The game's carts and wagons (cart_prefabs.hpp), and cart_a (one mesh, kept for the scripts
// that spawn it).
const std::vector<Blueprint>& blueprints() {
  static const std::vector<Blueprint> list = [] {
    std::vector<Blueprint> made;
    for (const prefabs::Prefab& prefab : prefabs::kPrefabs) {
      made.push_back(blueprint_from(prefab));
    }
    Blueprint cart_a;
    cart_a.name = "cart_a";
    cart_a.parts = {{"objects/manmade/vehicles/carts/cart_a.cgf", {0.0f, 0.0f, 0.0f}}};
    cart_a.seats = {{
        {true, {0.0f, 1.0f, 0.9f}, 0.0f},     // driver
        {false, {}, 0.0f},                    // bench
        {true, {0.35f, -0.5f, 0.8f}, 0.0f},   // rightBack
        {true, {-0.35f, -0.5f, 0.8f}, 0.0f},  // leftBack
        {false, {}, 0.0f},                    // rightFront
        {false, {}, 0.0f},                    // leftFront
        {false, {}, 0.0f},                    // rightMiddle
        {false, {}, 0.0f},                    // leftMiddle
    }};
    cart_a.hitch = 2.6f;
    cart_a.driver_walks = true;
    cart_a.lead = {0.0f, 2.6f, 0.0f};
    cart_a.front_axle = 1.0f;
    cart_a.back_axle = 0.0f;
    cart_a.front_reach = 3.0f;
    cart_a.back_reach = -1.0f;
    made.push_back(cart_a);
    return made;
  }();
  return list;
}

const Blueprint* blueprint_of(const std::string& name) {
  for (const Blueprint& blueprint : blueprints()) {
    if (name == blueprint.name) {
      return &blueprint;
    }
  }
  return nullptr;
}

struct Cart {
  std::uint32_t id = 0;
  const Blueprint* blueprint = nullptr;
  // Where this game shows it (the origin and the heading about the up axis) and, for a cart
  // another client drives, where the server last said it is.
  Vec3 position;
  float yaw = 0.0f;
  Vec3 target;
  float target_yaw = 0.0f;
  float max_speed = 0.0f;
  CartPace pace = CartPace::Stand;
  std::uint32_t driver = 0;
  // Seat index -> player network id (0 when free).
  std::array<std::uint32_t, kSeatCount> riders{};
  // Seat index -> its rider has climbed on and is held there; not while they climb on or down.
  std::array<bool, kSeatCount> settled{};
  // Seat index -> its rider climbs down by their clip, so nothing puts them down at the end.
  std::array<bool, kSeatCount> climbing_off{};
  // The driver's hands hold the reins (their climb's clip has reached them, or they sit).
  bool reins_held = false;
  // The server took its horses out of the shafts (CART Harness with none); `looks` holds the
  // look of each harnessed horse, empty for the cart's own horses.
  bool unharnessed = false;
  std::vector<std::string> looks;
  // Seat index -> how far the rider's body still is from their seat as their climb has ended, for
  // that game's own body or replica: the offset fades in the seconds left, so they do not jump.
  std::array<Vec3, kSeatCount> settle_offset{};
  std::array<float, kSeatCount> settle_turn{};
  std::array<float, kSeatCount> settle_left{};
  // Seat index -> the offset is still to be taken from the body as it stands.
  std::array<bool, kSeatCount> settle_fresh{};
  std::array<float, kSeatCount> settle_seconds{};
  // Seat index -> the rider moves along the bench to where their climb down starts: seconds left,
  // and whether they do or have.
  std::array<float, kSeatCount> scoot_left{};
  std::array<bool, kSeatCount> scoot{};
  // The local player's walk to the start of their climb: the points still ahead (in the cart's
  // frame), since when it goes and when it last gained ground, and whether it holds the forward
  // key.
  std::vector<Vec3> walk;
  int walk_seat = -1;
  double walk_since = 0.0;
  double walk_gained_at = 0.0;
  float walk_best = 0.0f;
  bool walk_forward = false;
  // The server waits for the local player to stand at the start of their climb.
  bool climb_waits = false;
  // The local player's body stands at the start of their climb until the clip starts.
  bool hold_start = false;
  // The local player's body on its last bit to the start of their climb: seconds left, and how far
  // it still has to go each second.
  float approach_seconds = 0.0f;
  Vec3 approach_rate;
  float approach_turn_rate = 0.0f;
  // The driver's last report.
  double reported_at = 0.0;
  Vec3 reported;
  float reported_yaw = 0.0f;
  CartPace reported_pace = CartPace::Stand;
  // Its pitch (nose up, radians) on the ground under its axles.
  float pitch = 0.0f;
  // The seated driver's speed along the cart (metres a second, negative backing up).
  float speed = 0.0f;
  // Where it was shown last frame and how fast it goes and turns (for its horses' walk).
  Vec3 shown;
  float shown_yaw = 0.0f;
  // How far it has rolled along itself (metres), for its wheels.
  float rolled = 0.0f;
  // What it carries (CART Load; empty for nothing) and how many of the load's parts stand.
  std::string load;
  std::size_t load_parts = 0;
  Vec3 velocity;
  float turn_rate = 0.0f;
  // How far the seated driver has the front axle turned, and how far this game shows it turned,
  // from how the cart really goes, so that it reads the same in every game (radians, to the left).
  float steer = 0.0f;
  float shown_steer = 0.0f;
  // The seat the local player is held in (-1 for none), and the cart's heading when they were
  // last turned with it.
  int passenger_seat = -1;
};

struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

Natives natives;
bool installed = false;
// The cart's own walk is pressing a key (take_action lets it through).
bool pressing = false;

void press_key(const char* action, unsigned mode, float value) {
  pressing = true;
  natives.press(action, mode, value);
  pressing = false;
}
// Engine thread only: server cart id -> what it shows.
std::unordered_map<std::uint32_t, Cart> carts;
double last_frame = 0.0;

std::string part_name(std::uint32_t id, std::size_t part) {
  std::string name = "kcds_cart_" + std::to_string(id);
  if (part > 0) {
    name += "_" + std::to_string(part);
  }
  return name;
}

void log_line(const char* format, std::uint32_t id, const char* what) {
  if (natives.log) {
    natives.log(format, id, what);
  }
}

// The heading about the up axis of a rotation: the yaw of its forward (y) axis.
float yaw_of(const std::array<float, 4>& q) {
  const float fx = 2.0f * (q[0] * q[1] - q[3] * q[2]);
  const float fy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
  return std::atan2(-fx, fy);
}

float yaw_of(const float q[4]) {
  return yaw_of(std::array<float, 4>{q[0], q[1], q[2], q[3]});
}

void rotation_of(float yaw, float out[4]) {
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = std::sin(yaw * 0.5f);
  out[3] = std::cos(yaw * 0.5f);
}

// Rotations (x, y, z, w): a then b, the inverse of a turn, and a vector turned.
std::array<float, 4> multiply(const std::array<float, 4>& a, const std::array<float, 4>& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

std::array<float, 4> inverse(const std::array<float, 4>& q) {
  return {-q[0], -q[1], -q[2], q[3]};
}

Vec3 turned(const std::array<float, 4>& q, const Vec3& v) {
  const std::array<float, 4> p = multiply(multiply(q, {v.x, v.y, v.z, 0.0f}), inverse(q));
  return {p[0], p[1], p[2]};
}

// The shortest turn from `from` to `to`, -pi to pi.
float turn(float from, float to) {
  float d = std::fmod(to - from, 2.0f * kPi);
  if (d > kPi) {
    d -= 2.0f * kPi;
  }
  if (d < -kPi) {
    d += 2.0f * kPi;
  }
  return d;
}

// A point of the cart's frame in the world: pitched about its side axis, then turned.
Vec3 world_point(const Cart& cart, const Vec3& local) {
  const float pc = std::cos(cart.pitch);
  const float ps = std::sin(cart.pitch);
  const float ahead = local.y * pc - local.z * ps;
  const float up = local.y * ps + local.z * pc;
  const float s = std::sin(cart.yaw);
  const float c = std::cos(cart.yaw);
  return {cart.position.x + local.x * c - ahead * s, cart.position.y + local.x * s + ahead * c,
          cart.position.z + up};
}

// The cart's rotation: its heading, then its pitch about its own side (x) axis.
void cart_rotation(const Cart& cart, float out[4]) {
  const float sy = std::sin(cart.yaw * 0.5f);
  const float cy = std::cos(cart.yaw * 0.5f);
  const float sp = std::sin(cart.pitch * 0.5f);
  const float cp = std::cos(cart.pitch * 0.5f);
  out[0] = cy * sp;
  out[1] = sy * sp;
  out[2] = sy * cp;
  out[3] = cy * cp;
}

// The root part's rotation in the world: the cart's, then the part's own in the prefab.
void root_rotation(const Cart& cart, float out[4]) {
  float cart_turn[4];
  cart_rotation(cart, cart_turn);
  const std::array<float, 4> turn = multiply(
      {cart_turn[0], cart_turn[1], cart_turn[2], cart_turn[3]}, cart.blueprint->parts[0].rotation);
  out[0] = turn[0];
  out[1] = turn[1];
  out[2] = turn[2];
  out[3] = turn[3];
}

// A part's place and turn on the root (attached children are placed in its frame), a wheel turned
// by how far it rolled.
void part_on_root(const Part& root, const Part& part, float roll, float steer, float position[3],
                  float rotation[4]) {
  const std::array<float, 4> back = inverse(root.rotation);
  const Vec3 local = turned(back, {part.offset.x - root.offset.x, part.offset.y - root.offset.y,
                                   part.offset.z - root.offset.z});
  std::array<float, 4> turn = multiply(back, part.rotation);
  if (part.wheel && part.offset.z > 0.1f) {
    // Rolling ahead turns the wheel's top ahead: about its axle (x) the other way round.
    const float angle = -roll / part.offset.z;
    // A front wheel turns with the steering first, about the upright.
    turn = multiply({0.0f, 0.0f, std::sin(steer * 0.5f), std::cos(steer * 0.5f)}, turn);
    turn = multiply(turn, {std::sin(angle * 0.5f), 0.0f, 0.0f, std::cos(angle * 0.5f)});
  }
  position[0] = local.x;
  position[1] = local.y;
  position[2] = local.z;
  rotation[0] = turn[0];
  rotation[1] = turn[1];
  rotation[2] = turn[2];
  rotation[3] = turn[3];
}

bool harnessed(const Cart& cart) {
  return !cart.blueprint->horses.empty() && !cart.unharnessed;
}

// The driver sits on the front plank and drives with the keys (else they walk and lead it).
bool seated_driver(const Cart& cart) {
  return harnessed(cart) && !cart.blueprint->driver_walks;
}

// The local player is held on one of the cart's seats (a passenger, or a seated driver).
bool held_on(const Cart& cart, int seat) {
  return seat > 0 || (seat == 0 && seated_driver(cart));
}

Vec3 vec_of(const std::array<float, 3>& p) {
  return {p[0], p[1], p[2]};
}

int seat_of(const Cart& cart, std::uint32_t player) {
  for (std::size_t i = 0; i < kSeatCount; ++i) {
    if (player != 0 && cart.riders[i] == player) {
      return int(i);
    }
  }
  return -1;
}

// Moves the cart's root (and with it every part) to where the cart is shown.
void place(std::uint32_t id, const Cart& cart) {
  void* root = props::entity::find(part_name(id, 0));
  if (!root) {
    return;
  }
  const Vec3 at = world_point(cart, cart.blueprint->parts[0].offset);
  const float position[3] = {at.x, at.y, at.z};
  float rotation[4];
  root_rotation(cart, rotation);
  props::entity::place_rotated(root, position, rotation);
}

// The cart stands on the ground under its axles: its height and pitch from a ray under each.
void settle(Cart& cart) {
  if (!natives.ground_height) {
    return;
  }
  const Blueprint& blueprint = *cart.blueprint;
  const float s = std::sin(cart.yaw);
  const float c = std::cos(cart.yaw);
  const float above = cart.position.z + kGroundRayAbove;
  float front = 0.0f;
  float back = 0.0f;
  const bool found =
      natives.ground_height(cart.position.x - blueprint.front_axle * s,
                            cart.position.y + blueprint.front_axle * c, above, front) &&
      natives.ground_height(cart.position.x - blueprint.back_axle * s,
                            cart.position.y + blueprint.back_axle * c, above, back);
  if (!found) {
    return;
  }
  const float length = blueprint.front_axle - blueprint.back_axle;
  if (length < 0.1f) {
    cart.pitch = 0.0f;
    cart.position.z = back;
    return;
  }
  cart.pitch = std::atan2(front - back, length);
  cart.position.z = back - blueprint.back_axle * (front - back) / length;
}

std::string horse_name(std::uint32_t id, std::size_t horse) {
  return "kcds_cart_" + std::to_string(id) + "_horse_" + std::to_string(horse);
}

// How far the front axle reads turned in this game: from how the cart goes, as a wagon does.
void show_steering(Cart& cart, float dt) {
  const float ahead = -cart.velocity.x * std::sin(cart.yaw) + cart.velocity.y * std::cos(cart.yaw);
  float wanted = 0.0f;
  if (std::fabs(ahead) > kSteerShownSpeed) {
    const float wheelbase = std::max(1.0f, cart.blueprint->front_axle - cart.blueprint->back_axle);
    wanted = std::atan(cart.turn_rate * wheelbase / ahead);
    wanted = std::clamp(wanted, -kSteerRadians, kSteerRadians);
  }
  const float step = kSteerOutRate * dt;
  cart.shown_steer += std::clamp(wanted - cart.shown_steer, -step, step);
}

// The cart's horses stand on the ground in front of it, facing its front, and walk as it goes.
// Who holds the reins: a seated driver from when their climb's clip takes them until their climb
// down lets them go (a walking driver has no climb: they hold them from the start).
std::uint32_t reins_holder(const Cart& cart) {
  if (!seated_driver(cart)) {
    return cart.driver;
  }
  return cart.reins_held ? cart.driver : 0;
}

// Where free reins lie: on the front plank, where the driver's hands reach for them in their climb.
// Measured live: both hands at 0.65 of the driver's climb, on the front plank's top.
const Vec3 kReinsRestRight{-0.09f, 2.89f, 1.42f};
const Vec3 kReinsRestLeft{-0.09f, 2.89f, 1.42f};

void keep_horses(std::uint32_t id, Cart& cart, float dt) {
  if (dt > 0.0f) {
    const float share = std::min(1.0f, dt * kVelocityRate);
    const float vx = (cart.position.x - cart.shown.x) / dt;
    const float vy = (cart.position.y - cart.shown.y) / dt;
    cart.velocity.x += (vx - cart.velocity.x) * share;
    cart.velocity.y += (vy - cart.velocity.y) * share;
    cart.turn_rate += (turn(cart.shown_yaw, cart.yaw) / dt - cart.turn_rate) * share;
    show_steering(cart, dt);
  }
  cart.shown = cart.position;
  cart.shown_yaw = cart.yaw;
  if (!natives.keep_horse || !harnessed(cart)) {
    return;
  }
  const std::vector<Vec3>& team = cart.blueprint->horses;
  // A pair walks either side of the pole and looks into the bend; a horse in shafts goes with them.
  const float look = team.size() > 1 ? cart.shown_steer * kHorseSteerShare : 0.0f;
  float rotation[4];
  rotation_of(cart.yaw + look, rotation);
  const float velocity[3] = {cart.velocity.x, cart.velocity.y, 0.0f};
  const Vec3 front = world_point(cart, {0.0f, cart.blueprint->front_axle, 0.0f});
  const std::vector<Vec3>& horses = cart.blueprint->horses;
  for (std::size_t i = 0; i < horses.size(); ++i) {
    Vec3 at = world_point(cart, {horses[i].x, horses[i].y, 0.0f});
    float ground = at.z;
    if (natives.ground_height &&
        natives.ground_height(at.x, at.y, front.z + kHorseRayAbove, ground)) {
      at.z = ground;
    }
    const float position[3] = {at.x, at.y, at.z};
    // A pair's horse on the left (negative x) wears the left yoke, the other the right one.
    int side = 0;
    if (horses.size() > 1) {
      side = horses[i].x < 0.0f ? 1 : 2;
    }
    const Vec3 rest_right = world_point(cart, kReinsRestRight);
    const Vec3 rest_left = world_point(cart, kReinsRestLeft);
    const float rest[6] = {rest_right.x, rest_right.y, rest_right.z,
                           rest_left.x,  rest_left.y,  rest_left.z};
    const char* look = i < cart.looks.size() ? cart.looks[i].c_str() : "";
    natives.keep_horse(horse_name(id, i).c_str(), position, rotation, velocity, cart.turn_rate,
                       side, part_name(id, 0).c_str(), reins_holder(cart), rest, look);
  }
}

void remove_horses(std::uint32_t id, const Blueprint& blueprint) {
  if (!natives.remove_horse) {
    return;
  }
  for (std::size_t i = 0; i < blueprint.horses.size(); ++i) {
    natives.remove_horse(horse_name(id, i).c_str());
  }
}

std::string load_part_name(std::uint32_t id, std::size_t part) {
  return "kcds_cart_" + std::to_string(id) + "_load_" + std::to_string(part);
}

// The load a cart of this blueprint can carry by that name (cart_prefabs.hpp kLoads), or null.
const prefabs::Load* load_of(const Blueprint& blueprint, const std::string& name) {
  for (const prefabs::Load& load : prefabs::kLoads) {
    if (name == load.name && std::string(blueprint.name) == load.cart) {
      return &load;
    }
  }
  return nullptr;
}

void remove_load(std::uint32_t id, Cart& cart) {
  for (std::size_t i = 0; i < cart.load_parts; ++i) {
    props::entity::remove(load_part_name(id, i));
  }
  cart.load_parts = 0;
}

// What the cart carries (CART Load): the game's goods of that loaded cart, hung from its root as
// its own parts are; none for an empty name or one this cart cannot carry.
void show_load(std::uint32_t id, Cart& cart, const std::string& name) {
  remove_load(id, cart);
  cart.load = name;
  const prefabs::Load* load = name.empty() ? nullptr : load_of(*cart.blueprint, name);
  void* root = props::entity::find(part_name(id, 0));
  if (!load || !root) {
    if (!name.empty() && !load) {
      log_line("CART %u has no load %s", id, name.c_str());
    }
    return;
  }
  const AttachParams attach{0, nullptr};
  float root_position[3];
  props::entity::world_position(root, root_position);
  for (std::size_t i = 0; i < load->part_count; ++i) {
    const prefabs::Part& source = load->parts[i];
    Part part;
    part.model = source.model;
    part.offset = {source.position[0], source.position[1], source.position[2]};
    part.rotation = {source.rotation[0], source.rotation[1], source.rotation[2],
                     source.rotation[3]};
    part.scale = {source.scale[0], source.scale[1], source.scale[2]};
    void* child = props::entity::spawn_solid_mesh(load_part_name(id, i), root_position, part.model);
    if (!child) {
      continue;
    }
    cart.load_parts = i + 1;
    reinterpret_cast<AttachChild>(natives.attach_child)(root, child, &attach);
    float local[3];
    float turn[4];
    part_on_root(cart.blueprint->parts[0], part, 0.0f, 0.0f, local, turn);
    props::entity::place_rotated(child, local, turn);
    if (part.scale.x != 1.0f || part.scale.y != 1.0f || part.scale.z != 1.0f) {
      const float scale[3] = {part.scale.x, part.scale.y, part.scale.z};
      props::entity::set_scale(child, scale);
    }
    props::entity::physicalize(child, props::entity::Physics::Static, 0.0f);
  }
}

void remove_parts(std::uint32_t id, const Blueprint& blueprint) {
  for (std::size_t i = blueprint.parts.size(); i-- > 0;) {
    props::entity::remove(part_name(id, i));
  }
}

bool build(std::uint32_t id, const Cart& cart) {
  const Blueprint& blueprint = *cart.blueprint;
  remove_parts(id, blueprint);
  const Vec3 root_at = world_point(cart, blueprint.parts[0].offset);
  const float root_position[3] = {root_at.x, root_at.y, root_at.z};
  void* root =
      props::entity::spawn_solid_mesh(part_name(id, 0), root_position, blueprint.parts[0].model);
  if (!root) {
    return false;
  }
  float rotation[4];
  root_rotation(cart, rotation);
  props::entity::rotate(root, rotation);
  props::entity::physicalize(root, props::entity::Physics::Static, 0.0f);
  const AttachParams attach{0, nullptr};
  for (std::size_t i = 1; i < blueprint.parts.size(); ++i) {
    const Part& part = blueprint.parts[i];
    void* child = props::entity::spawn_solid_mesh(part_name(id, i), root_position, part.model);
    if (!child) {
      continue;
    }
    reinterpret_cast<AttachChild>(natives.attach_child)(root, child, &attach);
    float local[3];
    float turn[4];
    part_on_root(blueprint.parts[0], part, 0.0f, 0.0f, local, turn);
    props::entity::place_rotated(child, local, turn);
    if (part.scale.x != 1.0f || part.scale.y != 1.0f || part.scale.z != 1.0f) {
      const float scale[3] = {part.scale.x, part.scale.y, part.scale.z};
      props::entity::set_scale(child, scale);
    }
    props::entity::physicalize(child, props::entity::Physics::Static, 0.0f);
  }
  return true;
}

// The cart's wheels turn by how far it went along itself since they last turned.
void roll_wheels(std::uint32_t id, Cart& cart) {
  const float s = std::sin(cart.yaw);
  const float c = std::cos(cart.yaw);
  const float ahead = -(cart.position.x - cart.shown.x) * s + (cart.position.y - cart.shown.y) * c;
  if (std::fabs(ahead) < 0.0005f) {
    return;
  }
  cart.rolled += ahead;
  const Blueprint& blueprint = *cart.blueprint;
  for (std::size_t i = 1; i < blueprint.parts.size(); ++i) {
    if (!blueprint.parts[i].wheel) {
      continue;
    }
    void* wheel = props::entity::find(part_name(id, i));
    if (!wheel) {
      continue;
    }
    float local[3];
    float turn[4];
    const bool front = blueprint.parts[i].offset.y > blueprint.front_axle - 0.5f;
    part_on_root(blueprint.parts[0], blueprint.parts[i], cart.rolled,
                 front && blueprint.horses.size() > 1 ? cart.shown_steer : 0.0f, local, turn);
    props::entity::place_rotated(wheel, local, turn);
  }
}

// The local player climbs down beside their seat.
void step_down(const Cart& cart, int seat) {
  if (seat < 0 || !natives.place_local) {
    return;
  }
  Vec3 local = cart.blueprint->seats[std::size_t(seat)].offset;
  local.x = local.x >= 0.0f ? local.x + kStepDownMeters : local.x - kStepDownMeters;
  local.z = 0.0f;
  const Vec3 at = world_point(cart, local);
  const float position[3] = {at.x, at.y, at.z};
  float rotation[4];
  rotation_of(cart.yaw, rotation);
  natives.place_local(position, rotation);
}

// The local walking driver stands where they lead the cart, facing the way it goes.
void stand_at_lead(const Cart& cart) {
  if (!natives.place_local) {
    return;
  }
  const Vec3 at = world_point(cart, {cart.blueprint->lead.x, cart.blueprint->lead.y, 0.0f});
  const float position[3] = {at.x, at.y, at.z};
  float rotation[4];
  rotation_of(cart.yaw, rotation);
  natives.place_local(position, rotation);
}

void spawn(const CartOrder& order) {
  const Blueprint* blueprint = blueprint_of(order.blueprint);
  if (!blueprint) {
    log_line("CART %u %s: no such blueprint in this client", order.id, order.blueprint.c_str());
    return;
  }
  const auto found = carts.find(order.id);
  if (found != carts.end()) {
    remove_horses(order.id, *found->second.blueprint);
    remove_load(order.id, found->second);
    remove_parts(order.id, *found->second.blueprint);
    carts.erase(found);
  }
  Cart cart;
  cart.id = order.id;
  cart.blueprint = blueprint;
  cart.position = vec_of(order.position);
  cart.yaw = yaw_of(order.rotation);
  settle(cart);
  cart.target = cart.position;
  cart.target_yaw = cart.yaw;
  cart.max_speed = order.max_speed;
  cart.reported = cart.position;
  cart.reported_yaw = cart.yaw;
  cart.shown = cart.position;
  cart.shown_yaw = cart.yaw;
  const bool built = build(order.id, cart);
  log_line("CART %u %s", order.id, built ? "spawned" : "not spawned");
  if (built) {
    carts[order.id] = cart;
  }
}

void destroy(std::uint32_t id) {
  const auto found = carts.find(id);
  if (found == carts.end()) {
    return;
  }
  const std::uint32_t own = natives.local_player();
  const int seat = seat_of(found->second, own);
  if (held_on(found->second, seat)) {
    step_down(found->second, seat);
  }
  remove_horses(id, *found->second.blueprint);
  remove_load(id, found->second);
  remove_parts(id, *found->second.blueprint);
  carts.erase(found);
}

// The cart's body and load in this game: solid, or not while the local player climbs. The climb's
// clip moves the body through the cart's side, and its capsule stopped there: it stayed on
// the ground and jumped to the seat. Every other game's replica has no capsule on a cart.
void set_solid(const Cart& cart, bool solid) {
  const auto physics = solid ? props::entity::Physics::Static : props::entity::Physics::None;
  for (std::size_t i = 0; i < cart.blueprint->parts.size(); ++i) {
    props::entity::physicalize(props::entity::find(part_name(cart.id, i)), physics, 0.0f);
  }
  for (std::size_t i = 0; i < cart.load_parts; ++i) {
    props::entity::physicalize(props::entity::find(load_part_name(cart.id, i)), physics, 0.0f);
  }
}

Vec3 cart_local(const Cart& cart, float x, float y);

// Whether the seat's way on and off goes by the back of the cart (the benches), not its front.
bool boards_at_back(int seat) {
  return seat >= int(CartSeat::RightBack);
}

// A cart's benches lie behind its axle, off the wagons' place: its passengers' clips end on their
// seats.
bool on_wagon_bench(const Cart& cart, int seat) {
  return boards_at_back(seat) && cart.blueprint->back_reach >= -1.0f;
}

// Where the seat's clip ends along the cart: a body moves along its bench from and to there.
float clip_end_y(const Cart& cart, int seat) {
  const Seat& held = cart.blueprint->seats[std::size_t(seat)];
  return on_wagon_bench(cart, seat) ? kClipEndY[std::size_t(seat)] : held.offset.y;
}

float clip_end_x(const Cart& cart, int seat) {
  const Seat& held = cart.blueprint->seats[std::size_t(seat)];
  return on_wagon_bench(cart, seat) ? kClipEndX[std::size_t(seat)] : held.offset.x;
}

// Where the seat's climb starts, in the cart's frame, and which way the body then faces on the
// cart.
Vec3 climb_start(const Cart& cart, int seat, float& start_yaw) {
  const Seat& held = cart.blueprint->seats[std::size_t(seat)];
  const Climb& climb = kClimbs[std::size_t(seat)];
  start_yaw = held.yaw - climb.turn;
  if (on_wagon_bench(cart, seat)) {
    return {kBoardBackX, kBoardBackY, 0.0f};
  }
  const float s = std::sin(start_yaw);
  const float c = std::cos(start_yaw);
  return {held.offset.x - (climb.right * c - climb.ahead * s),
          held.offset.y - (climb.right * s + climb.ahead * c), held.offset.z - climb.up};
}

// A world point in the cart's frame, on the ground plane of its heading.
Vec3 cart_local(const Cart& cart, float x, float y) {
  const float s = std::sin(cart.yaw);
  const float c = std::cos(cart.yaw);
  const float dx = x - cart.position.x;
  const float dy = y - cart.position.y;
  return {dx * c + dy * s, -dx * s + dy * c, 0.0f};
}

// The way to the climb's start, round the cart: climbs start behind it or at one side of its front.
std::vector<Vec3> walk_way(const Vec3& from, const Vec3& start) {
  const float from_side = from.x >= 0.0f ? kWalkRoundSide : -kWalkRoundSide;
  const bool at_back = from.y < kWalkRoundBack + 0.5f;
  std::vector<Vec3> way;
  if (start.y < 0.5f) {
    if (!at_back) {
      way.push_back({from_side, kWalkRoundBack, 0.0f});
    }
  } else {
    const float start_side = start.x >= 0.0f ? kWalkRoundSide : -kWalkRoundSide;
    const bool same_side = from.x * start.x > 0.0f && std::fabs(from.x) >= kWalkRoundSide - 0.3f;
    if (!same_side) {
      if (!at_back) {
        way.push_back({from_side, kWalkRoundBack, 0.0f});
      }
      way.push_back({start_side, kWalkRoundBack, 0.0f});
    }
  }
  way.push_back(start);
  return way;
}

void report_at_start(Cart& cart) {
  cart.climb_waits = false;
  cart.hold_start = true;
  CartReport report;
  report.kind = CartReportKind::AtStart;
  report.cart = cart.id;
  natives.report(report);
}

// The body waits at the start of its climb, or the terrain there would take it lower than the
// wagon's floor is high.
void hold_at_start(Cart& cart) {
  float start_yaw = 0.0f;
  const Vec3 start = world_point(cart, climb_start(cart, cart.walk_seat, start_yaw));
  const float at[3] = {start.x, start.y, start.z};
  float turned[4];
  rotation_of(cart.yaw + start_yaw, turned);
  natives.place_local(at, turned);
}

// The body has come to the start of its climb, or gave up walking there: the rest in a moment, with
// the cart let through, and then the climb.
void finish_walk(Cart& cart) {
  if (cart.walk_forward && natives.press) {
    press_key("moveforward", 2, 0.0f);
  }
  cart.walk_forward = false;
  cart.walk.clear();
  float position[3];
  float rotation[4];
  if (!natives.local_pose || !natives.local_pose(position, rotation)) {
    report_at_start(cart);
    return;
  }
  set_solid(cart, false);
  float start_yaw = 0.0f;
  const Vec3 start = world_point(cart, climb_start(cart, cart.walk_seat, start_yaw));
  const float dx = start.x - position[0];
  const float dy = start.y - position[1];
  const float dz = start.z - position[2];
  const float gap = std::sqrt(dx * dx + dy * dy);
  const float seconds =
      gap > kClimbApproachStuck ? kClimbApproachStuckSeconds : kClimbApproachSeconds;
  cart.approach_seconds = seconds;
  cart.approach_rate = {dx / seconds, dy / seconds, dz / seconds};
  cart.approach_turn_rate = turn(yaw_of(rotation), cart.yaw + start_yaw) / seconds;
  cart.climb_waits = true;
}

// The local player's seat is ordered: they walk to where its climb starts, round the cart, with the
// game's own movement keys, and everyone sees it as any walk. From far off they are put there.
void start_walk(Cart& cart, int seat, double now) {
  float position[3];
  float rotation[4];
  if (!natives.local_pose || !natives.place_local || !natives.local_pose(position, rotation)) {
    report_at_start(cart);
    return;
  }
  cart.walk_seat = seat;
  float start_yaw = 0.0f;
  const Vec3 local = climb_start(cart, seat, start_yaw);
  const Vec3 start = world_point(cart, local);
  const float dx = start.x - position[0];
  const float dy = start.y - position[1];
  if (!natives.press || dx * dx + dy * dy > kClimbApproachFarthest * kClimbApproachFarthest) {
    const float at[3] = {start.x, start.y, start.z};
    float turned[4];
    rotation_of(cart.yaw + start_yaw, turned);
    natives.place_local(at, turned);
    set_solid(cart, false);
    report_at_start(cart);
    return;
  }
  cart.walk = walk_way(cart_local(cart, position[0], position[1]), local);
  cart.walk_since = now;
  cart.walk_gained_at = now;
  cart.walk_best = 1e9f;
}

// The walk, this frame: the body turns to the next point with the game's own turning key (its view
// goes with it) and walks on with the forward key.
void keep_walking(Cart& cart, float dt, double now) {
  float position[3];
  float rotation[4];
  if (!natives.local_pose(position, rotation)) {
    return;
  }
  const Vec3 next = world_point(cart, cart.walk.front());
  const float dx = next.x - position[0];
  const float dy = next.y - position[1];
  const float gap = std::sqrt(dx * dx + dy * dy);
  if (gap < kWalkNear) {
    cart.walk.erase(cart.walk.begin());
    cart.walk_best = 1e9f;
    if (cart.walk.empty()) {
      finish_walk(cart);
    }
    return;
  }
  if (gap < cart.walk_best - kWalkStuckMeters) {
    cart.walk_best = gap;
    cart.walk_gained_at = now;
  }
  const bool stuck = cart.walk_forward && now - cart.walk_gained_at > kWalkStuckSeconds;
  if (stuck || now - cart.walk_since > kWalkLongest) {
    finish_walk(cart);
    return;
  }
  const float off = turn(yaw_of(rotation), std::atan2(-dx, dy));
  const float step = kWalkTurnRate * dt;
  const float degrees = off * 180.0f / kPi;
  press_key("rotateyaw", 1, -std::clamp(degrees, -step, step));
  const bool forward = std::fabs(off) < kWalkTurnFirst;
  if (forward != cart.walk_forward) {
    press_key("moveforward", forward ? 1 : 2, forward ? 1.0f : 0.0f);
    cart.walk_forward = forward;
    cart.walk_gained_at = now;
  }
}

// The local player on their way up or down: their clip moves the body; it goes along with the cart
// and, as the climb starts, on to the clip's start. A standing cart past that leaves it to the
// clip.
void carry_climber(Cart& cart, const Vec3& cart_moved, float dt) {
  const bool cart_went = cart_moved.x != 0.0f || cart_moved.y != 0.0f || cart_moved.z != 0.0f;
  if (cart.approach_seconds <= 0.0f && !cart_went) {
    return;
  }
  float position[3];
  float rotation[4];
  if (!natives.local_pose || !natives.place_local || !natives.local_pose(position, rotation)) {
    return;
  }
  const float share = std::min(dt, std::max(cart.approach_seconds, 0.0f));
  cart.approach_seconds -= share;
  if (cart.approach_seconds <= 0.0f && cart.climb_waits) {
    report_at_start(cart);
  }
  const float at[3] = {position[0] + cart_moved.x + cart.approach_rate.x * share,
                       position[1] + cart_moved.y + cart.approach_rate.y * share,
                       position[2] + cart_moved.z + cart.approach_rate.z * share};
  float turned[4];
  rotation_of(yaw_of(rotation) + cart.approach_turn_rate * share, turned);
  natives.place_local(at, turned);
}

void seat(Cart& cart, const CartOrder& order) {
  const auto index = std::size_t(order.seat);
  // A player sits in one seat: an older seat of theirs is free now.
  for (std::uint32_t& rider : cart.riders) {
    if (rider == order.player) {
      rider = 0;
    }
  }
  cart.riders[index] = order.player;
  if (order.seat == CartSeat::Driver) {
    cart.driver = order.player;
    cart.target = cart.position;
    cart.target_yaw = cart.yaw;
    cart.speed = 0.0f;
  }
  // A seated driver sits on the front plank (frame holds them there); a walking one starts where
  // they lead it.
  cart.settled[index] = false;
  cart.climbing_off[index] = false;
  if (index == 0) {
    cart.reins_held = false;
  }
  const bool walks = order.seat == CartSeat::Driver && !seated_driver(cart);
  if (order.player != natives.local_player()) {
    return;
  }
  if (walks) {
    stand_at_lead(cart);
    return;
  }
  start_walk(cart, int(index), last_frame);
}

// The rider's climb on has ended: every game holds them on the seat from here.
void settled(Cart& cart, const CartOrder& order) {
  const auto index = std::size_t(order.seat);
  if (cart.riders[index] != order.player) {
    return;
  }
  cart.settled[index] = true;
  if (index == 0) {
    cart.reins_held = true;
  }
  cart.settle_fresh[index] = true;
  cart.scoot[index] = false;
  if (order.player == natives.local_player()) {
    if (!cart.walk.empty()) {
      finish_walk(cart);
    }
    cart.climb_waits = false;
    cart.hold_start = false;
    cart.approach_seconds = 0.0f;
    set_solid(cart, true);
  }
}

// The rider's clip starts: it moves their body from here.
void climb_on(Cart& cart, const CartOrder& order) {
  if (order.player == natives.local_player()) {
    cart.hold_start = false;
  }
}

// The rider moves along their bench to where their climb down starts.
void scoot_off(Cart& cart, const CartOrder& order) {
  const auto index = std::size_t(order.seat);
  if (cart.riders[index] == order.player) {
    cart.scoot_left[index] = kScootSeconds;
    cart.scoot[index] = true;
  }
}

// The rider's climb down starts: their clip takes the body from the seat.
void climb_off(Cart& cart, const CartOrder& order) {
  const auto index = std::size_t(order.seat);
  if (cart.riders[index] != order.player) {
    return;
  }
  cart.settled[index] = false;
  cart.climbing_off[index] = true;
  cart.scoot[index] = false;
  if (order.player == natives.local_player()) {
    set_solid(cart, false);
    // The seat's sitting pose holds the body in from the bench; the clip starts from the bench.
    float position[3];
    float rotation[4];
    const int seat = int(index);
    if (on_wagon_bench(cart, seat) && natives.local_pose &&
        natives.local_pose(position, rotation)) {
      const Seat& held = cart.blueprint->seats[index];
      const Vec3 from =
          world_point(cart, {clip_end_x(cart, seat), clip_end_y(cart, seat), held.offset.z});
      cart.approach_seconds = kClimbOffApproachSeconds;
      cart.approach_rate = {(from.x - position[0]) / kClimbOffApproachSeconds,
                            (from.y - position[1]) / kClimbOffApproachSeconds,
                            (from.z - position[2]) / kClimbOffApproachSeconds};
      cart.approach_turn_rate = 0.0f;
    }
  }
}

// The clip's way down ended below the terrain (the cart stands on its own ground): up onto it.
void stand_on_ground() {
  float position[3];
  float rotation[4];
  float ground = 0.0f;
  if (!natives.ground_height || !natives.local_pose(position, rotation) ||
      !natives.ground_height(position[0], position[1], position[2] + kGroundRayAbove, ground)) {
    return;
  }
  if (position[2] < ground) {
    position[2] = ground;
    natives.place_local(position, rotation);
  }
}

void leave(Cart& cart, const CartOrder& order) {
  const auto index = std::size_t(order.seat);
  if (cart.riders[index] != order.player) {
    return;
  }
  const bool climbed_down = cart.climbing_off[index];
  cart.riders[index] = 0;
  cart.settled[index] = false;
  cart.climbing_off[index] = false;
  if (index == 0) {
    cart.reins_held = false;
  }
  const bool walked = order.seat == CartSeat::Driver && !seated_driver(cart);
  if (order.seat == CartSeat::Driver) {
    // The server tells a driver nothing of their own cart, so its target is still where the drive
    // began: without this, follow() takes the cart back there once nobody drives it.
    if (cart.driver != 0 && cart.driver == natives.local_player()) {
      cart.target = cart.position;
      cart.target_yaw = cart.yaw;
    }
    cart.driver = 0;
    cart.pace = CartPace::Stand;
    cart.speed = 0.0f;
  }
  if (order.player == natives.local_player() && !walked) {
    if (!cart.walk.empty()) {
      finish_walk(cart);
    }
    cart.climb_waits = false;
    cart.hold_start = false;
    cart.approach_seconds = 0.0f;
    if (!climbed_down) {
      step_down(cart, int(index));
    } else {
      stand_on_ground();
    }
    set_solid(cart, true);
  }
}

// The local player leads the cart on foot (by the horse's reins, or at the hitch of a cart
// without horses): its lead point follows them and it turns towards them, as a towed cart does.
bool way_blocked(const Cart& cart, float distance);

// True when the cart, moved to `next`, would stand on ground steeper than its horses pull it on
// (and steeper than where it stands now, so it can always come off a slope).
bool too_steep(const Cart& cart, Cart next) {
  settle(next);
  return std::fabs(next.pitch) > kSteepestPitch && std::fabs(next.pitch) > std::fabs(cart.pitch);
}

void lead_on_foot(Cart& cart) {
  float position[3];
  float rotation[4];
  if (!natives.local_pose || !natives.local_pose(position, rotation)) {
    return;
  }
  const Vec3& lead = cart.blueprint->lead;
  Cart next = cart;
  const float dx = position[0] - cart.position.x;
  const float dy = position[1] - cart.position.y;
  if (dx * dx + dy * dy > 0.0001f) {
    // The heading that puts the lead point (off the middle when beside a horse) on the walker.
    next.yaw = std::atan2(-dx, dy) - std::atan2(-lead.x, lead.y);
  }
  const float s = std::sin(next.yaw);
  const float c = std::cos(next.yaw);
  next.position = {position[0] - (lead.x * c - lead.y * s), position[1] - (lead.x * s + lead.y * c),
                   position[2]};
  // How far the cart goes along itself (backwards when negative).
  const float moved = (next.position.x - cart.position.x) * -std::sin(cart.yaw) +
                      (next.position.y - cart.position.y) * std::cos(cart.yaw);
  if (!way_blocked(next, moved) && !too_steep(cart, next)) {
    cart.yaw = next.yaw;
    cart.position = next.position;
    return;
  }
  // The horse does not go there: the walker is held at its head by the reins.
  if (natives.place_local) {
    const Vec3 at = world_point(cart, lead);
    const float held[3] = {at.x, at.y, position[2]};
    natives.place_local(held, nullptr);
  }
}

// The local driver's movement actions while they drive (take_action).
struct DriveKeys {
  bool forward = false;
  bool back = false;
  bool left = false;
  bool right = false;
  bool fast = false;
};
DriveKeys keys;

// True when something is in the way of the cart going `distance` along itself (backwards when
// negative): rays from its front (or back) across its width, at a low and a high height above
// the ground there.
bool way_blocked(const Cart& cart, float distance) {
  if (!natives.blocked) {
    return false;
  }
  const bool ahead = distance >= 0.0f;
  const float reach = ahead ? cart.blueprint->front_reach : cart.blueprint->back_reach;
  const float look = (ahead ? 1.0f : -1.0f) * (std::fabs(distance) + kBlockMargin);
  for (const float height : kBlockHeights) {
    for (const float side : {-kBlockHalfWidth, 0.0f, kBlockHalfWidth}) {
      Vec3 from = world_point(cart, {side, reach, 0.0f});
      Vec3 to = world_point(cart, {side, reach + look, 0.0f});
      float ground = from.z;
      if (natives.ground_height) {
        natives.ground_height(from.x, from.y, from.z + kGroundRayAbove, ground);
      }
      const float a[3] = {from.x, from.y, ground + height};
      const float b[3] = {to.x, to.y, ground + height};
      if (natives.blocked(a, b, cart.id)) {
        return true;
      }
    }
  }
  return false;
}

// The local player drives a harnessed cart from its bench: forward walks the horses on (a trot
// with the sprint), back slows and backs up, left and right steer as its front axle would.
void drive_seated(Cart& cart, float dt) {
  float target = 0.0f;
  if (keys.forward && !keys.back) {
    // The harnessed horses trot at most as fast as the game's trot blend space goes (3.6 m/s).
    const float trot = std::clamp(cart.max_speed, kDriveWalkSpeed, kDriveTrotSpeed);
    target = keys.fast ? trot : kDriveWalkSpeed;
  } else if (keys.back && !keys.forward) {
    target = -kDriveReverseSpeed;
  }
  const bool gaining = target * cart.speed >= 0.0f && std::fabs(target) > std::fabs(cart.speed);
  const float step = (gaining ? kDriveAcceleration : kDriveBraking) * dt;
  cart.speed += std::clamp(target - cart.speed, -step, step);
  // Something in the way stops it (a wall, a fence, a tree), as the horses would.
  Cart next = cart;
  next.position.x -= std::sin(cart.yaw) * cart.speed * dt;
  next.position.y += std::cos(cart.yaw) * cart.speed * dt;
  if (cart.speed != 0.0f && (way_blocked(cart, cart.speed * dt) || too_steep(cart, next))) {
    cart.speed = 0.0f;
    return;
  }
  const float wanted = ((keys.left ? 1.0f : 0.0f) - (keys.right ? 1.0f : 0.0f)) * kSteerRadians;
  const float steer_rate = std::fabs(wanted) > std::fabs(cart.steer) ? kSteerInRate : kSteerOutRate;
  cart.steer += std::clamp(wanted - cart.steer, -steer_rate * dt, steer_rate * dt);
  const float wheelbase = std::max(1.0f, cart.blueprint->front_axle - cart.blueprint->back_axle);
  cart.yaw += cart.speed * std::tan(cart.steer) / wheelbase * dt;
  cart.position.x -= std::sin(cart.yaw) * cart.speed * dt;
  cart.position.y += std::cos(cart.yaw) * cart.speed * dt;
}

// The driver's client reports the cart's pose and pace when they changed.
void report_pose(std::uint32_t id, Cart& cart, double now) {
  const double elapsed = now - cart.reported_at;
  if (elapsed < kReportSeconds) {
    return;
  }
  const float mx = cart.position.x - cart.reported.x;
  const float my = cart.position.y - cart.reported.y;
  const float moved = std::sqrt(mx * mx + my * my);
  const float speed = elapsed > 0.0 ? float(moved / elapsed) : 0.0f;
  const float ahead = -mx * std::sin(cart.yaw) + my * std::cos(cart.yaw);
  CartPace pace = CartPace::Stand;
  if (speed >= kStandingSpeed) {
    if (ahead < 0.0f) {
      pace = CartPace::Reverse;
    } else {
      pace = speed > kWalkingSpeed ? CartPace::Trot : CartPace::Walk;
    }
  }
  cart.pace = pace;
  const bool changed = moved > kReportMeters ||
                       std::fabs(turn(cart.reported_yaw, cart.yaw)) > kReportRadians ||
                       pace != cart.reported_pace;
  if (!changed) {
    // A cart standing still is not reported again; its last report stays the reference.
    if (pace == CartPace::Stand) {
      cart.reported_at = now;
    }
    return;
  }
  CartReport report;
  report.kind = CartReportKind::Drive;
  report.cart = id;
  report.position = {cart.position.x, cart.position.y, cart.position.z};
  float turned[4];
  rotation_of(cart.yaw, turned);
  report.rotation = {turned[0], turned[1], turned[2], turned[3]};
  report.pace = pace;
  if (natives.report && natives.report(report)) {
    cart.reported_at = now;
    cart.reported = cart.position;
    cart.reported_yaw = cart.yaw;
    cart.reported_pace = pace;
  }
}

// Another player drives the cart (or nobody does): it closes on the server's pose.
void follow(Cart& cart, float dt) {
  const float dx = cart.target.x - cart.position.x;
  const float dy = cart.target.y - cart.position.y;
  const float dz = cart.target.z - cart.position.z;
  const float dyaw = turn(cart.yaw, cart.target_yaw);
  const float share = std::min(1.0f, dt * kFollowRate);
  cart.position = {cart.position.x + dx * share, cart.position.y + dy * share,
                   cart.position.z + dz * share};
  cart.yaw += dyaw * share;
}

Vec3 seat_point(const Cart& cart, const Seat& held) {
  return world_point(cart, held.offset);
}

// A seated body's rotation: the seat's way on the cart, tilted and turned with it.
void seat_rotation(const Cart& cart, const Seat& held, float out[4]) {
  float on_cart[4];
  cart_rotation(cart, on_cart);
  float on_seat[4];
  rotation_of(held.yaw, on_seat);
  const std::array<float, 4> whole = multiply({on_cart[0], on_cart[1], on_cart[2], on_cart[3]},
                                              {on_seat[0], on_seat[1], on_seat[2], on_seat[3]});
  out[0] = whole[0];
  out[1] = whole[1];
  out[2] = whole[2];
  out[3] = whole[3];
}

// Where the rider is shown on their seat. `position` and `rotation` hold where their body stands
// now: the first time after their climb that gives the offset from the seat, which fades
// (fade_settle).
void rider_seat_pose(Cart& cart, int seat, const Seat& held, float position[3], float rotation[4]) {
  const auto index = std::size_t(seat);
  Vec3 on_seat = held.offset;
  if (cart.scoot[index]) {
    const float gone = 1.0f - std::clamp(cart.scoot_left[index] / kScootSeconds, 0.0f, 1.0f);
    const float eased = gone * gone * (3.0f - 2.0f * gone);
    on_seat.y += (clip_end_y(cart, seat) - on_seat.y) * eased;
  }
  const Vec3 at = seat_point(cart, {true, on_seat, held.yaw});
  float turned[4];
  seat_rotation(cart, held, turned);
  if (cart.settle_fresh[index]) {
    cart.settle_fresh[index] = false;
    cart.settle_offset[index] = {position[0] - at.x, position[1] - at.y, position[2] - at.z};
    cart.settle_turn[index] = turn(yaw_of(turned), yaw_of(rotation));
    const Vec3& off = cart.settle_offset[index];
    const float way = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
    cart.settle_seconds[index] = std::max(kSettleSeconds, way / kBenchMoveSpeed);
    cart.settle_left[index] = cart.settle_seconds[index];
    if (natives.log) {
      natives.log("CART %u seat %d: the climb ended %.2f %.2f %.2f m and %.2f rad from the seat",
                  cart.id, seat, off.x, off.y, off.z, cart.settle_turn[index]);
    }
  }
  const float whole = std::max(cart.settle_seconds[index], kSettleSeconds);
  const float left = std::clamp(cart.settle_left[index] / whole, 0.0f, 1.0f);
  const float share = left * left * (3.0f - 2.0f * left);
  const Vec3& off = cart.settle_offset[index];
  position[0] = at.x + off.x * share;
  position[1] = at.y + off.y * share;
  position[2] = at.z + off.z * share;
  const float half = cart.settle_turn[index] * share * 0.5f;
  const std::array<float, 4> faded = multiply({0.0f, 0.0f, std::sin(half), std::cos(half)},
                                              {turned[0], turned[1], turned[2], turned[3]});
  rotation[0] = faded[0];
  rotation[1] = faded[1];
  rotation[2] = faded[2];
  rotation[3] = faded[3];
}

void fade_settle(Cart& cart, int seat, float dt) {
  const auto index = std::size_t(seat);
  cart.settle_left[index] = std::max(0.0f, cart.settle_left[index] - dt);
  cart.scoot_left[index] = std::max(0.0f, cart.scoot_left[index] - dt);
}

// The local player sits on their seat while the cart moves, facing the seat's way and turning with
// the cart. Their looking around turns only the head and the view: free look in the first person
// (camera/free_look.hpp, animation_orders_tick.cpp), and take_action keeps the turning from the
// body.
void hold_passenger(Cart& cart, int seat, float dt) {
  if (!natives.place_local) {
    return;
  }
  const Seat& held = cart.blueprint->seats[std::size_t(seat)];
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (natives.local_pose) {
    natives.local_pose(position, rotation);
  }
  cart.passenger_seat = seat;
  rider_seat_pose(cart, seat, held, position, rotation);
  fade_settle(cart, seat, dt);
  natives.place_local(position, rotation);
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  installed = natives.attach_child && natives.local_player && natives.local_pose &&
              natives.place_local && natives.report;
  return installed;
}

void apply(const CartOrder& order) {
  if (!installed) {
    return;
  }
  if (order.kind == CartKind::Spawn) {
    spawn(order);
    return;
  }
  if (order.kind == CartKind::Destroy) {
    destroy(order.id);
    return;
  }
  const auto found = carts.find(order.id);
  if (found == carts.end()) {
    return;
  }
  Cart& cart = found->second;
  switch (order.kind) {
    case CartKind::Seat:
      if (cart.blueprint->seats[std::size_t(order.seat)].present) {
        seat(cart, order);
      }
      break;
    case CartKind::Settled:
      settled(cart, order);
      break;
    case CartKind::ClimbOn:
      climb_on(cart, order);
      break;
    case CartKind::ScootOff:
      scoot_off(cart, order);
      break;
    case CartKind::ClimbOff:
      climb_off(cart, order);
      break;
    case CartKind::ReinsTaken:
      cart.reins_held = cart.driver == order.player;
      break;
    case CartKind::ReinsGone:
      cart.reins_held = false;
      break;
    case CartKind::Leave:
      leave(cart, order);
      break;
    case CartKind::Deny:
      log_line("CART %u seat refused: %s", order.id,
               kcd2s::integration::cart_seat_name(order.seat));
      break;
    case CartKind::Teleport:
      cart.position = vec_of(order.position);
      cart.yaw = yaw_of(order.rotation);
      cart.target = cart.position;
      cart.target_yaw = cart.yaw;
      cart.reported = cart.position;
      cart.reported_yaw = cart.yaw;
      cart.pace = CartPace::Stand;
      cart.speed = 0.0f;
      settle(cart);
      cart.shown = cart.position;
      cart.shown_yaw = cart.yaw;
      place(order.id, cart);
      if (cart.driver != 0 && cart.driver == natives.local_player() && !seated_driver(cart)) {
        stand_at_lead(cart);
      }
      break;
    case CartKind::Speed:
      cart.max_speed = order.max_speed;
      break;
    case CartKind::Load:
      show_load(order.id, cart, order.blueprint);
      break;
    case CartKind::Harness:
      // Other horses (or none) in the shafts: the ones shown go, and the next frame brings the
      // new ones in their looks.
      remove_horses(order.id, *cart.blueprint);
      cart.unharnessed = order.looks.empty();
      cart.looks = order.looks;
      log_line("CART %u harness: %s", order.id, order.looks.empty() ? "no horses" : "horses");
      break;
    case CartKind::Pose:
      cart.target = vec_of(order.position);
      cart.target_yaw = yaw_of(order.rotation);
      cart.pace = order.pace;
      break;
    default:
      break;
  }
}

void frame(double now) {
  const float dt = last_frame > 0.0 ? float(std::clamp(now - last_frame, 0.0, 0.25)) : 0.0f;
  last_frame = now;
  if (carts.empty() || !installed) {
    return;
  }
  const std::uint32_t own = natives.local_player();
  for (auto& [id, cart] : carts) {
    // The other riders' fades (this game's own is in hold_passenger).
    const int own_seat = seat_of(cart, own);
    for (std::size_t i = 0; i < kSeatCount; ++i) {
      if (int(i) != own_seat) {
        fade_settle(cart, int(i), dt);
      }
    }
    const Vec3 was = cart.position;
    if (own != 0 && cart.driver == own) {
      if (seated_driver(cart)) {
        // On the way up or down the driver does not drive yet.
        if (cart.settled[0]) {
          drive_seated(cart, dt);
        }
      } else {
        lead_on_foot(cart);
      }
      settle(cart);
      report_pose(id, cart, now);
    } else {
      follow(cart, dt);
      settle(cart);
    }
    place(id, cart);
    roll_wheels(id, cart);
    keep_horses(id, cart, dt);
    // Everyone in a seat is held there, a seated driver too.
    const int seat = seat_of(cart, own);
    if (held_on(cart, seat) && cart.settled[std::size_t(seat)]) {
      hold_passenger(cart, seat, dt);
    } else {
      cart.passenger_seat = -1;
      if (held_on(cart, seat) && !cart.walk.empty()) {
        keep_walking(cart, dt, now);
      } else if (held_on(cart, seat) && cart.hold_start) {
        hold_at_start(cart);
      } else if (held_on(cart, seat)) {
        const Vec3 moved{cart.position.x - was.x, cart.position.y - was.y, cart.position.z - was.z};
        carry_climber(cart, moved, dt);
      }
    }
  }
}

void reset() {
  carts.clear();
  last_frame = 0.0;
  keys = DriveKeys{};
}

bool rider_pose(std::uint32_t player, float position[3], float rotation[4]) {
  if (player == 0) {
    return false;
  }
  for (auto& [id, cart] : carts) {
    const int seat = seat_of(cart, player);
    if (!held_on(cart, seat) || !cart.settled[std::size_t(seat)]) {
      continue;
    }
    rider_seat_pose(cart, seat, cart.blueprint->seats[std::size_t(seat)], position, rotation);
    return true;
  }
  return false;
}

bool rider_on_cart(std::uint32_t player) {
  if (player == 0) {
    return false;
  }
  for (const auto& [id, cart] : carts) {
    if (held_on(cart, seat_of(cart, player))) {
      return true;
    }
  }
  return false;
}

bool local_held() {
  const std::uint32_t own = installed ? natives.local_player() : 0;
  if (own == 0) {
    return false;
  }
  for (const auto& [id, cart] : carts) {
    const int seat = seat_of(cart, own);
    if (held_on(cart, seat) && cart.settled[std::size_t(seat)]) {
      return true;
    }
  }
  return false;
}

bool take_action(const char* action, unsigned mode) {
  if (!action) {
    return false;
  }
  // The walk to a climb's start presses the keys itself; the player's own wait until they are on.
  if (!pressing) {
    for (const auto& [id, cart] : carts) {
      if (!cart.walk.empty() &&
          (std::strncmp(action, "rotate", 6) == 0 || std::strncmp(action, "move", 4) == 0)) {
        return mode != 2;
      }
    }
  }
  if (std::strncmp(action, "rotate", 6) == 0) {
    return local_held();
  }
  const std::uint32_t own = installed ? natives.local_player() : 0;
  // The use key gets a held player down (their game climbs down on the server's order).
  if (std::strcmp(action, "use") == 0) {
    if (mode != 1 || own == 0) {
      return false;
    }
    for (const auto& [id, cart] : carts) {
      const int seat = seat_of(cart, own);
      if (held_on(cart, seat) && cart.settled[std::size_t(seat)]) {
        CartReport report;
        report.kind = CartReportKind::Leave;
        report.cart = id;
        natives.report(report);
        return true;
      }
    }
    return false;
  }
  bool* key = nullptr;
  if (std::strcmp(action, "moveforward") == 0) {
    key = &keys.forward;
  } else if (std::strcmp(action, "moveback") == 0) {
    key = &keys.back;
  } else if (std::strcmp(action, "moveleft") == 0) {
    key = &keys.left;
  } else if (std::strcmp(action, "moveright") == 0) {
    key = &keys.right;
  } else if (std::strcmp(action, "sprint") == 0) {
    key = &keys.fast;
  }
  if (!key) {
    return false;
  }
  if (mode == 1) {
    *key = true;
  } else if (mode == 2) {
    // A release always reaches the game too, so a key held before the driver sat down is let go.
    *key = false;
    return false;
  }
  for (const auto& [id, cart] : carts) {
    if (own != 0 && cart.driver == own && seated_driver(cart) && cart.settled[0]) {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::carts::placed
