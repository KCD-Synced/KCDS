#include "kcd2s/integration/hand_targets.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace integration = kcd2s::integration;

KCD2S_TEST(hand_targets, field_round_trip) {
  std::vector<integration::HandTarget> hands;
  KCD2S_CHECK(integration::hand_targets_field(hands).empty());
  integration::HandTarget right;
  right.position = {0.25f, 0.45f, 1.3f};
  right.rotation = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
  right.weight = 0.8f;
  right.blend = 0.25f;
  right.first_person_position = std::array<float, 3>{0.2f, 0.55f, 1.45f};
  integration::HandTarget left;
  left.right = false;
  left.joint = "LeftArm";
  left.position = {-0.1f, 0.3f, -0.2f};
  left.pole = std::array<float, 3>{-0.5f, -0.2f, 1.0f};
  hands = {right, left};
  const std::string field = integration::hand_targets_field(hands);
  KCD2S_CHECK(field ==
              "r||250,450,1300|0,0,0,10000||80|250|200,550,1450|;"
              "l|LeftArm|-100,300,-200||-500,-200,1000|100|0||");
  std::vector<integration::HandTarget> parsed;
  KCD2S_CHECK(integration::parse_hand_targets_field(field, parsed));
  KCD2S_CHECK_EQ(parsed.size(), std::size_t(2));
  KCD2S_CHECK(parsed[0] == right);
  KCD2S_CHECK(parsed[1].joint == "LeftArm");
  KCD2S_CHECK(parsed[1].pole.has_value() && std::abs((*parsed[1].pole)[0] + 0.5f) < 1e-6f);
  KCD2S_CHECK(integration::parse_hand_targets_field("", parsed) && parsed.empty());
}

KCD2S_TEST(hand_targets, malformed_fields_are_refused) {
  std::vector<integration::HandTarget> parsed;
  const char* bad[] = {
      "x||0,0,0|||100|0||",                     // no such hand
      "r||0,0,0|||100|0|",                      // too few parts
      "r||0,0|||100|0||",                       // a position of two numbers
      "r||0,0,9000|||100|0||",                  // out of reach
      "r||0,0,0|||101|0||",                     // weight over 100
      "r||0,0,0|||100|20000||",                 // blend over 10 s
      "r||0,0,0|0,0,0,0||100|0||",              // no rotation
      "r|Right;Arm|0,0,0|||100|0||",            // a joint name with a separator
      "r||0,0,0|||100|0||;r||0,0,0|||100|0||",  // the right hand twice
      "r||0,0,0|||100|0|1,1,|",                 // a first-person position cut short
  };
  for (const char* field : bad) {
    KCD2S_CHECK(!integration::parse_hand_targets_field(field, parsed));
    KCD2S_CHECK(parsed.empty());
  }
  // A rotation off unit length is made one.
  KCD2S_CHECK(integration::parse_hand_targets_field("l||0,0,0|0,0,0,9000||100|0||", parsed));
  KCD2S_CHECK(parsed.size() == 1 && std::abs((*parsed[0].rotation)[3] - 1.0f) < 1e-6f);
}
