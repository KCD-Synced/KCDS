#include "dodges.hpp"

#include <cmath>
#include <limits>

#include "kcd2s/integration/combat.hpp"
#include "kcd2s/testkit.hpp"

using kcds::runtime::combat::answer_contact;
using kcds::runtime::combat::dodge_degrees;
using kcds::runtime::combat::dodge_vector;
using kcds::runtime::combat::DodgeAnswer;
using kcds::runtime::combat::DodgeVector;

namespace {

bool near(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

}  // namespace

KCD2S_TEST(dodges, the_movement_keys_give_the_direction_the_request_dodges_in) {
  const DodgeVector none{};
  // The key vectors the combat input adds up (..).
  KCD2S_CHECK(near(*dodge_degrees({1, 0}, none), 0.0f));
  KCD2S_CHECK(near(*dodge_degrees({0, 1}, none), 90.0f));
  KCD2S_CHECK(near(*dodge_degrees({-1, 0}, none), 180.0f));
  KCD2S_CHECK(near(*dodge_degrees({0, -1}, none), -90.0f));
  // Left and back held together.
  KCD2S_CHECK(near(*dodge_degrees({-1, -1}, none), -135.0f));
  // Without a key the request takes its second vector, and without either it makes no dodge.
  KCD2S_CHECK(near(*dodge_degrees(none, {0.0f, -0.5f}), -90.0f));
  KCD2S_CHECK(!dodge_degrees(none, none));
  // Left and right together cancel out, as in the game.
  KCD2S_CHECK(!dodge_degrees({0, 0}, none));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(near(*dodge_degrees({nan, 1}, {1, 0}), 0.0f));
}

KCD2S_TEST(dodges, every_direction_the_request_takes_can_be_relayed) {
  using kcd2s::integration::CombatAction;
  using kcd2s::integration::valid_combat_action;
  // A straight left is a half turn (pi times the float conversion may round past 180 degrees
  // without the clamp), and -180 after a negative zero.
  const DodgeVector directions[] = {
      {-1, 0}, {-1, -0.0f}, {1, 0}, {0, 1}, {0, -1}, {-1, -1}, {0.3f, -0.95f},
  };
  for (const DodgeVector& keys : directions) {
    const auto degrees = dodge_degrees(keys, {});
    KCD2S_CHECK(degrees.has_value());
    if (degrees) {
      KCD2S_CHECK(valid_combat_action({CombatAction::Dodge, 1, *degrees}));
    }
  }
}

KCD2S_TEST(dodges, a_relayed_direction_is_the_unit_vector_of_its_keys) {
  for (const DodgeVector keys : {DodgeVector{1, 0}, DodgeVector{0, 1}, DodgeVector{-1, 0},
                                 DodgeVector{0, -1}, DodgeVector{-1, 1}}) {
    const DodgeVector unit = dodge_vector(*dodge_degrees(keys, {}));
    const float length = std::sqrt(keys.x * keys.x + keys.y * keys.y);
    KCD2S_CHECK(near(unit.x, keys.x / length));
    KCD2S_CHECK(near(unit.y, keys.y / length));
  }
}

KCD2S_TEST(dodges, a_slot_dodge_on_the_dodgers_screen_takes_the_relayed_blow_out) {
  // The contact arrived at 1000 ms. No dodge and no window: taken at once.
  KCD2S_CHECK(answer_contact(1000, 1000, 0, 0) == DodgeAnswer::Take);
  // The swing still coming on the dodger's screen: it waits, at most 300 ms.
  KCD2S_CHECK(answer_contact(1000, 1000, 0, 1) == DodgeAnswer::Wait);
  KCD2S_CHECK(answer_contact(1299, 1000, 0, 1) == DodgeAnswer::Wait);
  KCD2S_CHECK(answer_contact(1300, 1000, 0, 1) == DodgeAnswer::Take);
  // A slot dodge after it arrived, while it waited: dodged.
  KCD2S_CHECK(answer_contact(1150, 1000, 1120, 0) == DodgeAnswer::Dodged);
  // A slot dodge up to 400 ms before it arrived: dodged; one earlier: taken.
  KCD2S_CHECK(answer_contact(1000, 1000, 600, 0) == DodgeAnswer::Dodged);
  KCD2S_CHECK(answer_contact(1000, 1000, 599, 0) == DodgeAnswer::Take);
}
