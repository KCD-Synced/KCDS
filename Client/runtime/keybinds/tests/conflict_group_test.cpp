#include "../conflict_group.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using kcds::runtime::keybinds::conflicts::group_of;
  using kcds::runtime::keybinds::conflicts::Member;

  // A server's two actions on E (one at a workplace, one at a notice board), KCDS's own actions on
  // their own keys and an action with no default key.
  const std::vector<Member> actions = {
      {"kcds_lights_candle", ""},    {"kcds_chat_open", "t"},     {"kcds_work", "e"},
      {"kcds_board", "e"},           {"kcds_third_person", "f5"}, {"kcds_point", "v"},
      {"kcds_voice_talk", "mouse4"}, {"kcds_voice_tier", "f9"},
  };
  const std::vector<std::string> group = group_of(actions);
  const std::vector<std::string> wanted = {"kcds_chat_open", "kcds_third_person", "kcds_point",
                                           "kcds_voice_talk", "kcds_voice_tier"};
  expect(group == wanted, "the shared E pair and the unbound action stay out, the rest in order");

  expect(group_of({}).empty(), "no actions, no group");
  expect(group_of({{"kcds_a", "e"}, {"kcds_b", "e"}, {"kcds_c", "e"}}).empty(),
         "three on one key all stay out");
  expect(group_of({{"kcds_a", "f6"}}).size() == 1, "a lone action is a member");

  if (failures == 0) {
    std::printf("conflict_group: all passed\n");
  }
  return failures == 0 ? 0 : 1;
}
