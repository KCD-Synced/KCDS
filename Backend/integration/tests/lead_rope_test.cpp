#include "kcd2s/integration/lead_rope.hpp"

#include "kcd2s/testkit.hpp"

namespace integration = kcd2s::integration;

KCD2S_TEST(lead_rope, field_round_trip) {
  integration::LeadRope rope;
  rope.target = 12;
  rope.from = "RightHand";
  rope.to = "Bip01 Pelvis";
  rope.length = 2.5f;
  KCD2S_CHECK(integration::lead_rope_field(rope) == "12|RightHand|Bip01 Pelvis|2.50");
  const auto back = integration::lead_rope_of("12|RightHand|Bip01 Pelvis|2.50");
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->target == 12);
  KCD2S_CHECK(back->from == "RightHand");
  KCD2S_CHECK(back->to == "Bip01 Pelvis");
  KCD2S_CHECK(back->length > 2.49f && back->length < 2.51f);
}

KCD2S_TEST(lead_rope, none_and_bad_fields) {
  KCD2S_CHECK(!integration::lead_rope_of("").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|RightHand|Hips").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|RightHand|Hips|2.5|x").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("0|RightHand|Hips|2.5").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("x|RightHand|Hips|2.5").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("4294967296|RightHand|Hips|2.5").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12||Hips|2.5").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|RightHand|Hips|0.1").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|RightHand|Hips|25").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|RightHand|Hips|long").has_value());
  KCD2S_CHECK(!integration::lead_rope_of("12|" + std::string(49, 'a') + "|Hips|2").has_value());
}
