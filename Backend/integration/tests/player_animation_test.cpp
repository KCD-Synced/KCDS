#include "kcd2s/integration/player_animation.hpp"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/animation_props.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

AnimationOrder play_order() {
  AnimationOrder order;
  order.kind = AnimationOrder::Kind::Play;
  order.request = 4097;
  order.fragment = "Wave";
  order.tags = "r_bucket+sittingNoTable";
  order.loop = true;
  order.lock_movement = true;
  return order;
}

AnimationProp model_prop() {
  AnimationProp prop;
  prop.model = "objects/manmade/common_furniture/bucket/bucket_a_a.cgf";
  return prop;
}

AnimationProp item_prop() {
  AnimationProp prop;
  Guid item{};
  for (std::size_t i = 0; i < item.size(); ++i) {
    item[i] = std::uint8_t(0x10 + i);
  }
  prop.item = item;
  prop.left_hand = true;
  prop.joint = "hand_L_end";
  prop.position = std::array<float, 3>{0.1f, -0.25f, 2.0f};
  prop.rotation = std::array<float, 4>{0.0f, 0.70710678f, 0.0f, 0.70710678f};
  return prop;
}

AnimationOrder stop_order(std::uint64_t request, bool stand_up) {
  AnimationOrder order;
  order.kind = AnimationOrder::Kind::Stop;
  order.request = request;
  order.stand_up = stand_up;
  return order;
}

bool decodes_to(const AnimationOrder& order) {
  AnimationOrder decoded;
  return decode_animation_order(encode_animation_order(order), decoded) && decoded == order;
}

}  // namespace

KCD2S_TEST(player_animation, play_orders_round_trip) {
  AnimationOrder order = play_order();
  KCD2S_CHECK(decodes_to(order));
  order.props = {model_prop(), item_prop()};
  KCD2S_CHECK(decodes_to(order));
  // A prop that follows the animation.
  order.props[1].from_animation = true;
  KCD2S_CHECK(decodes_to(order));
  // ... that rests in the world as a server Prop before and after.
  order.props[1].rest = 4242;
  KCD2S_CHECK(decodes_to(order));
  order.props[1].from_animation = false;
  KCD2S_CHECK(encode_animation_order(order).empty() || !decodes_to(order));
  order.props[1].rest = 0;
  // A prop held until the animation puts it down, never with from_animation.
  order.props[1].until_put_down = true;
  KCD2S_CHECK(decodes_to(order));
  order.props[1].from_animation = true;
  KCD2S_CHECK(!decodes_to(order));
  order.props[1].from_animation = false;
  order.props[1].until_put_down = false;
  // A request that only holds props and tags has no fragment; a clip stands in for the fragment;
  // a restore puts state back without playing it.
  AnimationOrder props_only;
  props_only.request = 1;
  props_only.props = {item_prop()};
  KCD2S_CHECK(decodes_to(props_only));
  AnimationOrder clip;
  clip.request = 2;
  clip.clip = "animations/myresource/wave.caf";
  clip.restore = true;
  KCD2S_CHECK(decodes_to(clip));
  AnimationOrder largest = play_order();
  largest.request = kAnimationRequestMax;
  largest.fragment = std::string(kAnimationFragmentMax, 'f');
  largest.tags = std::string(kAnimationTagsMax, 't');
  KCD2S_CHECK(decodes_to(largest));
  AnimationOrder longest_clip = clip;
  longest_clip.clip = std::string(kAnimationClipMax, 'c');
  KCD2S_CHECK(decodes_to(longest_clip));
  // Only the upper body plays it.
  AnimationOrder upper = clip;
  upper.restore = false;
  upper.upper_body = true;
  upper.loop = true;
  KCD2S_CHECK(decodes_to(upper));
  // The body without gravity while it runs.
  AnimationOrder hanged = play_order();
  hanged.no_gravity = true;
  KCD2S_CHECK(decodes_to(hanged));
  // Paired with another player.
  AnimationOrder paired = play_order();
  paired.partner = 7;
  KCD2S_CHECK(decodes_to(paired));
  // Its slave: placed at the master's by the clips' start locations; a slave needs a partner.
  paired.paired_slave = true;
  KCD2S_CHECK(decodes_to(paired));
  paired.partner = 0;
  KCD2S_CHECK(encode_animation_order(paired).empty());
  // The body keeps its heading.
  AnimationOrder free_look = play_order();
  free_look.lock_rotation = true;
  KCD2S_CHECK(decodes_to(free_look));
  // Its look limits; out of range they do not travel.
  free_look.look_yaw = 40;
  free_look.look_up = 10;
  free_look.look_down = 55;
  KCD2S_CHECK(decodes_to(free_look));
  free_look.look_up = 90;
  KCD2S_CHECK(encode_animation_order(free_look).empty());
  // A speed and a start time; out of range they do not travel.
  AnimationOrder timed = play_order();
  timed.speed = 0.5f;
  timed.start = 1.5f;
  KCD2S_CHECK(decodes_to(timed));
  timed.speed = 11.0f;
  KCD2S_CHECK(encode_animation_order(timed).empty());
  timed.speed = 1.0f;
  timed.start = -1.0f;
  KCD2S_CHECK(encode_animation_order(timed).empty());
  // The running request's speed and props.
  AnimationOrder speed;
  speed.kind = AnimationOrder::Kind::Speed;
  speed.request = 7;
  speed.speed = 0.0f;
  KCD2S_CHECK(decodes_to(speed));
  speed.request = 0;
  KCD2S_CHECK(encode_animation_order(speed).empty());
  speed.request = 7;
  speed.fragment = "Wave";
  KCD2S_CHECK(encode_animation_order(speed).empty());
  AnimationOrder props;
  props.kind = AnimationOrder::Kind::Props;
  props.request = 7;
  AnimationProp bucket;
  bucket.model = "objects/bucket.cgf";
  bucket.position = std::array<float, 3>{0.1f, 0.0f, 0.0f};
  props.props.push_back(bucket);
  KCD2S_CHECK(decodes_to(props));
  props.props.clear();
  KCD2S_CHECK(decodes_to(props));
  props.speed = 2.0f;
  KCD2S_CHECK(encode_animation_order(props).empty());
}

