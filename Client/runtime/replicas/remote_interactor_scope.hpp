// Identity predicate shared by native input-destructor scopes and portable regression tests.
#pragma once

namespace kcds::runtime::replicas {

inline bool remote_input_without_torch_ownership(const void* input, const void* owner,
                                                 const void* constructing_player,
                                                 const void* releasing_player,
                                                 const void* expected_input) noexcept {
  if (!input || !owner) {
    return false;
  }
  // Input Init has not run during Player construction. This also covers destruction of
  // an owned constructor temporary before it is stored at player+2952.
  if (constructing_player && owner == constructing_player) {
    return true;
  }
  return releasing_player && owner == releasing_player && input == expected_input;
}

}  // namespace kcds::runtime::replicas
