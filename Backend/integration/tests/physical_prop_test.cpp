#include "kcd2s/integration/physical_prop.hpp"

#include "kcd2s/testkit.hpp"

namespace integration = kcd2s::integration;

KCD2S_TEST(physical_prop, field_round_trip) {
  integration::PhysicalProp whip{integration::PhysicalPropKind::Whip, 20, 1.6f};
  KCD2S_CHECK(integration::physical_prop_field(whip) == "whip 20 1.60");
  const auto back = integration::physical_prop_of("whip 20 1.60");
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->kind == integration::PhysicalPropKind::Whip);
  KCD2S_CHECK(back->segments == 20);
  KCD2S_CHECK(back->length > 1.59f && back->length < 1.61f);
  const auto rope = integration::physical_prop_of("rope 16 1.20");
  KCD2S_CHECK(rope.has_value() && rope->kind == integration::PhysicalPropKind::Rope);
  // A chain: a rope drawn with the game's chain links.
  const auto chain = integration::physical_prop_of("chain 24 1.50");
  KCD2S_CHECK(chain.has_value() && chain->kind == integration::PhysicalPropKind::Chain);
  KCD2S_CHECK(integration::physical_prop_field(*chain) == "chain 24 1.50");
}

KCD2S_TEST(physical_prop, none_and_bad_fields) {
  KCD2S_CHECK(!integration::physical_prop_of("").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("cloth 10 1.0").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 1 1.0").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 65 1.0").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 16 0.1").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 16 20.5").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 16").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope 16 1.0 extra").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("rope sixteen 1.0").has_value());
}

KCD2S_TEST(physical_prop, settings_in_the_field) {
  // Only the settings that differ from the first ones are written.
  integration::PhysicalProp whip{integration::PhysicalPropKind::Whip, 20, 1.6f};
  whip.left_hand = true;
  whip.pitch = 20.0f;
  whip.damping = 0.1f;
  whip.collide = 2;
  const std::string field = integration::physical_prop_field(whip);
  KCD2S_CHECK(field == "whip 20 1.60 h=1 p=20 d=0.1 c=2");
  const auto back = integration::physical_prop_of(field);
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->left_hand);
  KCD2S_CHECK(back->pitch == 20.0f);
  KCD2S_CHECK(back->collide == 2);
  KCD2S_CHECK(back->gravity == 9.8f);
  KCD2S_CHECK(back->damping > 0.099f && back->damping < 0.101f);
  // Out of range, an unknown code, a code without a value: refused.
  KCD2S_CHECK(!integration::physical_prop_of("whip 20 1.60 p=200").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("whip 20 1.60 x=1").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("whip 20 1.60 p=").has_value());
  KCD2S_CHECK(!integration::physical_prop_of("whip 20 1.60 c=3").has_value());
  // An animation's values on top: blended numbers kept in range, stepped ones rounded.
  const auto keyed =
      integration::physical_prop_with(*back, {{'p', 95.0f}, {'c', 0.6f}, {'h', 0.2f}});
  KCD2S_CHECK(keyed.pitch == 95.0f);
  KCD2S_CHECK(keyed.collide == 1);
  KCD2S_CHECK(!keyed.left_hand);
}

KCD2S_TEST(physical_prop, a_posed_lash_in_the_field) {
  // A whip's lash posed instead of moved by physics: k shape, u curl, w sway.
  const auto posed = integration::physical_prop_of("whip 20 1.60 k=1 u=120 w=-30");
  KCD2S_CHECK(posed.has_value());
  KCD2S_CHECK(posed->shape == 1.0f);
  KCD2S_CHECK(posed->curl == 120.0f);
  KCD2S_CHECK(posed->sway == -30.0f);
  KCD2S_CHECK(integration::physical_prop_field(*posed) == "whip 20 1.60 k=1 u=120 w=-30");
  KCD2S_CHECK(!integration::physical_prop_of("whip 20 1.60 k=2").has_value());
  // Blended along an animation: half way from straight to a curl of 120.
  const auto keyed = integration::physical_prop_with(*posed, {{'u', 60.0f}, {'k', 0.5f}});
  KCD2S_CHECK(keyed.curl == 60.0f && keyed.shape == 0.5f);
}

KCD2S_TEST(physical_prop, limits) {
  KCD2S_CHECK(integration::physical_prop_of("rope 2 0.20").has_value());
  KCD2S_CHECK(integration::physical_prop_of("whip 64 20.00").has_value());
  integration::PhysicalProp wrong{integration::PhysicalPropKind::Rope, 70, 1.0f};
  KCD2S_CHECK(!integration::physical_prop_fits(wrong));
}