// The upper-body mark of the replicated animation field: "U" in front, or alone.
KCD2S_TEST(player_animation, upper_body_field_marks_and_splits) {
  bool upper = false;
  std::string rest;
  split_upper_body_field(upper_body_field(true, "props"), upper, rest);
  KCD2S_CHECK(upper && rest == "props");
  split_upper_body_field(upper_body_field(true, ""), upper, rest);
  KCD2S_CHECK(upper && rest.empty());
  split_upper_body_field(upper_body_field(false, "props"), upper, rest);
  KCD2S_CHECK(!upper && rest == "props");
  // A field that only starts with a U (a prop's name) is no mark.
  split_upper_body_field("Upright", upper, rest);
  KCD2S_CHECK(!upper && rest == "Upright");
}

// The speed mark of the replicated animation field: "S" and the mark in front.
KCD2S_TEST(player_animation, speed_field_marks_and_splits) {
  std::string mark;
  std::string rest;
  split_speed_field(speed_field("0.50:0:0.42", "U props"), mark, rest);
  KCD2S_CHECK(mark == "0.50:0:0.42" && rest == "U props");
  split_speed_field(speed_field("0.00:12:1.00", ""), mark, rest);
  KCD2S_CHECK(mark == "0.00:12:1.00" && rest.empty());
  split_speed_field(speed_field("", "props"), mark, rest);
  KCD2S_CHECK(mark.empty() && rest == "props");
  // A field that only starts with an S (a prop's name) is no mark.
  split_speed_field("Sack", mark, rest);
  KCD2S_CHECK(mark.empty() && rest == "Sack");
  float speed = 0.0f;
  unsigned layer = 0;
  float place = 0.0f;
  KCD2S_CHECK(parse_speed_mark("0.50:12:0.42", speed, layer, place) && speed == 0.5f &&
              layer == 12 && place == 0.42f);
  KCD2S_CHECK(!parse_speed_mark("0.50:16:0.42", speed, layer, place));
  KCD2S_CHECK(!parse_speed_mark("0.50:0:1.5", speed, layer, place));
  KCD2S_CHECK(!parse_speed_mark("21:0:0.5", speed, layer, place));
  KCD2S_CHECK(!parse_speed_mark("0.50:0", speed, layer, place));
  KCD2S_CHECK(!parse_speed_mark("x:0:0.5", speed, layer, place));
}

