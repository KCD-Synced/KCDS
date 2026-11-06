#include "kcd2s/integration/pose_animation.hpp"

#include <cmath>
#include <string>

#include "kcd2s/testkit.hpp"

namespace integration = kcd2s::integration;

namespace {

integration::PoseAnimation swing() {
  integration::PoseAnimation animation;
  animation.loop = false;
  integration::PoseKey start;
  start.time = 0.0f;
  start.turns["RightArm"] = {40, 0, 0};
  integration::PoseKey up;
  up.time = 0.5f;
  up.turns["RightArm"] = {130, 0, -40};
  up.turns["RightForeArm"] = {0, 0, 70};
  integration::PoseKey back;
  back.time = 1.0f;
  back.turns["RightArm"] = {40, 0, 0};
  animation.keys = {start, up, back};
  return animation;
}

bool near(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

}  // namespace

KCD2S_TEST(pose_animation, field_round_trip) {
  const std::string field = integration::pose_animation_field(swing());
  KCD2S_CHECK(
      field ==
      "A1NS|0:RightArm=40,0,0|0.5:RightArm=130,0,-40;RightForeArm=0,0,70|1:RightArm=40,0,0");
  const auto back = integration::pose_animation_of(field);
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(!back->loop);
  KCD2S_CHECK(back->blend == integration::PoseBlend::Spline);
  KCD2S_CHECK(back->keys.size() == 3);
  KCD2S_CHECK(near(back->keys[1].turns.at("RightArm")[2], -40.0f));
}

KCD2S_TEST(pose_animation, bad_fields) {
  KCD2S_CHECK(!integration::pose_animation_of("").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1XS|0:a=1,2,3").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0.5:a=1,2,3").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2|1:a=0,0,0").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a b=1,2,3").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3|0:a=1,2,3").has_value());
  KCD2S_CHECK(integration::pose_animation_of("A1LI|0:a=1,2,3|2:a=0,0,0").has_value());
  integration::PoseAnimation long_one;
  for (int index = 0; index < 33; ++index) {
    integration::PoseKey key;
    key.time = float(index) * 0.1f;
    long_one.keys.push_back(key);
  }
  KCD2S_CHECK(integration::pose_animation_field(long_one).empty());
}

KCD2S_TEST(pose_animation, blends_and_play_time) {
  auto animation = swing();
  // Through every key; a joint a key leaves out is not turned there.
  KCD2S_CHECK(near(integration::pose_turns_at(animation, 0.5f).at("RightArm")[0], 130.0f));
  KCD2S_CHECK(near(integration::pose_turns_at(animation, 0.0f).at("RightArm")[0], 40.0f));
  animation.blend = integration::PoseBlend::Linear;
  const auto quarter = integration::pose_turns_at(animation, 0.25f);
  KCD2S_CHECK(near(quarter.at("RightArm")[0], 85.0f));
  KCD2S_CHECK(near(quarter.at("RightForeArm")[2], 35.0f));
  animation.blend = integration::PoseBlend::Smooth;
  KCD2S_CHECK(near(integration::pose_turns_at(animation, 0.25f).at("RightArm")[0], 85.0f));
  // Eased: a fifth of the way it has moved about a tenth (smoothstep 0.104), not a fifth.
  KCD2S_CHECK(near(integration::pose_turns_at(animation, 0.1f).at("RightArm")[0], 49.36f));
  // Not looping it holds at the end; looping it goes round.
  KCD2S_CHECK(near(integration::pose_play_time(animation, 5.0), 1.0f));
  animation.loop = true;
  KCD2S_CHECK(near(integration::pose_play_time(animation, 2.25), 0.25f));
  const auto turn = integration::pose_quaternion({90, 0, 0});
  KCD2S_CHECK(near(turn[0], 0.7071f) && near(turn[3], 0.7071f));
}

