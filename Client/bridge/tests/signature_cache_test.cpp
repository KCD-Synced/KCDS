#include "kcd2s/bridge/signature_cache.hpp"

#include <string>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::bridge;

KCD2S_TEST(signature_cache, a_saved_cache_comes_back_for_its_build) {
  SignatureCache saved("game-1 client-1");
  KCD2S_CHECK(!saved.changed());
  saved.remember("48 8B ?? 89", 0x1234);
  saved.remember("E8 ?? ?? ?? ?? C3", 0xABCDEF012);
  KCD2S_CHECK(saved.changed());

  SignatureCache loaded("game-1 client-1");
  loaded.load(saved.serialized());
  KCD2S_CHECK_EQ(loaded.size(), std::size_t{2});
  KCD2S_CHECK(!loaded.changed());
  KCD2S_CHECK(loaded.find("48 8B ?? 89") == std::size_t{0x1234});
  KCD2S_CHECK(loaded.find("E8 ?? ?? ?? ?? C3") == std::size_t{0xABCDEF012});
  KCD2S_CHECK(!loaded.find("90 90").has_value());
}

KCD2S_TEST(signature_cache, another_build_or_a_damaged_file_is_not_used) {
  SignatureCache saved("game-1 client-1");
  saved.remember("48 8B", 16);
  const std::string text = saved.serialized();

  SignatureCache other_game("game-2 client-1");
  other_game.load(text);
  KCD2S_CHECK_EQ(other_game.size(), std::size_t{0});

  SignatureCache other_client("game-1 client-2");
  other_client.load(text);
  KCD2S_CHECK_EQ(other_client.size(), std::size_t{0});

  SignatureCache damaged("game-1 client-1");
  damaged.load(text + "not a line\n");
  KCD2S_CHECK_EQ(damaged.size(), std::size_t{0});

  SignatureCache cut("game-1 client-1");
  cut.load(text.substr(0, text.size() - 1));
  KCD2S_CHECK_EQ(cut.size(), std::size_t{0});

  SignatureCache nothing("game-1 client-1");
  nothing.load("");
  KCD2S_CHECK_EQ(nothing.size(), std::size_t{0});
}

KCD2S_TEST(signature_cache, a_new_offset_replaces_the_old_and_counts_as_a_change) {
  SignatureCache saved("build");
  saved.remember("48 8B", 16);
  SignatureCache loaded("build");
  loaded.load(saved.serialized());
  loaded.remember("48 8B", 32);
  KCD2S_CHECK(loaded.changed());
  KCD2S_CHECK(loaded.find("48 8B") == std::size_t{32});
}