// The pointing mark of the replicated animation field: "P" and the point in front.
KCD2S_TEST(player_animation, pointing_field_marks_and_splits) {
  std::string mark;
  std::string rest;
  split_pointing_field(pointing_field("1430.5,-2580.2,64.1", "U props"), mark, rest);
  KCD2S_CHECK(mark == "1430.5,-2580.2,64.1" && rest == "U props");
  split_pointing_field(pointing_field("", "props"), mark, rest);
  KCD2S_CHECK(mark.empty() && rest == "props");
  // A field that only starts with a P (a prop's name) is no mark.
  split_pointing_field("Pail", mark, rest);
  KCD2S_CHECK(mark.empty() && rest == "Pail");
  float point[3] = {};
  KCD2S_CHECK(parse_pointing_mark("1430.5,-2580.2,64.1", point) && point[0] == 1430.5f &&
              point[1] == -2580.2f && point[2] == 64.1f);
  KCD2S_CHECK(!parse_pointing_mark("1430.5,2580.2", point));
  KCD2S_CHECK(!parse_pointing_mark("1430.5,2580.2,64.1,1", point));
  KCD2S_CHECK(!parse_pointing_mark("1e7,0,0", point));
  KCD2S_CHECK(!parse_pointing_mark("x,0,0", point));
}

KCD2S_TEST(player_animation, stop_orders_round_trip) {
  KCD2S_CHECK(decodes_to(stop_order(0, false)));
  KCD2S_CHECK(decodes_to(stop_order(9, true)));
  KCD2S_CHECK(decodes_to(stop_order(kAnimationRequestMax, true)));
}

// Player.setExpression: a face order carries a fragment (empty: the game's
// face again) and tags, nothing else.
KCD2S_TEST(player_animation, face_orders_round_trip) {
  AnimationOrder face;
  face.kind = AnimationOrder::Kind::Face;
  face.fragment = "FE_DialogueSpeaking";
  face.tags = "happy";
  KCD2S_CHECK(decodes_to(face));
  face.fragment.clear();
  face.tags.clear();
  KCD2S_CHECK(decodes_to(face));
  face.request = 3;
  KCD2S_CHECK(encode_animation_order(face).empty());
  face.request = 0;
  face.loop = true;
  KCD2S_CHECK(encode_animation_order(face).empty());
  face.loop = false;
  face.clip = "humans/male/x";
  KCD2S_CHECK(encode_animation_order(face).empty());
}

// Player.setSpeaking: a talk order carries one of the known styles, or none for not
// talking, and nothing else.
KCD2S_TEST(player_animation, talk_orders_round_trip) {
  AnimationOrder talk;
  talk.kind = AnimationOrder::Kind::Talk;
  for (const char* style : {"neutral", "happy", "drunk", "chew", ""}) {
    talk.fragment = style;
    KCD2S_CHECK(decodes_to(talk));
  }
  talk.fragment = "whisper";
  KCD2S_CHECK(encode_animation_order(talk).empty());
  talk.fragment = "neutral";
  talk.tags = "happy";
  KCD2S_CHECK(encode_animation_order(talk).empty());
  talk.tags.clear();
  talk.request = 1;
  KCD2S_CHECK(encode_animation_order(talk).empty());
  // A known kind with an unknown style does not decode.
  AnimationOrder face;
  face.kind = AnimationOrder::Kind::Face;
  face.fragment = "whisper";
  std::vector<std::uint8_t> bytes = encode_animation_order(face);
  KCD2S_CHECK(!bytes.empty());
  bytes[1] = std::uint8_t(AnimationOrder::Kind::Talk);
  AnimationOrder decoded;
  KCD2S_CHECK(!decode_animation_order(bytes, decoded));
}

