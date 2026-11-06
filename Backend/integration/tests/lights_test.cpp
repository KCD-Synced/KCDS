#include "kcd2s/integration/lights.hpp"

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// LIGHT.

namespace {

LightOrder lantern() {
  LightOrder order;
  order.id = 4097;
  order.position = {2293.2f, 2026.0f, 106.5f};
  order.color = {1.0f, 0.55f, 0.2f};
  order.intensity = 2.5f;
  order.radius = 8.0f;
  order.cast_shadows = true;
  return order;
}

}  // namespace

KCD2S_TEST(lights, orders_round_trip) {
  LightOrder spawn = lantern();
  LightOrder decoded;
  KCD2S_CHECK(decode_light(encode_light(spawn), decoded) && decoded == spawn);

  LightOrder update = lantern();
  update.kind = LightKind::Update;
  update.cast_shadows = false;
  update.radius = kLightRadiusMax;
  KCD2S_CHECK(decode_light(encode_light(update), decoded) && decoded == update);

  // A destroy carries the id alone.
  LightOrder destroy;
  destroy.kind = LightKind::Destroy;
  destroy.id = 4097;
  const auto bytes = encode_light(destroy);
  KCD2S_CHECK(bytes.size() == 4);
  KCD2S_CHECK(decode_light(bytes, decoded) && decoded.kind == LightKind::Destroy &&
              decoded.id == 4097);
}

KCD2S_TEST(lights, on_a_body_joint) {
  // the light hangs on a body's joint, its position the offset from it.
  LightOrder torch = lantern();
  torch.body = 7;
  torch.joint = "RightHand";
  torch.position = {0.0f, 0.0f, 0.3f};
  LightOrder decoded;
  KCD2S_CHECK(decode_light(encode_light(torch), decoded) && decoded == torch);

  // An order from a server before the body: the last two fields missing.
  auto bytes = encode_light(lantern());
  bytes.resize(bytes.size() - 2);
  KCD2S_CHECK(decode_light(bytes, decoded) && decoded.body == 0 && decoded.joint.empty() &&
              decoded.radius == lantern().radius);

  LightOrder bad = torch;
  bad.body = 0;
  KCD2S_CHECK(encode_light(bad).empty());
  bad = torch;
  bad.position = {9.0f, 0.0f, 0.0f};
  KCD2S_CHECK(encode_light(bad).empty());
}

KCD2S_TEST(lights, refuses_what_cannot_travel) {
  LightOrder order = lantern();
  order.id = 0;
  KCD2S_CHECK(encode_light(order).empty());

  order = lantern();
  order.color[1] = 1.5f;
  KCD2S_CHECK(encode_light(order).empty());

  order = lantern();
  order.intensity = kLightIntensityMax + 1.0f;
  KCD2S_CHECK(encode_light(order).empty());

  order = lantern();
  order.radius = 0.05f;
  KCD2S_CHECK(encode_light(order).empty());

  order = lantern();
  order.position[2] = std::nanf("");
  KCD2S_CHECK(encode_light(order).empty());

  // Truncated and trailing bytes, and an unknown kind.
  auto bytes = encode_light(lantern());
  LightOrder decoded;
  bytes.pop_back();
  KCD2S_CHECK(!decode_light(bytes, decoded));
  bytes = encode_light(lantern());
  bytes.push_back(0);
  KCD2S_CHECK(!decode_light(bytes, decoded));
  bytes = encode_light(lantern());
  bytes[1] = 9;
  KCD2S_CHECK(!decode_light(bytes, decoded));
}
