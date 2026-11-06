#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

// The seats a player holds: the game's smart objects (seats, benches, beds) the
// owner's player has reserved, by the smart object's WUID (the same on every client for the
// level's smart objects) and the resource index it holds. The owner publishes them in the
// NetworkPlayer field "seats" (seats_field) and every other client reserves the same for the
// player's replica (Client/runtime/seats/seat_reservations.hpp).
//
// The field: "WUID:resource" for each seat, the WUID in 16 hex digits, joined by ';'; "" for none.
// An entry may end in "@x,y,z", where the smart object stands, in whole centimetres: a
// smart object a script placed (SmartObject.spawn) gets its WUID in the order each client builds
// its objects, so the same bench had another WUID on every client (chess), and
// the other clients find it by its place instead.
namespace kcd2s::integration {

// More than anyone holds at once; a longer field is not one.
inline constexpr std::size_t kMaxSeats = 8;
// The farthest place a field carries, in centimetres (the maps are 8 km across).
inline constexpr long kMaxSeatPlace = 1000000;

struct Seat {
  std::uint64_t smart_object = 0;
  std::uint16_t resource = 0;
  // Where it stands, in centimetres, when the owner knew it; not part of which seat it is.
  bool has_place = false;
  long place[3] = {0, 0, 0};

  friend bool operator<(const Seat& a, const Seat& b) {
    if (a.smart_object != b.smart_object) {
      return a.smart_object < b.smart_object;
    }
    return a.resource < b.resource;
  }

  friend bool operator==(const Seat& a, const Seat& b) {
    return a.smart_object == b.smart_object && a.resource == b.resource;
  }
};

inline std::string seats_field(const std::set<Seat>& seats) {
  std::string field;
  for (const Seat& seat : seats) {
    char entry[80];
    if (seat.has_place) {
      std::snprintf(entry, sizeof(entry), "%016llX:%u@%ld,%ld,%ld",
                    static_cast<unsigned long long>(seat.smart_object), unsigned(seat.resource),
                    seat.place[0], seat.place[1], seat.place[2]);
    } else {
      std::snprintf(entry, sizeof(entry), "%016llX:%u",
                    static_cast<unsigned long long>(seat.smart_object), unsigned(seat.resource));
    }
    if (!field.empty()) {
      field += ';';
    }
    field += entry;
  }
  return field;
}

// "x,y,z" in whole centimetres; false when it is not one.
inline bool parse_seat_place(const char* begin, const char* end, long place[3]) {
  const char* at = begin;
  for (int axis = 0; axis < 3; ++axis) {
    const auto read = std::from_chars(at, end, place[axis]);
    if (read.ec != std::errc() || place[axis] < -kMaxSeatPlace || place[axis] > kMaxSeatPlace) {
      return false;
    }
    at = read.ptr;
    if (axis < 2) {
      if (at == end || *at != ',') {
        return false;
      }
      ++at;
    }
  }
  return at == end;
}

// The seats of a field; false (and seats left partly filled) when it is not one.
inline bool parse_seats_field(const std::string& field, std::set<Seat>& seats) {
  const char* text = field.data();
  std::size_t start = 0;
  while (start < field.size()) {
    std::size_t end = field.find(';', start);
    if (end == std::string::npos) {
      end = field.size();
    }
    const std::size_t colon = field.find(':', start);
    if (colon == std::string::npos || colon > end || colon == start || colon + 1 == end ||
        seats.size() == kMaxSeats) {
      return false;
    }
    Seat seat;
    unsigned resource = 0;
    const std::size_t at = field.find('@', colon);
    const std::size_t resource_end = at != std::string::npos && at < end ? at : end;
    const auto wuid = std::from_chars(text + start, text + colon, seat.smart_object, 16);
    const auto index = std::from_chars(text + colon + 1, text + resource_end, resource);
    if (wuid.ec != std::errc() || wuid.ptr != text + colon || index.ec != std::errc() ||
        index.ptr != text + resource_end || resource > 0xFFFF) {
      return false;
    }
    if (resource_end != end) {
      if (!parse_seat_place(text + resource_end + 1, text + end, seat.place)) {
        return false;
      }
      seat.has_place = true;
    }
    seat.resource = std::uint16_t(resource);
    seats.insert(seat);
    start = end + 1;
    if (start == field.size()) {
      // A trailing ';' is not a field this side writes.
      return false;
    }
  }
  return true;
}

}  // namespace kcd2s::integration