KCD2S_TEST(player_animation, orders_the_server_would_not_send_are_refused) {
  const auto refused = [](const AnimationOrder& order) {
    return encode_animation_order(order).empty();
  };
  AnimationOrder order = play_order();
  KCD2S_CHECK(!refused(order));
  order.request = 0;
  KCD2S_CHECK(refused(order));
  order = play_order();
  order.request = kAnimationRequestMax + 1;
  KCD2S_CHECK(refused(order));
  // Nothing to play.
  order = play_order();
  order.fragment.clear();
  order.tags.clear();
  KCD2S_CHECK(refused(order));
  // A clip takes the place of the fragment.
  order = play_order();
  order.clip = "animations/a.caf";
  KCD2S_CHECK(refused(order));
  // Bounds and characters of the texts.
  order = play_order();
  order.fragment = std::string(kAnimationFragmentMax + 1, 'f');
  KCD2S_CHECK(refused(order));
  order = play_order();
  order.tags = std::string(kAnimationTagsMax + 1, 't');
  KCD2S_CHECK(refused(order));
  order = play_order();
  order.fragment.clear();
  order.clip = std::string(kAnimationClipMax + 1, 'c');
  KCD2S_CHECK(refused(order));
  order = play_order();
  order.fragment = "Wave Hello";
  KCD2S_CHECK(refused(order));
  order.fragment = "Wave\n";
  KCD2S_CHECK(refused(order));
  order.fragment = "W\xc3\xa1ve";
  KCD2S_CHECK(refused(order));
  // Props: at most two, each an item or a model, with sane offsets.
  order = play_order();
  order.props = {model_prop(), model_prop(), model_prop()};
  KCD2S_CHECK(refused(order));
  order.props = {AnimationProp{}};
  KCD2S_CHECK(refused(order));
  AnimationProp both = item_prop();
  both.model = "objects/a.cgf";
  order.props = {both};
  KCD2S_CHECK(refused(order));
  AnimationProp far = model_prop();
  far.position = std::array<float, 3>{0.0f, kAnimationOffsetMax + 1.0f, 0.0f};
  order.props = {far};
  KCD2S_CHECK(refused(order));
  far.position = std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
  order.props = {far};
  KCD2S_CHECK(refused(order));
  AnimationProp tilted = model_prop();
  tilted.rotation = std::array<float, 4>{0.0f, 0.0f, 0.0f, 2.0f};
  order.props = {tilted};
  KCD2S_CHECK(refused(order));
  tilted.rotation = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  order.props = {tilted};
  KCD2S_CHECK(refused(order));
  // The game's joints have spaces: one travels, in the order and in the field; '|',
  // the field's stand-in for a space, does not.
  AnimationProp spaced = model_prop();
  spaced.joint = "Bip01 Neck";
  order.props = {spaced};
  KCD2S_CHECK(!refused(order));
  const std::string spaced_field = animation_props_field({spaced});
  KCD2S_CHECK(spaced_field.find("JBip01|Neck") != std::string::npos);
  std::vector<AnimationProp> spaced_back;
  KCD2S_CHECK(parse_animation_props_field(spaced_field, spaced_back) && spaced_back.size() == 1 &&
              spaced_back[0].joint == "Bip01 Neck");
  spaced.joint = "Bip01|Neck";
  order.props = {spaced};
  KCD2S_CHECK(refused(order));
  // A stop carries a stand-up request exactly when it asks for the stand-up, and nothing else.
  KCD2S_CHECK(refused(stop_order(0, true)));
  KCD2S_CHECK(refused(stop_order(3, false)));
  KCD2S_CHECK(refused(stop_order(kAnimationRequestMax + 1, true)));
  AnimationOrder stop = stop_order(3, true);
  stop.fragment = "Wave";
  KCD2S_CHECK(refused(stop));
  AnimationOrder unknown = play_order();
  unknown.kind = AnimationOrder::Kind(3);
  KCD2S_CHECK(refused(unknown));
}

