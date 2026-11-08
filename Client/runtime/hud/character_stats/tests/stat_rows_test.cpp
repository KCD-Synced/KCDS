#include "../stat_rows.hpp"

#include <string>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::hud::character_stats;

KCD2S_TEST(character_stats, a_row_id_names_its_stat) {
  KCD2S_CHECK_EQ(std::string(stat_name("0:speech")), std::string("speech"));
  KCD2S_CHECK_EQ(std::string(stat_name("1:charisma")), std::string("charisma"));
  KCD2S_CHECK_EQ(std::string(stat_name("strength")), std::string("strength"));
}

KCD2S_TEST(character_stats, hidden_by_stat_name_whatever_the_number) {
  StatRows rows;
  rows.active = true;
  rows.hidden = {"speech", "charisma"};
  KCD2S_CHECK(is_hidden(rows, "0:speech"));
  KCD2S_CHECK(is_hidden(rows, "1:charisma"));
  KCD2S_CHECK(!is_hidden(rows, "0:strength"));
  KCD2S_CHECK(!is_hidden(rows, "0:speech_bonus"));
}

KCD2S_TEST(character_stats, labels_and_values_fit_the_panel) {
  KCD2S_CHECK_EQ(fitted_label("Thirst"), std::string("Thirst"));
  const std::string long_label(100, 'a');
  KCD2S_CHECK_EQ(fitted_label(long_label).size(), kMostLabelBytes);
  // A two-byte letter across the limit is left out whole.
  std::string accented(kMostLabelBytes - 1, 'a');
  accented += "\xC5\xBE";
  KCD2S_CHECK_EQ(fitted_label(accented).size(), kMostLabelBytes - 1);
  KCD2S_CHECK_EQ(fitted_value(42.7), 42);
  KCD2S_CHECK_EQ(fitted_value(1e9), kMostValue);
  KCD2S_CHECK_EQ(fitted_value(-1e9), -kMostValue);
}
