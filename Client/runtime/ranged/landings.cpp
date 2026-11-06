#include "landings.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace kcds::runtime::ranged {
namespace {

using Clock = std::chrono::steady_clock;
using kcd2s::integration::ShotLanded;

// A shot's projectile lands within this long, or never.
constexpr auto kFlight = std::chrono::seconds(30);
// How far from where it left a projectile may land, in metres.
constexpr float kReach = 400.0f;
// A pickable's item is looked at once the game has given it one; one with none by then is not a
// shot's.
constexpr auto kSettle = std::chrono::milliseconds(300);
constexpr auto kGiveUp = std::chrono::seconds(3);
// A projectile that bounced off becomes a pickable while it still falls: it is reported where it
// comes to rest, once it moved less than kStill between two looks kRestLook apart, or where it is
// after kRestGiveUp.
constexpr float kStill = 0.01f;
constexpr auto kRestLook = std::chrono::milliseconds(150);
constexpr auto kRestGiveUp = std::chrono::seconds(6);
constexpr std::size_t kMaxFlights = 64;
constexpr std::size_t kMaxWaiting = 256;
constexpr std::size_t kMaxLandings = 32;
// An item's class and the class's GUID.
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;

struct Flight {
  bool local = false;
  std::array<std::uint8_t, 16> ammo{};
  float origin[3] = {};
  Clock::time_point at;
};

struct Waiting {
  std::uint32_t entity = 0;
  Clock::time_point made;
  // Where it was at the last look, once it has an item.
  bool seen = false;
  float position[3] = {};
  Clock::time_point looked;
};

LandingNatives natives;
LandingLogger log_line = nullptr;
std::deque<Flight> flights;
std::mutex waiting_mutex;
std::vector<Waiting> waiting;
std::vector<ShotLanded> landings;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The rotation of a world matrix (its scale left out): x, y, z, w.
void rotation_of(const float* matrix, float* rotation) {
  float m[3][3];
  for (int column = 0; column < 3; ++column) {
    const float length =
        std::sqrt(matrix[column] * matrix[column] + matrix[4 + column] * matrix[4 + column] +
                  matrix[8 + column] * matrix[8 + column]);
    for (int row = 0; row < 3; ++row) {
      m[row][column] = length > 0 ? matrix[row * 4 + column] / length : 0;
    }
  }
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 1;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0) {
    const float s = 0.5f / std::sqrt(trace + 1.0f);
    w = 0.25f / s;
    x = (m[2][1] - m[1][2]) * s;
    y = (m[0][2] - m[2][0]) * s;
    z = (m[1][0] - m[0][1]) * s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = 2.0f * std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]);
    w = (m[2][1] - m[1][2]) / s;
    x = 0.25f * s;
    y = (m[0][1] + m[1][0]) / s;
    z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = 2.0f * std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]);
    w = (m[0][2] - m[2][0]) / s;
    x = (m[0][1] + m[1][0]) / s;
    y = 0.25f * s;
    z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = 2.0f * std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]);
    w = (m[1][0] - m[0][1]) / s;
    x = (m[0][2] + m[2][0]) / s;
    y = (m[1][2] + m[2][1]) / s;
    z = 0.25f * s;
  }
  const float length = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(length > 0) || !std::isfinite(length)) {
    rotation[0] = 0;
    rotation[1] = 0;
    rotation[2] = 0;
    rotation[3] = 1;
    return;
  }
  rotation[0] = x / length;
  rotation[1] = y / length;
  rotation[2] = z / length;
  rotation[3] = w / length;
}

// The shot in flight a pickable of this ammunition lying here belongs to (the oldest), or
// flights.end().
std::deque<Flight>::iterator flight_of(const std::array<std::uint8_t, 16>& ammo,
                                       const float* position) {
  for (auto it = flights.begin(); it != flights.end(); ++it) {
    const float dx = position[0] - it->origin[0];
    const float dy = position[1] - it->origin[1];
    const float dz = position[2] - it->origin[2];
    if (it->ammo == ammo && dx * dx + dy * dy + dz * dz <= kReach * kReach) {
      return it;
    }
  }
  return flights.end();
}