KCD2S_TEST(player_animation, malformed_orders_do_not_decode) {
  AnimationOrder order = play_order();
  order.props = {model_prop(), item_prop()};
  const auto message = encode_animation_order(order);
  AnimationOrder decoded;
  KCD2S_CHECK(decode_animation_order(message, decoded));
  // Every truncation fails, and so does a trailing byte.
  for (std::size_t length = 0; length < message.size(); ++length) {
    const std::vector<std::uint8_t> cut(message.begin(), message.begin() + length);
    KCD2S_CHECK(!decode_animation_order(cut, decoded));
  }
  auto longer = message;
  longer.push_back(0);
  KCD2S_CHECK(!decode_animation_order(longer, decoded));
  // Another tag, an unknown kind, flag bits nobody defined.
  auto other = message;
  other[0] = kAnimationEndTag;
  KCD2S_CHECK(!decode_animation_order(other, decoded));
  other = message;
  other[1] = 3;
  KCD2S_CHECK(!decode_animation_order(other, decoded));
  other = message;
  other[1] = 0;
  KCD2S_CHECK(!decode_animation_order(other, decoded));
  // The flags byte follows the two-byte request 4097.
  other = message;
  other[4] |= 0x80;
  KCD2S_CHECK(!decode_animation_order(other, decoded));
  auto stop = encode_animation_order(stop_order(9, true));
  stop[3] = 2;
  KCD2S_CHECK(!decode_animation_order(stop, decoded));
  stop = encode_animation_order(stop_order(9, true));
  stop[3] = 0;
  KCD2S_CHECK(!decode_animation_order(stop, decoded));
  // A text that claims more bytes than the message holds.
  const std::vector<std::uint8_t> huge = {kAnimationOrderTag, 1, 1, 0, 0x7F, 'a'};
  KCD2S_CHECK(!decode_animation_order(huge, decoded));
  // Three props.
  AnimationOrder two = play_order();
  two.fragment.clear();
  two.tags = "t";
  two.loop = false;
  two.lock_movement = false;
  two.props = {model_prop(), model_prop()};
  auto three = encode_animation_order(two);
  KCD2S_CHECK(decode_animation_order(three, decoded));
  // The prop count follows the tag and kind (2 bytes), the request (2), the flags (1), the empty
  // fragment (1), the empty clip (1) and the tags "t" (2).
  const std::size_t count_at = 2 + 2 + 1 + 1 + 1 + 2;
  KCD2S_CHECK_EQ(unsigned(three[count_at]), 2u);
  three[count_at] = 3;
  KCD2S_CHECK(!decode_animation_order(three, decoded));
  // A decoder keeps what it had when it refuses.
  AnimationOrder kept = play_order();
  AnimationOrder untouched = kept;
  KCD2S_CHECK(!decode_animation_order(longer, kept));
  KCD2S_CHECK(kept == untouched);
}

KCD2S_TEST(player_animation, ends_round_trip_and_refuse_what_a_client_would_not_send) {
  for (const auto reason :
       {AnimationReason::Finished, AnimationReason::Interrupted, AnimationReason::Refused,
        AnimationReason::Stopped, AnimationReason::Replaced}) {
    for (const auto exit : {AnimationExit::None, AnimationExit::StandUp, AnimationExit::GetUp}) {
      AnimationEnd end;
      end.request = 123456789012ull;
      end.reason = reason;
      end.exit = exit;
      AnimationEnd decoded;
      KCD2S_CHECK(decode_animation_end(encode_animation_end(end), decoded));
      KCD2S_CHECK(decoded == end);
    }
  }
  AnimationEnd end;
  end.request = kAnimationRequestMax;
  AnimationEnd decoded;
  KCD2S_CHECK(decode_animation_end(encode_animation_end(end), decoded));
  KCD2S_CHECK(decoded == end);

  end.request = 0;
  KCD2S_CHECK(encode_animation_end(end).empty());
  end.request = kAnimationRequestMax + 1;
  KCD2S_CHECK(encode_animation_end(end).empty());
  end.request = 5;
  end.reason = AnimationReason(0);
  KCD2S_CHECK(encode_animation_end(end).empty());
  end.reason = AnimationReason(6);
  KCD2S_CHECK(encode_animation_end(end).empty());
  end.reason = AnimationReason::Finished;
  end.exit = AnimationExit(3);
  KCD2S_CHECK(encode_animation_end(end).empty());

  const std::vector<std::uint8_t> good = {kAnimationEndTag, 5, 1, 0};
  KCD2S_CHECK(decode_animation_end(good, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 0, 1, 0}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 5, 0, 0}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 5, 6, 0}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 5, 1, 3}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 5, 1}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationEndTag, 5, 1, 0, 0}, decoded));
  KCD2S_CHECK(!decode_animation_end({kAnimationOrderTag, 5, 1, 0}, decoded));
  KCD2S_CHECK(!decode_animation_end({}, decoded));
  // A request id of 2^53, one past the largest a script can hold exactly.
  const std::vector<std::uint8_t> too_large = {
      kAnimationEndTag, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x10, 1, 0};
  KCD2S_CHECK(!decode_animation_end(too_large, decoded));
}

namespace {

// One server and one client over a loopback link; the client owns `player`.
struct Link {
  replication::SchemaRegistry schema;
  Server server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  core::EntityId player = 0;
  core::PeerId peer = 0;
  ClientPeer client;
  double now = 0;

  static replication::SchemaRegistry make_schema() {
    replication::SchemaRegistry schema;
    schema.define("Player").field("position", replication::FieldKind::Vec3,
                                  replication::Priority::High, replication::Authority::Owner);
    return schema;
  }

