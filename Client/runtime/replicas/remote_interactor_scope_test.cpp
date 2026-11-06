#include "remote_interactor_scope.hpp"
#include "remote_ui_role.hpp"

#include "kcd2s/testkit.hpp"

namespace {
using namespace kcds::runtime::replicas;
}  // namespace

KCD2S_TEST(remote_interactor_scope, release_requires_exact_input_and_owner) {
  int remote = 0;
  int local = 0;
  int input = 0;
  int other_input = 0;
  KCD2S_CHECK(remote_input_without_torch_ownership(&input, &remote, nullptr, &remote, &input));
  KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &local, nullptr, &remote, &input));
  KCD2S_CHECK(
      !remote_input_without_torch_ownership(&other_input, &remote, nullptr, &remote, &input));
  KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &remote, nullptr, nullptr, &input));
  KCD2S_CHECK(!remote_input_without_torch_ownership(nullptr, &remote, &remote, nullptr, nullptr));
  KCD2S_CHECK(!remote_input_without_torch_ownership(&input, nullptr, &remote, &remote, &input));
}

KCD2S_TEST(remote_interactor_scope, constructor_temporary_does_not_require_published_input) {
  int remote = 0;
  int other = 0;
  int input = 0;
  RemotePlayerConstruction scope(&remote, true);
  KCD2S_CHECK(remote_input_without_torch_ownership(&input, &remote, constructing_remote_player,
                                                   nullptr, nullptr));
  KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &other, constructing_remote_player,
                                                    nullptr, nullptr));
}

KCD2S_TEST(remote_interactor_scope, nested_construction_preserves_local_and_unrelated_cleanup) {
  int outer = 0;
  int inner = 0;
  int input = 0;
  {
    RemotePlayerConstruction scope(&outer, true);
    {
      RemotePlayerConstruction nested(&inner, false);
      KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &inner, constructing_remote_player,
                                                        nullptr, nullptr));
      KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &outer, constructing_remote_player,
                                                        nullptr, nullptr));
    }
    {
      RemotePlayerConstruction nested(&inner, true);
      KCD2S_CHECK(remote_input_without_torch_ownership(&input, &inner, constructing_remote_player,
                                                       nullptr, nullptr));
      KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &outer, constructing_remote_player,
                                                        nullptr, nullptr));
    }
    KCD2S_CHECK(remote_input_without_torch_ownership(&input, &outer, constructing_remote_player,
                                                     nullptr, nullptr));
  }
  KCD2S_CHECK(!remote_input_without_torch_ownership(&input, &outer, constructing_remote_player,
                                                    nullptr, nullptr));
}
