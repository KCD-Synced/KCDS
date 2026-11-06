#include "../local/script_inbox.hpp"
#ifdef _WIN32
#include "../local/script_process.hpp"
#else
#include "../local/script_process_posix.hpp"
#endif
#include "kcd2s/testkit.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace kcd2s;
using namespace kcd2s::local;

namespace {
const sdk::Value& field(const sdk::Value& value, const char* key) {
  for (const auto& [name, entry] : value.as_map()) {
    if (name == key)
      return entry;
  }
  throw std::runtime_error("missing fixture field");
}

template <class F>
bool overflowed(F operation) {
  try {
    operation();
  } catch (const ScriptInboxOverflow& e) {
    return std::string(e.what()).find("session failed") != std::string::npos;
  }
  return false;
}

template <class F>
bool until(F condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return condition();
}
}  // namespace

KCD2S_TEST(script_inbox, preserves_fifo_reuse_and_ignores_malformed_values) {
  ScriptInbox inbox;
  KCD2S_CHECK(inbox.push(sdk::encode(sdk::Value(1))));
  KCD2S_CHECK(inbox.push({255}));
  KCD2S_CHECK(inbox.push(sdk::encode(sdk::Value(2))));
  auto first = inbox.take();
  KCD2S_CHECK_EQ(first.size(), std::size_t{2});
  if (first.size() == 2) {
    KCD2S_CHECK_EQ(first[0].as_int(), 1);
    KCD2S_CHECK_EQ(first[1].as_int(), 2);
  }
  KCD2S_CHECK_EQ(inbox.state().messages, std::size_t{0});
  KCD2S_CHECK_EQ(inbox.state().bytes, std::size_t{0});
  KCD2S_CHECK(inbox.push(sdk::encode(sdk::Value("next"))));
  auto second = inbox.take();
  KCD2S_CHECK_EQ(second.size(), std::size_t{1});
  if (!second.empty())
    KCD2S_CHECK_EQ(second[0].as_str(), std::string("next"));
}

KCD2S_TEST(script_inbox, message_limit_is_explicit_sticky_and_does_not_evade_byte_accounting) {
  ScriptInbox inbox;
  for (std::size_t i = 0; i < ScriptInbox::kMaxMessages; ++i) {
    KCD2S_CHECK(inbox.push(sdk::encode(sdk::Value(std::int64_t(i)))));
  }
  const auto before = inbox.state();
  KCD2S_CHECK(!inbox.push(sdk::encode(sdk::Value("overflow"))));
  KCD2S_CHECK(inbox.state().failed);
  KCD2S_CHECK_EQ(inbox.state().messages, before.messages);
  KCD2S_CHECK_EQ(inbox.state().bytes, before.bytes);
  KCD2S_CHECK(overflowed([&] { inbox.take(); }));
  KCD2S_CHECK(!inbox.push({0}));
  KCD2S_CHECK(overflowed([&] { inbox.check(); }));
}

KCD2S_TEST(script_inbox, allocation_bytes_are_bounded_even_with_spare_vector_capacity) {
  ScriptInbox inbox;
  std::vector<std::uint8_t> payload(1, 0);
  payload.reserve(ScriptInbox::kMaxBytes);
  const auto allocated = payload.capacity();
  KCD2S_CHECK(inbox.push(std::move(payload)));
  KCD2S_CHECK_EQ(inbox.state().bytes, allocated);
  KCD2S_CHECK(!inbox.push({0}));
  KCD2S_CHECK_EQ(inbox.state().messages, std::size_t{1});
  KCD2S_CHECK(overflowed([&] { inbox.take(); }));
}

KCD2S_TEST(script_inbox, actual_child_preserves_1100_spawn_pairs_and_interleaved_logs) {
  ScriptProcess child(process_text(KCDS_SCRIPT_TEST_CHILD), {process_text("burst")});
  // >3 MiB of hex input versus a 1 MiB Windows pipe (smaller on POSIX), concurrent with
  // the child's >2 MiB stdout burst. Waiting for mailbox space in the reader would deadlock.
  for (int i = 0; i < 32; ++i)
    KCD2S_CHECK(child.send(sdk::Value(std::string(60000, 'p'))));
  KCD2S_CHECK(until([&] { return child.inbox_state().messages == 3301; }));
  const auto messages = child.take();
  KCD2S_CHECK_EQ(messages.size(), std::size_t{3301});
  if (messages.size() != 3301)
    return;
  for (std::size_t i = 0; i < 1100; ++i) {
    const auto& position = messages[3 * i];
    const auto& log = messages[3 * i + 1];
    const auto& rotation = messages[3 * i + 2];
    KCD2S_CHECK_EQ(field(position, "op").as_str(), std::string("entityPosition"));
    KCD2S_CHECK_EQ(field(log, "op").as_str(), std::string("log"));
    KCD2S_CHECK_EQ(field(rotation, "op").as_str(), std::string("entityRotation"));
    for (const auto* command : {&position, &log, &rotation})
      KCD2S_CHECK_EQ(field(*command, "id").as_int(), std::int64_t(i + 1));
    KCD2S_CHECK_EQ(field(field(position, "position"), "x").as_double(), double(i + 1));
    KCD2S_CHECK_EQ(field(field(rotation, "rotation"), "w").as_double(), 1.0);
    KCD2S_CHECK_EQ(field(log, "text").as_str().size(), std::size_t{1024});
  }
  KCD2S_CHECK_EQ(messages.back().as_str(), std::string("done"));
  KCD2S_CHECK_EQ(child.counters().inbox_drops, std::uint64_t{0});
  KCD2S_CHECK(until([&] { return !child.alive(); }));
}

