#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// Appearance: APPEARANCE carries exactly the named parts to the player's own client;
// the "appearance" field carries what a body wears.
KCD2S_TEST(appearance, request_round_trip) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  WorldConfig c;
  KCD2S_CHECK(server.configure_world(p, c));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }

  AppearanceRequest first;
  first.head = "m_head_004";
  first.beard = "";
  AppearanceRequest second;
  second.gender = "male";
  second.hair = "m_hair_005_ginger";
  KCD2S_CHECK(server.request_appearance(e, first));
  KCD2S_CHECK(server.request_appearance(e, second));
  // A name that is no component name never leaves the server; an unknown player has no client.
  AppearanceRequest spaced;
  spaced.hair = "two words";
  KCD2S_CHECK(!server.request_appearance(e, spaced));
  AppearanceRequest piped;
  piped.body = "a|b";
  KCD2S_CHECK(!server.request_appearance(e, piped));
  AppearanceRequest long_name;
  long_name.head = std::string(kAppearanceNameMax + 1, 'h');
  KCD2S_CHECK(!server.request_appearance(e, long_name));
  KCD2S_CHECK(!server.request_appearance(e + 100, first));
  for (int i = 0; i < 5; ++i) {
    tick();
  }

  auto requests = client.take_appearance_requests();
  KCD2S_CHECK_EQ(requests.size(), std::size_t(2));
  KCD2S_CHECK(!requests[0].gender && !requests[0].body && !requests[0].hair);
  KCD2S_CHECK(requests[0].head == std::optional<std::string>("m_head_004"));
  KCD2S_CHECK(requests[0].beard == std::optional<std::string>(""));
  KCD2S_CHECK(requests[1].gender == std::optional<std::string>("male"));
  KCD2S_CHECK(requests[1].hair == std::optional<std::string>("m_hair_005_ginger"));
  KCD2S_CHECK(!requests[1].head && !requests[1].beard && !requests[1].body);
  KCD2S_CHECK(client.take_appearance_requests().empty());

  // BODY_VISIBLE: drawn until a resource asks otherwise; the newest request holds.
  KCD2S_CHECK(client.body_visible());
  KCD2S_CHECK(server.request_body_visible(e, false));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.body_visible());
  KCD2S_CHECK(server.request_body_visible(e, true));
  KCD2S_CHECK(server.request_body_visible(e, false));
  KCD2S_CHECK(!server.request_body_visible(e + 100, true));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.body_visible());
  bool visible = true;
  KCD2S_CHECK(!decode_body_visible({kBodyVisibleTag, 2}, visible));
  KCD2S_CHECK(!decode_body_visible({kBodyVisibleTag}, visible));

  // DISGUISE: the player's own body until a resource asks otherwise; "" puts it back.
  const std::string barrel =
      "objects/manmade/task_specific_props/entertainment/tavern/beer_barrel.cgf";
  KCD2S_CHECK(client.disguise().empty());
  KCD2S_CHECK(server.request_disguise(e, barrel));
  KCD2S_CHECK(!server.request_disguise(e, std::string(kDisguiseMax + 1, 'x')));
  KCD2S_CHECK(!server.request_disguise(e, "line\nbreak"));
  KCD2S_CHECK(!server.request_disguise(e + 100, barrel));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(client.disguise() == barrel);
  KCD2S_CHECK(server.request_disguise(e, ""));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(client.disguise().empty());
  std::string model;
  KCD2S_CHECK(!decode_disguise({kDisguiseTag, 3, 'a', 'b'}, model));
  KCD2S_CHECK(decode_disguise({kDisguiseTag, 0}, model) && model.empty());
}

KCD2S_TEST(appearance, worn_round_trip) {
  // the pieces a player wears outside their inventory.
  std::vector<ItemClassId> items(2);
  items[0][0] = 0xa9;
  items[0][15] = 0x3f;
  items[1][3] = 0x11;
  std::vector<ItemClassId> decoded;
  KCD2S_CHECK(decode_worn(encode_worn(items), decoded) && decoded == items);
  KCD2S_CHECK(decode_worn(encode_worn({}), decoded) && decoded.empty());
  KCD2S_CHECK(encode_worn(std::vector<ItemClassId>(kMaxWorn + 1)).empty());
  auto bytes = encode_worn(items);
  bytes.pop_back();
  KCD2S_CHECK(!decode_worn(bytes, decoded));
}

KCD2S_TEST(appearance, malformed_requests_are_dropped) {
  AppearanceRequest request;
  request.hair = "m_hair_barber_03";
  auto bytes = encode_appearance(request);
  AppearanceRequest decoded;
  KCD2S_CHECK(decode_appearance(bytes, decoded));
  KCD2S_CHECK(decoded.hair == request.hair);

  auto truncated = bytes;
  truncated.pop_back();
  KCD2S_CHECK(!decode_appearance(truncated, decoded));
  auto trailing = bytes;
  trailing.push_back(0);
  KCD2S_CHECK(!decode_appearance(trailing, decoded));
  auto bad_flag = bytes;
  bad_flag[1] = 2;
  KCD2S_CHECK(!decode_appearance(bad_flag, decoded));
  auto control_char = bytes;
  control_char.back() = '\n';
  KCD2S_CHECK(!decode_appearance(control_char, decoded));
}

KCD2S_TEST(appearance, field_round_trip) {
  Appearance appearance{"male", "m_body_tan_03", "m_head_004", "m_hair_005_ginger",
                        "UC_beard_radzig"};
  const std::string field = appearance_field(appearance);
  KCD2S_CHECK(field == "male|m_body_tan_03|m_head_004|m_hair_005_ginger|UC_beard_radzig");
  Appearance parsed;
  KCD2S_CHECK(parse_appearance_field(field, parsed));
  KCD2S_CHECK(parsed == appearance);

  // The default body: every part the game's own.
  KCD2S_CHECK(parse_appearance_field("male||||", parsed));
  KCD2S_CHECK(parsed == Appearance{});

  // Not reported yet, the wrong number of parts, or no known gender.
  KCD2S_CHECK(!parse_appearance_field("", parsed));
  KCD2S_CHECK(!parse_appearance_field("male|||", parsed));
  KCD2S_CHECK(!parse_appearance_field("male|||||", parsed));
  KCD2S_CHECK(!parse_appearance_field("dwarf||||", parsed));
}

KCD2S_TEST(appearance, body_parts_field_round_trip) {
  const BodyParts henry{"henry_body",        "henry", "m_hair_henry", "m_beard_00",
                        "m_underwear01_m01", "male"};
  const std::string field = body_parts_field(henry);
  KCD2S_CHECK(field == "henry_body|henry|m_hair_henry|m_beard_00|m_underwear01_m01|male");
  BodyParts parsed;
  KCD2S_CHECK(parse_body_parts_field(field, parsed));
  KCD2S_CHECK(parsed == henry);
  KCD2S_CHECK(!parse_body_parts_field("", parsed));
  KCD2S_CHECK(parse_body_parts_field(
      "female_body|f_head_000|f_hair_katherine||f_underwear01_m01|female", parsed));
  KCD2S_CHECK(parsed[3].empty() && parsed[5] == "female");
  KCD2S_CHECK(!parse_body_parts_field("a|b|c|d|e", parsed));
  KCD2S_CHECK(!parse_body_parts_field("a|b|c|d|e|dwarf", parsed));
  KCD2S_CHECK(!parse_body_parts_field("a|b|c|d|e f|male", parsed));
}