// Whether a pickable has come to rest since the last look; remembers where it is now.
bool at_rest(Waiting& pickable, const float* position, Clock::time_point now) {
  if (pickable.seen && now - pickable.looked < kRestLook) {
    return false;
  }
  const float dx = position[0] - pickable.position[0];
  const float dy = position[1] - pickable.position[1];
  const float dz = position[2] - pickable.position[2];
  const bool still = pickable.seen && dx * dx + dy * dy + dz * dz <= kStill * kStill;
  pickable.seen = true;
  std::memcpy(pickable.position, position, sizeof(pickable.position));
  pickable.looked = now;
  return still || now - pickable.made >= kRestGiveUp;
}

// Whether a waiting pickable is done with: a shot's (taken out of the world once at rest), or no
// shot's.
bool settle(Waiting& pickable, Clock::time_point now) {
  void* item = natives.level_item_of(pickable.entity);
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  if (!item_class) {
    return now - pickable.made >= kGiveUp;
  }
  float matrix[12] = {};
  if (!natives.entity_transform(pickable.entity, matrix)) {
    return true;
  }
  const float position[3] = {matrix[3], matrix[7], matrix[11]};
  const auto ammo = at<std::array<std::uint8_t, 16>>(item_class, kClassGuid);
  const auto flight = flight_of(ammo, position);
  if (flight == flights.end()) {
    return true;
  }
  if (!at_rest(pickable, position, now)) {
    return false;
  }
  // One stuck in what it hit (a target, a wall, a tree) has no body: the game took it away, so it
  // stays there. One that fell or bounced off is a rigid body.
  const bool stuck = natives.physics_type && natives.physics_type(pickable.entity) == 0;
  const bool local = flight->local;
  flights.erase(flight);
  if (local && landings.size() < kMaxLandings) {
    ShotLanded landed;
    landed.ammo = ammo;
    std::memcpy(landed.position, position, sizeof(landed.position));
    rotation_of(matrix, landed.rotation);
    landed.stuck = stuck;
    landings.push_back(landed);
  }
  natives.remove_entity(pickable.entity);
  if (log_line) {
    log_line("LANDED %s projectile at %.2f %.2f %.2f%s: %s", local ? "own" : "replayed",
             position[0], position[1], position[2], stuck ? " (stuck)" : "",
             local ? "reported and taken out of this world" : "taken out of this world");
  }
  return true;
}

}  // namespace

bool install_landings(const LandingNatives& given, LandingLogger logger) {
  natives = given;
  log_line = logger;
  return natives.entity_transform && natives.level_item_of && natives.remove_entity;
}

void expect_landing(bool local, const std::array<std::uint8_t, 16>& ammo, const float* origin) {
  Flight flight;
  flight.local = local;
  flight.ammo = ammo;
  std::memcpy(flight.origin, origin, sizeof(flight.origin));
  flight.at = Clock::now();
  flights.push_back(flight);
  if (flights.size() > kMaxFlights) {
    flights.pop_front();
  }
}

void pickable_spawned(std::uint32_t entity) {
  std::lock_guard<std::mutex> lock(waiting_mutex);
  if (waiting.size() < kMaxWaiting) {
    waiting.push_back(Waiting{entity, Clock::now()});
  }
}

void tick_landings() {
  if (!natives.level_item_of) {
    return;
  }
  const auto now = Clock::now();
  while (!flights.empty() && now - flights.front().at > kFlight) {
    flights.pop_front();
  }
  std::vector<Waiting> ready;
  {
    std::lock_guard<std::mutex> lock(waiting_mutex);
    if (flights.empty()) {
      waiting.clear();
      return;
    }
    for (auto it = waiting.begin(); it != waiting.end();) {
      if (now - it->made >= kSettle) {
        ready.push_back(*it);
        it = waiting.erase(it);
      } else {
        ++it;
      }
    }
  }
  std::vector<Waiting> again;
  for (Waiting& pickable : ready) {
    if (!settle(pickable, now)) {
      again.push_back(pickable);
    }
  }
  if (!again.empty()) {
    std::lock_guard<std::mutex> lock(waiting_mutex);
    waiting.insert(waiting.end(), again.begin(), again.end());
  }
}

std::vector<ShotLanded> take_landings() {
  return std::exchange(landings, {});
}

void reset_landings() {
  flights.clear();
  landings.clear();
  std::lock_guard<std::mutex> lock(waiting_mutex);
  waiting.clear();
}

}  // namespace kcds::runtime::ranged
