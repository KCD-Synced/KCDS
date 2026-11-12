// The YAML subset of the kcds-bots scenario files (local/bots/scenario/yaml_subset.hpp).
#include "../local/bots/scenario/yaml_subset.hpp"

#include <string>

#include "kcd2s/testkit.hpp"

namespace yaml = kcd2s::local::bots::yaml;
using yaml::Node;

namespace {

// The line a ParseError names, or 0 when the text parses.
int error_line(const std::string& text) {
  try {
    yaml::parse(text);
  } catch (const yaml::ParseError& error) {
    return error.line();
  }
  return 0;
}

}  // namespace

KCD2S_TEST(bots_yaml, the_scenario_shape) {
  const Node root = yaml::parse(
      "# a battle\n"
      "name: battle\n"
      "duration: 10m\n"
      "network:\n"
      "  latency: [40ms, 120ms]\n"
      "  loss: 1%   # per direction\n"
      "groups:\n"
      "  - name: red\n"
      "    count: 50\n"
      "    spawn: { area: field_west, radius: 15 }\n"
      "    params: { target_group: blue, attack_interval: [0.8s, 2.0s] }\n"
      "  - name: archers\n"
      "    params:\n"
      "      target_group: [red, blue]\n");
  KCD2S_CHECK(root.is_map());
  KCD2S_CHECK_EQ(root.find("name")->scalar, std::string("battle"));
  KCD2S_CHECK_EQ(root.find("duration")->scalar, std::string("10m"));
  const Node* latency = root.find("network")->find("latency");
  KCD2S_CHECK(latency && latency->is_list() && latency->list.size() == 2);
  KCD2S_CHECK_EQ(latency->list[1].scalar, std::string("120ms"));
  KCD2S_CHECK_EQ(root.find("network")->find("loss")->scalar, std::string("1%"));
  const Node* groups = root.find("groups");
  KCD2S_CHECK(groups && groups->is_list() && groups->list.size() == 2);
  const Node& red = groups->list[0];
  KCD2S_CHECK_EQ(red.find("count")->scalar, std::string("50"));
  KCD2S_CHECK_EQ(red.find("spawn")->find("area")->scalar, std::string("field_west"));
  KCD2S_CHECK_EQ(red.find("params")->find("attack_interval")->list[0].scalar, std::string("0.8s"));
  const Node& archers = groups->list[1];
  KCD2S_CHECK_EQ(archers.find("params")->find("target_group")->list.size(), std::size_t{2});
  KCD2S_CHECK_EQ(archers.line, 12);
}

KCD2S_TEST(bots_yaml, lists_at_the_keys_indentation_and_scalars) {
  const Node root = yaml::parse(
      "areas:\n"
      "- name: \"town #1\"\n"
      "  at: [1.5, -2, 3e1]\n"
      "- name: 'it''s'\n"
      "empty:\n"
      "nothing: ~\n"
      "words: [ a , 'b, c' ]\n");
  const Node* areas = root.find("areas");
  KCD2S_CHECK(areas && areas->list.size() == 2);
  KCD2S_CHECK_EQ(areas->list[0].find("name")->scalar, std::string("town #1"));
  KCD2S_CHECK_EQ(areas->list[0].find("at")->list[2].scalar, std::string("3e1"));
  KCD2S_CHECK_EQ(areas->list[1].find("name")->scalar, std::string("it's"));
  KCD2S_CHECK(root.find("empty")->kind == Node::Kind::Null);
  KCD2S_CHECK(root.find("nothing")->kind == Node::Kind::Null);
  KCD2S_CHECK_EQ(root.find("words")->list[1].scalar, std::string("b, c"));
}

KCD2S_TEST(bots_yaml, nested_flow_and_empty_collections) {
  const Node root = yaml::parse("a: { b: [1, { c: 2 }], d: {}, e: [] }\n");
  const Node* a = root.find("a");
  KCD2S_CHECK_EQ(a->find("b")->list[1].find("c")->scalar, std::string("2"));
  KCD2S_CHECK(a->find("d")->is_map() && a->find("d")->map.empty());
  KCD2S_CHECK(a->find("e")->is_list() && a->find("e")->list.empty());
}

KCD2S_TEST(bots_yaml, errors_name_their_line) {
  KCD2S_CHECK_EQ(error_line("a: 1\na: 2\n"), 2);       // a key twice
  KCD2S_CHECK_EQ(error_line("a: [1, 2\n"), 1);         // unclosed
  KCD2S_CHECK_EQ(error_line("a: 1\n    b: 2\n"), 2);   // stray indentation
  KCD2S_CHECK_EQ(error_line("a:\n\tb: 1\n"), 2);       // tabs
  KCD2S_CHECK_EQ(error_line("a: &anchor 1\n"), 1);     // anchors
  KCD2S_CHECK_EQ(error_line("a: |\n  text\n"), 1);     // block strings
  KCD2S_CHECK_EQ(error_line("a: 1\n---\nb: 2\n"), 2);  // two documents
  KCD2S_CHECK_EQ(error_line("a: \"open\n"), 1);        // an open quote
  KCD2S_CHECK_EQ(error_line("a: {b 1}\n"), 1);         // no colon
  KCD2S_CHECK_EQ(error_line("a: 1 # fine\n\n# only a comment\n"), 0);
}

// A long flow map as Prettier writes it: over several lines, a trailing comma.
KCD2S_TEST(bots_yaml, flow_collections_over_several_lines) {
  const Node root = yaml::parse(
      "groups:\n"
      "  - {\n"
      "      name: red,   # a comment inside\n"
      "      count: 3,\n"
      "      params: { target_group: [blue, green,], block: \"a, b\" },\n"
      "    }\n"
      "  - { name: blue, count: 2 }\n"
      "after: 1\n");
  const Node* groups = root.find("groups");
  KCD2S_CHECK(groups && groups->list.size() == 2);
  KCD2S_CHECK_EQ(groups->list[0].find("count")->scalar, std::string("3"));
  KCD2S_CHECK_EQ(groups->list[0].find("params")->find("target_group")->list.size(), std::size_t{2});
  KCD2S_CHECK_EQ(groups->list[0].find("params")->find("block")->scalar, std::string("a, b"));
  KCD2S_CHECK_EQ(groups->list[1].find("name")->scalar, std::string("blue"));
  KCD2S_CHECK_EQ(root.find("after")->scalar, std::string("1"));
  KCD2S_CHECK_EQ(error_line("a: {\n  b: 1,\n"), 1);  // never closed
}

KCD2S_TEST(bots_yaml, an_empty_file_is_null) {
  KCD2S_CHECK(yaml::parse("").kind == Node::Kind::Null);
  KCD2S_CHECK(yaml::parse("# nothing\n\n").kind == Node::Kind::Null);
}