  Link()
      : schema(make_schema()),
        server(schema),
        links(transport::make_loopback_pair()),
        client(schema, links.second.get()) {
    server.world().set_tick(1);
    player = server.world().spawn(core::type_id("Player"));
    server.world().write(player, 0, core::Vec3{});
    peer = server.add_client(links.first.get(), player, 10000);
    WorldConfig config;
    server.configure_world(peer, config);
    server.set_owner(player, peer);
    run(8);
  }

  void run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1.0 / 60;
      server.world().set_tick(server.world().tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  }
};

}  // namespace

KCD2S_TEST(player_animation, orders_reach_the_owner_and_ends_reach_the_server) {
  Link link;
  AnimationOrder order = play_order();
  order.props = {model_prop()};
  // No world yet: nothing travels either way.
  KCD2S_CHECK(!link.server.send_animation_order(link.player, order));
  KCD2S_CHECK(!link.client.send_animation_end({7, AnimationReason::Finished}));

  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_animation_order(link.player, order));
  KCD2S_CHECK(link.server.send_animation_order(link.player, stop_order(8, true)));
  // Unknown player, an order the codec refuses.
  KCD2S_CHECK(!link.server.send_animation_order(link.player + 1, order));
  AnimationOrder empty;
  KCD2S_CHECK(!link.server.send_animation_order(link.player, empty));
  link.run(5);
  const auto orders = link.client.take_animation_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(2));
  if (orders.size() == 2) {
    KCD2S_CHECK(orders[0] == order);
    KCD2S_CHECK(orders[1] == stop_order(8, true));
  }
  KCD2S_CHECK(link.client.take_animation_orders().empty());

  KCD2S_CHECK(link.client.send_animation_end({4097, AnimationReason::Interrupted}));
  KCD2S_CHECK(link.client.send_animation_end({8, AnimationReason::Finished, AnimationExit::GetUp}));
  KCD2S_CHECK(!link.client.send_animation_end({0, AnimationReason::Finished}));
  link.run(5);
  const auto ends = link.server.take_animation_ends();
  KCD2S_CHECK_EQ(ends.size(), std::size_t(2));
  if (ends.size() == 2) {
    KCD2S_CHECK_EQ(ends[0].first, link.player);
    KCD2S_CHECK(ends[0].second == AnimationEnd({4097, AnimationReason::Interrupted}));
    KCD2S_CHECK(ends[1].second ==
                AnimationEnd({8, AnimationReason::Finished, AnimationExit::GetUp}));
  }
  KCD2S_CHECK(link.server.take_animation_ends().empty());
}

KCD2S_TEST(player_animation, a_burst_is_bounded_on_both_sides) {
  Link link;
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  // A client keeps the newest orders; the oldest are dropped.
  for (std::uint64_t request = 1; request <= kMaxAnimationOrders + 6; ++request) {
    AnimationOrder order = play_order();
    order.request = request;
    KCD2S_CHECK(link.server.send_animation_order(link.player, order));
  }
  link.run(10);
  const auto orders = link.client.take_animation_orders();
  KCD2S_CHECK_EQ(orders.size(), kMaxAnimationOrders);
  if (orders.size() == kMaxAnimationOrders) {
    KCD2S_CHECK_EQ(orders.front().request, std::uint64_t(7));
    KCD2S_CHECK_EQ(orders.back().request, std::uint64_t(kMaxAnimationOrders + 6));
  }
  // The server takes at most kMaxAnimationEndsPerSecond reports of a client per second.
  for (std::uint64_t request = 1; request <= kMaxAnimationEndsPerSecond + 8; ++request) {
    KCD2S_CHECK(link.client.send_animation_end({request, AnimationReason::Finished}));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_animation_ends().size(), std::size_t(kMaxAnimationEndsPerSecond));
}

KCD2S_TEST(player_animation, a_new_world_drops_the_orders_of_the_old_one) {
  Link link;
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_animation_order(link.player, play_order()));
  link.run(5);
  // The order arrived but the game did not take it before the world changed.
  WorldConfig next;
  next.revision = 2;
  KCD2S_CHECK(link.server.configure_world(link.peer, next));
  link.run(5);
  KCD2S_CHECK(link.client.take_animation_orders().empty());
}

