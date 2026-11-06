#include "kcd2s/integration/seats.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// Seats: the "seats" field carries the smart objects a player holds.
KCD2S_TEST(seats, field_round_trip) {
  std::set<Seat> seats;
  KCD2S_CHECK(seats_field(seats).empty());
  KCD2S_CHECK(parse_seats_field("", seats) && seats.empty());

  seats = {{0x080000000000021D, 0}, {0x0800000000001749, 0}, {0x080000000000007F, 2}};
  const std::string field = seats_field(seats);
  KCD2S_CHECK(field == "080000000000007F:2;080000000000021D:0;0800000000001749:0");
  std::set<Seat> parsed;
  KCD2S_CHECK(parse_seats_field(field, parsed));
  KCD2S_CHECK(parsed == seats);
}

// A placed smart object's seat carries where it stands: its WUID differs between clients.
KCD2S_TEST(seats, field_carries_the_place) {
  Seat bench{0x0800000000001A86, 0};
  bench.has_place = true;
  bench.place[0] = 153460;
  bench.place[1] = 212045;
  bench.place[2] = -1342;
  const std::string field = seats_field({bench, {0x080000000000021D, 1}});
  KCD2S_CHECK(field == "080000000000021D:1;0800000000001A86:0@153460,212045,-1342");
  std::set<Seat> parsed;
  KCD2S_CHECK(parse_seats_field(field, parsed));
  KCD2S_CHECK(parsed.size() == 2);
  const Seat& back = *parsed.rbegin();
  KCD2S_CHECK(back.has_place && back.place[0] == 153460 && back.place[2] == -1342);
  KCD2S_CHECK(!parsed.begin()->has_place);
  const char* refused[] = {
      "0800000000001A86:0@",              // no place
      "0800000000001A86:0@1,2",           // two axes
      "0800000000001A86:0@1,2,3,4",       // four
      "0800000000001A86:0@1,2,x",         // not a number
      "0800000000001A86:0@1,2,99999999",  // too far
      "0800000000001A86:@1,2,3",          // no resource
  };
  for (const char* text : refused) {
    std::set<Seat> seats;
    KCD2S_CHECK(!parse_seats_field(text, seats));
  }
}

KCD2S_TEST(seats, malformed_fields_refused) {
  const char* refused[] = {
      "080000000000021D",         // no resource
      "080000000000021D:",        // empty resource
      ":0",                       // no WUID
      "08000000000002XD:0",       // not hex
      "080000000000021D:65536",   // resource out of range
      "080000000000021D:0;",      // trailing separator
      "080000000000021D:0;;1:0",  // empty entry
      "080000000000021D:-1",      // negative resource
      "1080000000000021D:0",      // WUID out of range
  };
  for (const char* field : refused) {
    std::set<Seat> seats;
    KCD2S_CHECK(!parse_seats_field(field, seats));
  }
  std::string many;
  for (int i = 0; i <= int(kMaxSeats); ++i) {
    if (i) {
      many += ';';
    }
    many += "080000000000021D:" + std::to_string(i);
  }
  std::set<Seat> seats;
  KCD2S_CHECK(!parse_seats_field(many, seats));
}