KCD2S_TEST(pose_animation, prop_keys) {
  // Keys that set the prop: written after '@', read back, blended between keys.
  auto animation = swing();
  animation.blend = integration::PoseBlend::Linear;
  animation.keys[0].prop = {{'p', 90.0f}, {'c', 1.0f}};
  animation.keys[2].prop = {{'p', 20.0f}, {'c', 2.0f}};
  const std::string field = integration::pose_animation_field(animation);
  KCD2S_CHECK(field ==
              "A1NI|0:RightArm=40,0,0@c1;p90|0.5:RightArm=130,0,-40;RightForeArm=0,0,70"
              "|1:RightArm=40,0,0@c2;p20");
  const auto back = integration::pose_animation_of(field);
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->keys.size() == 3);
  KCD2S_CHECK(back->keys[1].prop.empty());
  KCD2S_CHECK(near(back->keys[2].prop.at('p'), 20.0f));
  KCD2S_CHECK(near(back->keys[2].turns.at("RightArm")[0], 40.0f));
  // Halfway between the keys that set it; collide steps at its key.
  const auto half = integration::pose_prop_at(*back, 0.5f);
  KCD2S_CHECK(near(half.at('p'), 55.0f));
  KCD2S_CHECK(near(half.at('c'), 1.0f));
  KCD2S_CHECK(near(integration::pose_prop_at(*back, 1.0f).at('c'), 2.0f));
  // A key with a prop but no joints; a bad code, a value out of range: refused.
  KCD2S_CHECK(integration::pose_animation_of("A1LS|0:@p20|1:a=1,2,3").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3@x20").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3@p200").has_value());
  KCD2S_CHECK(integration::pose_prop_at(swing(), 0.3f).empty());
}

KCD2S_TEST(pose_animation, lift_keys) {
  // A key's lift of the hips (a baked game clip): after its joints, before '@'.
  auto animation = swing();
  animation.blend = integration::PoseBlend::Linear;
  animation.keys[0].lift = -0.06f;
  animation.keys[1].prop = {{'p', 90.0f}};
  animation.keys[2].lift = 0.0f;
  const std::string field = integration::pose_animation_field(animation);
  KCD2S_CHECK(field ==
              "A1NI|0:RightArm=40,0,0#-0.06|0.5:RightArm=130,0,-40;RightForeArm=0,0,70@p90"
              "|1:RightArm=40,0,0#0");
  const auto back = integration::pose_animation_of(field);
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->keys[0].lift && near(*back->keys[0].lift, -0.06f));
  KCD2S_CHECK(!back->keys[1].lift);
  KCD2S_CHECK(near(back->keys[1].prop.at('p'), 90.0f));
  // Between the keys that lift it, as they blend.
  KCD2S_CHECK(near(integration::pose_lift_at(*back, 0.5f), -0.03f));
  KCD2S_CHECK(near(integration::pose_lift_at(swing(), 0.5f), 0.0f));
  // A lift out of range or not a number: refused.
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3#-2").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3#x").has_value());
}

KCD2S_TEST(pose_animation, lash_keys) {
  // A key's lash shape (Lash to keys): after the lift, before '@'.
  auto animation = swing();
  animation.keys[0].lift = -0.05f;
  animation.keys[0].lash = {{0.1f, 0.0f, 0.0f}, {0.2f, 0.0f, -0.05f}};
  animation.keys[0].prop = {{'p', 90.0f}};
  animation.keys[2].lash = {{0.1f, 0.0f, 0.02f}, {0.2f, 0.0f, 0.05f}};
  const std::string field = integration::pose_animation_field(animation);
  KCD2S_CHECK(
      field ==
      "A1NS|0:RightArm=40,0,0#-0.05~0.1,0,0;0.2,0,-0.05@p90"
      "|0.5:RightArm=130,0,-40;RightForeArm=0,0,70|1:RightArm=40,0,0~0.1,0,0.02;0.2,0,0.05");
  const auto back = integration::pose_animation_of(field);
  KCD2S_CHECK(back.has_value());
  KCD2S_CHECK(back->keys[0].lash.size() == 2 && near(back->keys[0].lash[1][2], -0.05f));
  KCD2S_CHECK(back->keys[0].lift && near(*back->keys[0].lift, -0.05f));
  KCD2S_CHECK(near(back->keys[0].prop.at('p'), 90.0f));
  KCD2S_CHECK(back->keys[1].lash.empty());
  // Between the keys that shape it, point by point.
  const auto half = integration::pose_lash_at(*back, 0.5f);
  KCD2S_CHECK(half.size() == 2 && near(half[0][2], 0.01f) && near(half[1][2], 0.0f));
  KCD2S_CHECK(integration::pose_lash_at(swing(), 0.5f).empty());
  // A point too far or not three numbers: refused.
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3~9,0,0").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3~1,2").has_value());
}