// The props of the request a body holds, as the NetworkPlayer field "animationProps" carries them
// to every client (kcd2s/integration/animation_props.hpp).
KCD2S_TEST(player_animation, props_field_round_trip) {
  std::vector<AnimationProp> parsed{model_prop()};
  KCD2S_CHECK(animation_props_field({}).empty());
  KCD2S_CHECK(parse_animation_props_field("", parsed) && parsed.empty());

  const AnimationProp model = model_prop();
  KCD2S_CHECK(animation_props_field({model}) ==
              "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J");
  KCD2S_CHECK(parse_animation_props_field(animation_props_field({model}), parsed) &&
              parsed == std::vector<AnimationProp>{model});

  // The offsets go to a tenth of a millimetre; these are exact at that.
  AnimationProp item = item_prop();
  item.rotation = std::array<float, 4>{0.0f, 0.6f, 0.0f, 0.8f};
  const std::string both = animation_props_field({model, item});
  KCD2S_CHECK(both ==
              "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J L "
              "I13121110-1514-1716-1819-1a1b1c1d1e1f Jhand_L_end P0.1000,-0.2500,2.0000 "
              "Q0.0000,0.6000,0.0000,0.8000");
  KCD2S_CHECK(parse_animation_props_field(both, parsed) &&
              parsed == (std::vector<AnimationProp>{model, item}));

  // A prop that follows the animation ends with "A".
  AnimationProp follows = model_prop();
  follows.from_animation = true;
  const std::string followed = animation_props_field({follows, model});
  KCD2S_CHECK(followed ==
              "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J A "
              "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J");
  KCD2S_CHECK(parse_animation_props_field(followed, parsed) &&
              parsed == (std::vector<AnimationProp>{follows, model}));
  // ... and one resting as a server Prop with "S<id>" after it.
  follows.rest = 77;
  const std::string resting = animation_props_field({follows});
  KCD2S_CHECK(resting == "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J A S77");
  KCD2S_CHECK(parse_animation_props_field(resting, parsed) &&
              parsed == (std::vector<AnimationProp>{follows}));
  // .. and one held until the animation puts it down with "D".
  AnimationProp until = model_prop();
  until.until_put_down = true;
  const std::string held = animation_props_field({until});
  KCD2S_CHECK(held == "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J D");
  KCD2S_CHECK(parse_animation_props_field(held, parsed) &&
              parsed == (std::vector<AnimationProp>{until}));
  KCD2S_CHECK(!parse_animation_props_field(
      "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J A D", parsed));
  KCD2S_CHECK(!parse_animation_props_field(
      "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J S77", parsed));
  KCD2S_CHECK(!parse_animation_props_field(
      "R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J A S0", parsed));
}

KCD2S_TEST(player_animation, prop_cues_round_trip) {
  AnimationPropCue cue;
  cue.request = 9;
  cue.prop = 1;
  cue.taken = true;
  AnimationPropCue decoded;
  KCD2S_CHECK(decode_animation_prop_cue(encode_animation_prop_cue(cue), decoded) &&
              decoded.request == 9 && decoded.prop == 1 && decoded.taken);
  cue.taken = false;
  KCD2S_CHECK(decode_animation_prop_cue(encode_animation_prop_cue(cue), decoded) && !decoded.taken);
  cue.request = 0;
  KCD2S_CHECK(encode_animation_prop_cue(cue).empty());
  cue.request = 9;
  cue.prop = std::uint8_t(kAnimationPropsMax);
  KCD2S_CHECK(encode_animation_prop_cue(cue).empty());
  KCD2S_CHECK(!decode_animation_prop_cue({kAnimationPropCueTag}, decoded));
}

KCD2S_TEST(player_animation, props_field_refuses_what_it_does_not_write) {
  std::vector<AnimationProp> parsed;
  const char* const refused[] = {
      " R Mobjects/a.cgf J",                 // a space at the start
      "R Mobjects/a.cgf J ",                 // and at the end
      "R  Mobjects/a.cgf J",                 // two spaces
      "X Mobjects/a.cgf J",                  // no hand
      "R M J",                               // no mesh
      "R Iabc J",                            // not a GUID
      "R Mobjects/a.cgf",                    // no joint
      "R Mobjects/a.cgf J P1,2",             // two numbers for three
      "R Mobjects/a.cgf J P1,2,3,4",         // four
      "R Mobjects/a.cgf J P101,0,0",         // beyond 100 m
      "R Mobjects/a.cgf J Q0,0,0,2",         // not a unit rotation
      "R Mobjects/a.cgf J Q0,0,0,1 P0,0,0",  // the offset after the rotation
      // Three props.
      "R Mobjects/a.cgf J R Mobjects/b.cgf J R Mobjects/c.cgf J",
  };
  for (const char* field : refused) {
    parsed = {model_prop()};
    KCD2S_CHECK(!parse_animation_props_field(field, parsed) && parsed.empty());
  }
  // More props than an order holds, or one an order could not carry, make no field at all.
  KCD2S_CHECK(animation_props_field({model_prop(), model_prop(), model_prop()}).empty());
  AnimationProp both = model_prop();
  both.item = Guid{};
  KCD2S_CHECK(animation_props_field({both}).empty());
}