KCD2S_TEST(script_inbox, overflowing_child_keeps_draining_and_exits_without_pipe_deadlock) {
  ScriptProcess child(process_text(KCDS_SCRIPT_TEST_CHILD), {process_text("overflow")});
  KCD2S_CHECK(until([&] { return child.inbox_state().failed; }));
  KCD2S_CHECK(overflowed([&] { child.take(); }));
  KCD2S_CHECK(overflowed([&] { child.send(sdk::Value("cannot continue")); }));
  KCD2S_CHECK(until([&] { return !child.alive(); }));
  const auto state = child.inbox_state();
  KCD2S_CHECK(state.messages <= ScriptInbox::kMaxMessages);
  KCD2S_CHECK(state.bytes <= ScriptInbox::kMaxBytes);
  KCD2S_CHECK(state.failed);
}

KCD2S_TEST(script_outbox, push_does_not_wait_for_the_writer_and_keeps_order) {
  std::mutex gate;
  std::vector<std::string> written;
  std::unique_lock held(gate);
  ScriptOutbox outbox;
  outbox.start(
      [&](const std::string& line) {
        std::lock_guard open(gate);
        written.push_back(line);
        return true;
      },
      nullptr);
  double waited = 0.0;
  for (int i = 0; i < 100; ++i) {
    KCD2S_CHECK(outbox.push(std::to_string(i), waited));
  }
  KCD2S_CHECK_EQ(waited, 0.0);
  held.unlock();
  outbox.finish();
  outbox.join();
  KCD2S_CHECK_EQ(written.size(), std::size_t{100});
  for (std::size_t i = 0; i < written.size(); ++i) {
    KCD2S_CHECK_EQ(written[i], std::to_string(i));
  }
}

KCD2S_TEST(script_outbox, a_full_queue_waits_for_room) {
  std::atomic<bool> release{false};
  ScriptOutbox outbox(10);
  outbox.start(
      [&](const std::string&) {
        while (!release.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
      },
      nullptr);
  double waited = 0.0;
  KCD2S_CHECK(outbox.push(std::string(10, 'a'), waited));
  std::thread opener([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    release.store(true);
  });
  KCD2S_CHECK(outbox.push(std::string(10, 'b'), waited));
  opener.join();
  KCD2S_CHECK(waited > 0.0);
  KCD2S_CHECK(outbox.queued_bytes() <= std::size_t{10});
}

KCD2S_TEST(script_outbox, a_broken_pipe_refuses_lines) {
  ScriptOutbox outbox(10);
  outbox.start([](const std::string&) { return false; }, nullptr);
  double waited = 0.0;
  outbox.push("first", waited);
  KCD2S_CHECK(until([&] { return !outbox.push("next", waited); }));
}

KCD2S_TEST(script_outbox, closes_only_after_the_queue_was_written) {
  std::vector<std::string> events;
  ScriptOutbox outbox;
  outbox.start(
      [&](const std::string& line) {
        events.push_back(line);
        return true;
      },
      [&] { events.push_back("closed"); });
  double waited = 0.0;
  outbox.push("a", waited);
  outbox.push("b", waited);
  outbox.finish();
  outbox.join();
  KCD2S_CHECK_EQ(events.size(), std::size_t{3});
  if (events.size() == 3) {
    KCD2S_CHECK_EQ(events[2], std::string("closed"));
  }
  KCD2S_CHECK(!outbox.push("late", waited));
}

KCD2S_TEST(script_inbox, send_does_not_wait_for_a_host_that_is_not_reading) {
  ScriptProcess child(process_text(KCDS_SCRIPT_TEST_CHILD), {process_text("slow")});
  // Far more than the pipe holds (1 MiB on Windows, less on POSIX) while the child reads nothing.
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < 64; ++i) {
    KCD2S_CHECK(child.send(sdk::Value(std::string(60000, 'p'))));
  }
  const auto took = std::chrono::steady_clock::now() - started;
  KCD2S_CHECK(took < std::chrono::milliseconds(1000));
  KCD2S_CHECK(until([&] { return child.inbox_state().messages == 1; }));
  const auto messages = child.take();
  KCD2S_CHECK_EQ(messages.size(), std::size_t{1});
  if (messages.size() == 1) {
    KCD2S_CHECK_EQ(messages[0].as_int(), std::int64_t{64});
  }
}
