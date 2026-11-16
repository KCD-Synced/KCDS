// The server's output digest (server_output_digest.hpp) is a reliable guard: the same session
// sends the same bytes every run, and the bytes do not depend on how many snapshot workers build
// them. Without these, a matching digest before and after a change would prove nothing.
#include "server_output_digest.hpp"

#include "kcd2s/testkit.hpp"

namespace {

kcds::digest::DigestOptions small_session(unsigned workers) {
  kcds::digest::DigestOptions options;
  options.clients = 24;
  options.seconds = 12;
  options.workers = workers;
  return options;
}

}  // namespace

KCD2S_TEST(server_output_digest, the_same_session_sends_the_same_bytes) {
  const auto first = kcds::digest::run_digest(small_session(1));
  const auto second = kcds::digest::run_digest(small_session(1));
  KCD2S_CHECK(first.datagrams > 1000);
  KCD2S_CHECK(first.mirrored_entities > 24);
  KCD2S_CHECK_EQ(first.value, second.value);
  KCD2S_CHECK_EQ(first.datagrams, second.datagrams);
  KCD2S_CHECK_EQ(first.bytes, second.bytes);
}

KCD2S_TEST(server_output_digest, snapshot_workers_do_not_change_the_bytes) {
  const auto serial = kcds::digest::run_digest(small_session(1));
  const auto parallel = kcds::digest::run_digest(small_session(4));
  KCD2S_CHECK_EQ(serial.value, parallel.value);
  KCD2S_CHECK_EQ(serial.bytes, parallel.bytes);
}