// A clip a resource ships goes to every client in the same field, ahead of the props, since its id
// in a body's animation set is each client's own.
KCD2S_TEST(player_animation, shipped_clip_field_round_trip) {
  ShippedClip shipped;
  KCD2S_CHECK(shipped_clip_of("@dance/clips/wave.caf", shipped) &&
              shipped.clip == "dance/clips/wave.caf");
  KCD2S_CHECK(!shipped_clip_of("reading_stand_inv_in", shipped));
  KCD2S_CHECK(!shipped_clip_of("animations/human/wave.caf", shipped));
  const char* const not_shipped[] = {"@",
                                     "@dance",
                                     "@dance/",
                                     "@/wave.caf",
                                     "@dance/../x.caf",
                                     "@dance/./x.caf",
                                     "@dance//x.caf",
                                     "@dance/a b.caf",
                                     "@dance/c:x.caf",
                                     "@dance\\x.caf"};
  for (const char* clip : not_shipped) {
    KCD2S_CHECK(!shipped_clip_of(clip, shipped));
  }

  const ShippedClip loop{"dance/clips/wave.caf", true};
  std::optional<ShippedClip> clip;
  std::string props_field;
  KCD2S_CHECK(animation_field(loop, {}) == "C1dance/clips/wave.caf");
  KCD2S_CHECK(split_animation_field("C1dance/clips/wave.caf", clip, props_field) && clip == loop &&
              props_field.empty());

  const AnimationProp model = model_prop();
  const std::string with_props = animation_field(ShippedClip{"dance/wave.caf", false}, {model});
  KCD2S_CHECK(with_props ==
              "C0dance/wave.caf R Mobjects/manmade/common_furniture/bucket/bucket_a_a.cgf J");
  KCD2S_CHECK(split_animation_field(with_props, clip, props_field) &&
              clip == (ShippedClip{"dance/wave.caf", false}) &&
              props_field == animation_props_field({model}));

  // Without a clip the field is the props' own, as before.
  KCD2S_CHECK(animation_field(std::nullopt, {model}) == animation_props_field({model}));
  KCD2S_CHECK(split_animation_field(animation_props_field({model}), clip, props_field) && !clip &&
              props_field == animation_props_field({model}));
  KCD2S_CHECK(animation_field(std::nullopt, {}).empty());

  const char* const refused[] = {
      "C", "C1", "C2dance/x.caf", "C1dance", "C1dance/../x.caf", "C1dance/x.caf "};
  for (const char* field : refused) {
    KCD2S_CHECK(!split_animation_field(field, clip, props_field));
  }
  KCD2S_CHECK(animation_field(ShippedClip{"no-folder", true}, {}).empty());
}

// Player.setSpeaking: the owner's talking style leads the field, every client reads
// it off and the rest stays the shipped clip and props it was.
KCD2S_TEST(player_animation, talking_field_round_trip) {
  std::string style;
  std::string rest;
  KCD2S_CHECK(talking_field("", "C1dance/wave.caf") == "C1dance/wave.caf");
  KCD2S_CHECK(talking_field("drunk", "") == "Tdrunk");
  const std::string field = talking_field("happy", "C1dance/wave.caf");
  KCD2S_CHECK(field == "Thappy C1dance/wave.caf");
  KCD2S_CHECK(split_talking_field(field, style, rest) && style == "happy" &&
              rest == "C1dance/wave.caf");
  KCD2S_CHECK(split_talking_field("Tchew", style, rest) && style == "chew" && rest.empty());
  KCD2S_CHECK(split_talking_field("C1dance/wave.caf", style, rest) && style.empty() &&
              rest == "C1dance/wave.caf");
  // An unknown style is not written and not read.
  KCD2S_CHECK(talking_field("whisper", "x") == "x");
  KCD2S_CHECK(!split_talking_field("Twhisper", style, rest));
  KCD2S_CHECK(!split_talking_field("Tneutral ", style, rest));
}