KCD2S_TEST(pose_animation, lash_track) {
  // A lash track after the keys (Lash from motion): its own frames, played by each
  // segment's way, the lash its whole length between frames too, round the loop.
  auto animation = swing();
  animation.loop = true;
  animation.lash_track.rate = 10.0f;
  animation.lash_track.length = 1.0f;
  animation.lash_track.frames = {{0, 0, 0, 0}, {0, 0, 90, 0}};
  const std::string field = integration::pose_animation_field(animation);
  KCD2S_CHECK(field ==
              "A1LS|0:RightArm=40,0,0|0.5:RightArm=130,0,-40;RightForeArm=0,0,70"
              "|1:RightArm=40,0,0^10,1:0,0,0,0/0,0,90,0");
  const auto back = integration::pose_animation_of(field);
  KCD2S_CHECK(back.has_value() && back->keys.size() == 3);
  KCD2S_CHECK(back->lash_track.frames.size() == 2 && near(back->lash_track.frames[1][2], 90.0f));
  const auto start = integration::pose_lash_at(*back, 0.0f);
  KCD2S_CHECK(start.size() == 2 && near(start[1][0], 1.0f) && near(start[1][1], 0.0f));
  const auto turned = integration::pose_lash_at(*back, 0.1f);
  KCD2S_CHECK(near(turned[1][0], 0.5f) && near(turned[1][1], 0.5f));
  // Halfway the second piece is half turned and still half a metre long.
  const auto half = integration::pose_lash_at(*back, 0.05f);
  KCD2S_CHECK(near(std::hypot(half[1][0] - 0.5f, half[1][1]), 0.5f));
  // Looping, 0.2 s is the first frame again.
  KCD2S_CHECK(near(integration::pose_lash_at(*back, 0.2f)[1][0], 1.0f));
  // Kept in the body's frame: ",b" after the length, read back so.
  animation.lash_track.body = true;
  const std::string body_field = integration::pose_animation_field(animation);
  KCD2S_CHECK(body_field.find("^10,1,b:") != std::string::npos);
  const auto body_back = integration::pose_animation_of(body_field);
  KCD2S_CHECK(body_back.has_value() && body_back->lash_track.body);
  KCD2S_CHECK(!back->lash_track.body);
  // A pitch past 90°, frames of different sizes, a rate of 0: refused.
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3^10,1:0,95,0,0/0,0,0,0").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3^10,1:0,0,0,0/0,0").has_value());
  KCD2S_CHECK(!integration::pose_animation_of("A1LS|0:a=1,2,3^0,1:0,0,0,0/0,0,0,0").has_value());
}

KCD2S_TEST(pose_animation, defined_by_name) {
  // An animation defined by name may be longer than the field takes; the field names it.
  integration::PoseAnimation animation;
  for (int index = 0; index < 32; ++index) {
    integration::PoseKey key;
    key.time = float(index) / 10.0f;
    for (int joint = 0; joint < 24; ++joint) {
      key.turns["Joint" + std::to_string(joint)] = {float(index) + 0.5f, float(joint), -12.5f};
    }
    key.lift = -0.05f;
    animation.keys.push_back(key);
  }
  KCD2S_CHECK(integration::pose_animation_field(animation).empty());
  const std::string long_field =
      integration::pose_animation_field(animation, integration::kMaxDefinedPoseAnimationBytes);
  KCD2S_CHECK(long_field.size() > integration::kMaxPoseAnimationBytes);
  KCD2S_CHECK(!integration::pose_animation_of(long_field).has_value());
  const auto back =
      integration::pose_animation_of(long_field, integration::kMaxDefinedPoseAnimationBytes);
  KCD2S_CHECK(back.has_value() && back->keys.size() == 32);
  std::string name;
  bool loop = false;
  KCD2S_CHECK(integration::pose_animation_reference("R1Lwhipping", name, loop));
  KCD2S_CHECK(name == "whipping" && loop);
  KCD2S_CHECK(integration::pose_animation_reference("R1Nwhip_2", name, loop));
  KCD2S_CHECK(name == "whip_2" && !loop);
  KCD2S_CHECK(!integration::pose_animation_reference("R1Lbad name", name, loop));
  KCD2S_CHECK(!integration::pose_animation_reference("A1LS|0:a=1,2,3", name, loop));
}
