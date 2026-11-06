#include "remote_ui_role.hpp"

#include <array>
#include <cstdint>

#include "kcd2s/testkit.hpp"

namespace {
using namespace kcds::runtime::replicas;
using Player = std::array<std::uint8_t, kPlayerMissileUiListener + 8>;
}  // namespace

KCD2S_TEST(remote_ui_role, thousands_of_remote_lifetimes_do_not_accumulate_local_listeners) {
  Player local{}, remote{};
  unsigned native_registrations = 0;
  unsigned names_consumed = 0;
  const auto native = [&] {
    ++native_registrations;
    ++names_consumed;
  };
  const auto consume = [&] { ++names_consumed; };
  {
    RemotePlayerConstruction scope(local.data(), false);
    KCD2S_CHECK(route_ui_registration(local.data() + kPlayerMissileUiListener, native, consume));
  }
  for (unsigned i = 0; i < 10000; ++i) {
    RemotePlayerConstruction scope(remote.data(), true);
    KCD2S_CHECK(!route_ui_registration(remote.data() + kPlayerMissileUiListener, native, consume));
  }
  KCD2S_CHECK(native_registrations == 1);
  KCD2S_CHECK(names_consumed == 10001);
  KCD2S_CHECK(constructing_remote_player == nullptr);
}

KCD2S_TEST(remote_ui_role, nested_local_and_unrelated_native_listeners_are_preserved) {
  Player outer{}, inner{};
  unsigned forwarded = 0;
  unsigned skipped = 0;
  const auto native = [&] { ++forwarded; };
  const auto consume = [&] { ++skipped; };
  {
    RemotePlayerConstruction scope(outer.data(), true);
    KCD2S_CHECK(route_ui_registration(outer.data(), native, consume));
    {
      RemotePlayerConstruction nested(inner.data(), false);
      KCD2S_CHECK(route_ui_registration(inner.data() + kPlayerMissileUiListener, native, consume));
      KCD2S_CHECK(route_ui_registration(outer.data() + kPlayerMissileUiListener, native, consume));
    }
    {
      RemotePlayerConstruction nested(inner.data(), true);
      KCD2S_CHECK(!route_ui_registration(inner.data() + kPlayerMissileUiListener, native, consume));
      KCD2S_CHECK(route_ui_registration(outer.data() + kPlayerMissileUiListener, native, consume));
    }
    KCD2S_CHECK(!route_ui_registration(outer.data() + kPlayerMissileUiListener, native, consume));
  }
  KCD2S_CHECK(route_ui_registration(outer.data() + kPlayerMissileUiListener, native, consume));
  KCD2S_CHECK(forwarded == 5);
  KCD2S_CHECK(skipped == 2);
}

KCD2S_TEST(remote_ui_role, native_finally_state_transitions_restore_nested_scope) {
  Player outer{}, inner{};
  const auto* root = begin_remote_player_construction(outer.data(), true);
  KCD2S_CHECK(root == nullptr);
  const auto* remote_parent = begin_remote_player_construction(inner.data(), true);
  KCD2S_CHECK(constructing_remote_player == inner.data());
  end_remote_player_construction(remote_parent);
  KCD2S_CHECK(constructing_remote_player == outer.data());
  const auto* local_parent = begin_remote_player_construction(inner.data(), false);
  KCD2S_CHECK(constructing_remote_player == nullptr);
  end_remote_player_construction(local_parent);
  KCD2S_CHECK(constructing_remote_player == outer.data());
  end_remote_player_construction(root);
  KCD2S_CHECK(constructing_remote_player == nullptr);
  // This portable test covers the exact transition helpers used by __finally. It does not
  // claim to execute Windows SEH; that wrapper must also be compiled in the native client.
}
