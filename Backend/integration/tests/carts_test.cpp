#include "kcd2s/integration/carts.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

CartOrder sample_spawn() {
  CartOrder order;
  order.kind = CartKind::Spawn;
  order.id = 501;
  order.blueprint = "wagon_b_covered";
  order.position = {120.5f, -40.25f, 31.0f};
  order.rotation = {0.0f, 0.0f, 0.7071068f, 0.7071068f};
  order.dimension = 0xffffffffu;
  order.max_speed = 7.0f;
  return order;
}

}  // namespace

KCD2S_TEST(carts, tags_stay_clear_of_other_features) {
  static_assert(kCartTag == 78);
  static_assert(kCartReportTag == 79);
  KCD2S_CHECK(encode_cart(sample_spawn()).front() == 78);
}

KCD2S_TEST(carts, orders_round_trip) {
  CartOrder decoded;
  KCD2S_CHECK(decode_cart(encode_cart(sample_spawn()), decoded));
  KCD2S_CHECK(decoded == sample_spawn());

  for (const CartKind kind : {CartKind::Seat, CartKind::Settled, CartKind::ClimbOff,
                              CartKind::ReinsTaken, CartKind::ReinsGone, CartKind::ClimbOn,
                              CartKind::ScootOff, CartKind::Leave, CartKind::Deny}) {
    CartOrder seat;
    seat.kind = kind;
    seat.id = 501;
    seat.player = 7;
    seat.seat = CartSeat::LeftFront;
    KCD2S_CHECK(decode_cart(encode_cart(seat), decoded));
    KCD2S_CHECK(decoded == seat);
  }

  CartOrder pose;
  pose.kind = CartKind::Pose;
  pose.id = 501;
  pose.position = {1.0f, 2.0f, 3.0f};
  pose.pace = CartPace::Trot;
  pose.player = 8;
  KCD2S_CHECK(decode_cart(encode_cart(pose), decoded));
  KCD2S_CHECK(decoded == pose);

  CartOrder speed;
  speed.kind = CartKind::Speed;
  speed.id = 501;
  speed.max_speed = 12.5f;
  KCD2S_CHECK(decode_cart(encode_cart(speed), decoded));
  KCD2S_CHECK(decoded == speed);

  CartOrder destroy;
  destroy.kind = CartKind::Destroy;
  destroy.id = 501;
  KCD2S_CHECK(decode_cart(encode_cart(destroy), decoded));
  KCD2S_CHECK(decoded == destroy);

  // A load, and none.
  CartOrder load;
  load.kind = CartKind::Load;
  load.id = 501;
  load.blueprint = "goods2";
  KCD2S_CHECK(decode_cart(encode_cart(load), decoded));
  KCD2S_CHECK(decoded == load);
  load.blueprint.clear();
  KCD2S_CHECK(decode_cart(encode_cart(load), decoded));
  KCD2S_CHECK(decoded == load);
  load.blueprint = "bad\nname";
  KCD2S_CHECK(encode_cart(load).empty());

  // The horses in the shafts: a pair with their looks, one with the game's own, and none.
  CartOrder harness;
  harness.kind = CartKind::Harness;
  harness.id = 501;
  harness.looks = {"horse_body_02|horse_head_01|horse_hair_03|horse_tail_01", ""};
  KCD2S_CHECK(decode_cart(encode_cart(harness), decoded));
  KCD2S_CHECK(decoded == harness);
  harness.looks.clear();
  KCD2S_CHECK(decode_cart(encode_cart(harness), decoded));
  KCD2S_CHECK(decoded == harness);
  KCD2S_CHECK(decoded.looks.empty());
  harness.looks = {"a", "b", "c"};
  KCD2S_CHECK(encode_cart(harness).empty());
  harness.looks = {"bad\nlook"};
  KCD2S_CHECK(encode_cart(harness).empty());
  harness.looks = {std::string(kMaxCartLookBytes + 1, 'x')};
  KCD2S_CHECK(encode_cart(harness).empty());
}

KCD2S_TEST(carts, orders_refuse_bad_values) {
  CartOrder order = sample_spawn();
  order.blueprint.clear();
  KCD2S_CHECK(encode_cart(order).empty());
  order = sample_spawn();
  order.max_speed = kMaxCartSpeed + 1.0f;
  KCD2S_CHECK(encode_cart(order).empty());
  order = sample_spawn();
  order.position[0] = INFINITY;
  KCD2S_CHECK(encode_cart(order).empty());
  CartOrder seat;
  seat.kind = CartKind::Seat;
  seat.id = 501;
  KCD2S_CHECK(encode_cart(seat).empty());  // no player
  CartOrder decoded;
  KCD2S_CHECK(!decode_cart({kCartTag, 99, 1}, decoded));
  KCD2S_CHECK(!decode_cart({kCartReportTag, 2, 1}, decoded));
  auto cut = encode_cart(sample_spawn());
  cut.resize(cut.size() - 2);
  KCD2S_CHECK(!decode_cart(cut, decoded));
}

KCD2S_TEST(carts, reports_round_trip) {
  CartReport enter;
  enter.kind = CartReportKind::Enter;
  enter.cart = 501;
  enter.seat = CartSeat::Bench;
  CartReport decoded;
  KCD2S_CHECK(decode_cart_report(encode_cart_report(enter), decoded));
  KCD2S_CHECK(decoded == enter);

  CartReport drive;
  drive.kind = CartReportKind::Drive;
  drive.cart = 501;
  drive.position = {4.0f, 5.0f, 6.0f};
  drive.rotation = {0.0f, 0.0f, 1.0f, 0.0f};
  drive.pace = CartPace::Reverse;
  KCD2S_CHECK(decode_cart_report(encode_cart_report(drive), decoded));
  KCD2S_CHECK(decoded == drive);

  CartReport leave;
  leave.kind = CartReportKind::Leave;
  leave.cart = 501;
  KCD2S_CHECK(decode_cart_report(encode_cart_report(leave), decoded));
  KCD2S_CHECK(decoded == leave);

  KCD2S_CHECK(!decode_cart_report({kCartReportTag, 1, 1, 9}, decoded));  // no seat 9
  KCD2S_CHECK(!decode_cart_report({kCartTag, 2, 1}, decoded));
}

KCD2S_TEST(carts, names_match_the_server_catalog) {
  KCD2S_CHECK(cart_seat_of("rightBack") == CartSeat::RightBack);
  KCD2S_CHECK(!cart_seat_of("roof"));
  KCD2S_CHECK(std::string(cart_seat_name(CartSeat::LeftFront)) == "leftFront");
  KCD2S_CHECK(cart_pace_of("trot") == CartPace::Trot);
  KCD2S_CHECK(std::string(cart_pace_name(CartPace::Reverse)) == "reverse");
}
