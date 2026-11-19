// Edits made together settle once over the area they cover, in parts no wider than asked.
#include "settle_area.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::terrain::join;
using kcds::runtime::terrain::SettleArea;

KCD2S_TEST(settle_area, joins_the_boxes_of_edits) {
  SettleArea area;
  const float first[6] = {10, 20, 40, 22, 32, 60};
  const float second[6] = {18, 5, 30, 30, 25, 70};
  KCD2S_CHECK(join(area, first, 256));
  KCD2S_CHECK(join(area, second, 256));
  KCD2S_CHECK_EQ(area.edits, 2);
  KCD2S_CHECK_EQ(area.box[0], 10.0f);
  KCD2S_CHECK_EQ(area.box[1], 5.0f);
  KCD2S_CHECK_EQ(area.box[2], 30.0f);
  KCD2S_CHECK_EQ(area.box[3], 30.0f);
  KCD2S_CHECK_EQ(area.box[4], 32.0f);
  KCD2S_CHECK_EQ(area.box[5], 70.0f);
}

KCD2S_TEST(settle_area, too_wide_an_area_is_not_joined) {
  SettleArea area;
  const float here[6] = {0, 0, 0, 10, 10, 10};
  const float far_east[6] = {300, 0, 0, 310, 10, 10};
  const float far_north[6] = {0, 250, 0, 10, 260, 10};
  KCD2S_CHECK(join(area, here, 256));
  KCD2S_CHECK(!join(area, far_east, 256));
  KCD2S_CHECK(!join(area, far_north, 256));
  // Unchanged by the refused boxes.
  KCD2S_CHECK_EQ(area.edits, 1);
  KCD2S_CHECK_EQ(area.box[3], 10.0f);
  KCD2S_CHECK_EQ(area.box[4], 10.0f);
}
