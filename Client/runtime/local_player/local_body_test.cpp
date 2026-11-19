// The local body the client scripts' LocalPlayer reads, as the network thread sends it.
#include "local_body.hpp"

#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace body = kcds::runtime::local_body;
namespace sdk = kcd2s::sdk;

namespace {

body::Body standing() {
  body::Body b;
  b.position = {10.0f, 20.0f, 30.0f};
  b.rotation = {0.0f, 0.0f, 0.70710677f, 0.70710677f};
  b.velocity = {1.0f, 0.0f, 0.0f};
  b.gaze = {0.0f, 1.0f, 0.0f};
  b.vitals = std::array<float, 9>{90, 80, 70, 60, 0, 0, 100, 0, 0};
  return b;
}

const sdk::Value* field(const sdk::Value& message, const std::string& name) {
  for (const auto& [key, value] : message.as_map()) {
    if (key == name) {
      return &value;
    }
  }
  return nullptr;
}

sdk::Value wanted() {
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("localBodyWanted"))}});
}

}  // namespace

KCD2S_TEST(local_body, a_change_smaller_than_a_reading_shows_is_no_change) {
  std::optional<body::Body> a = standing();
  std::optional<body::Body> b = standing();
  b->position.x += 0.0004f;
  b->vitals->at(1) += 0.004f;
  KCD2S_CHECK(!body::differs(a, b));
  b->position.x += 0.002f;
  KCD2S_CHECK(body::differs(a, b));
  b = standing();
  b->vitals->at(1) -= 1.0f;
  KCD2S_CHECK(body::differs(a, b));
  b = standing();
  b->vitals.reset();
  KCD2S_CHECK(body::differs(a, b));
  KCD2S_CHECK(body::differs(a, std::nullopt));
  KCD2S_CHECK(!body::differs(std::nullopt, std::nullopt));
}

KCD2S_TEST(local_body, the_message_names_every_reading) {
  const sdk::Value message = body::message_of(standing());
  KCD2S_CHECK(field(message, "op")->as_str() == "localBody");
  const auto& q = field(message, "q")->as_array();
  KCD2S_CHECK_EQ(q.size(), std::size_t(4));
  KCD2S_CHECK(q[0].as_double() > 0.7 && q[3].as_double() > 0.7);  // w first, then x, y, z
  KCD2S_CHECK_EQ(field(message, "vitals")->as_array().size(), std::size_t(9));
  KCD2S_CHECK(field(message, "p")->as_array()[2].as_double() == 30.0);
  const sdk::Value none = body::message_of(std::nullopt);
  KCD2S_CHECK(field(none, "p") == nullptr);
}

KCD2S_TEST(local_body, it_is_sent_only_once_asked_and_then_on_change) {
  std::vector<sdk::Value> sent;
  const auto send = [&sent](const sdk::Value& message) {
    sent.push_back(message);
    return true;
  };
  const int session = 0;
  body::set(standing());
  body::pump(&session, send, true);
  KCD2S_CHECK(sent.empty());
  KCD2S_CHECK(body::handle_runtime_message(wanted()));
  body::pump(&session, send, true);
  KCD2S_CHECK_EQ(sent.size(), std::size_t(1));
  body::pump(&session, send, true);
  KCD2S_CHECK_EQ(sent.size(), std::size_t(1));
  body::Body moved = standing();
  moved.position.y += 1.0f;
  body::set(moved);
  body::pump(&session, send, true);
  KCD2S_CHECK_EQ(sent.size(), std::size_t(2));
  // Out of the world: one word that there is no body.
  body::pump(&session, send, false);
  KCD2S_CHECK_EQ(sent.size(), std::size_t(3));
  KCD2S_CHECK(field(sent.back(), "p") == nullptr);
  // A new runtime asks again.
  const int next = 0;
  body::set(standing());
  body::pump(&next, send, true);
  KCD2S_CHECK_EQ(sent.size(), std::size_t(3));
}
