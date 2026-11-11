// The generated layouts compile with their size checks (game_layouts.hpp's static_asserts) and keep
// the offsets the client was written against.
#include "game_layouts.hpp"

#include "kcd2s/testkit.hpp"

KCD2S_TEST(game_layouts, actor_and_soul_offsets) {
  namespace layouts = kcds::layouts;
  KCD2S_CHECK_EQ(layouts::Actor::kSoul, std::size_t(1640));
  KCD2S_CHECK_EQ(layouts::Actor::kEntity, std::size_t(56));
  KCD2S_CHECK_EQ(layouts::Soul::kHealth, std::size_t(1800));
  KCD2S_CHECK_EQ(layouts::Soul::kBuffsEnd - layouts::Soul::kBuffsBegin, std::size_t(8));
  KCD2S_CHECK_EQ(layouts::Soul::kProperties + layouts::SoulProperties::kHealth,
                 layouts::Soul::kHealth);
}
