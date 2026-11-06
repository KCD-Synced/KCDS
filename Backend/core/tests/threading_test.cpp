#include "kcd2s/core/threading.hpp"

#include <atomic>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::core::ThreadPool;

KCD2S_TEST(threading, pool_has_workers) {
  ThreadPool pool(4);
  KCD2S_CHECK_EQ(pool.size(), std::size_t{4});
  ThreadPool autop;  // hardware_concurrency
  KCD2S_CHECK(autop.size() >= 1);
}

KCD2S_TEST(threading, parallel_for_covers_every_index) {
  ThreadPool pool(4);
  const std::size_t n = 10000;
  std::vector<int> hits(n, 0);
  pool.parallel_for(n, [&](std::size_t i) { hits[i] = 1; });  // disjoint writes
  KCD2S_CHECK_EQ(std::accumulate(hits.begin(), hits.end(), 0), static_cast<int>(n));
}

KCD2S_TEST(threading, parallel_for_sum_matches_serial) {
  ThreadPool pool(8);
  const std::size_t n = 50000;
  std::vector<std::uint64_t> vals(n);
  for (std::size_t i = 0; i < n; ++i)
    vals[i] = i;

  std::vector<std::uint64_t> partial(n, 0);
  pool.parallel_for(n, [&](std::size_t i) { partial[i] = vals[i] * 2; });

  std::uint64_t expected = 0;
  for (std::size_t i = 0; i < n; ++i)
    expected += vals[i] * 2;
  std::uint64_t got = std::accumulate(partial.begin(), partial.end(), std::uint64_t{0});
  KCD2S_CHECK_EQ(got, expected);
}

KCD2S_TEST(threading, parallel_for_edge_counts) {
  ThreadPool pool(4);
  std::atomic<int> c0{0}, c1{0};
  pool.parallel_for(0, [&](std::size_t) { c0.fetch_add(1); });  // no-op
  pool.parallel_for(1, [&](std::size_t) { c1.fetch_add(1); });  // runs inline
  KCD2S_CHECK_EQ(c0.load(), 0);
  KCD2S_CHECK_EQ(c1.load(), 1);
}

KCD2S_TEST(threading, concurrent_enqueue_all_run) {
  ThreadPool pool(4);
  std::atomic<int> counter{0};
  const int tasks = 2000;
  // Enqueue many tasks and drain via a parallel_for barrier afterwards.
  for (int i = 0; i < tasks; ++i)
    pool.enqueue([&] { counter.fetch_add(1, std::memory_order_relaxed); });
  // Spin until all enqueued tasks have run (bounded; they will).
  for (int spins = 0; spins < 100000 && counter.load() < tasks; ++spins) {
    std::this_thread::yield();
  }
  KCD2S_CHECK_EQ(counter.load(), tasks);
}

KCD2S_TEST(threading, parallel_for_hands_an_exception_to_the_caller) {
  ThreadPool pool(4);
  std::atomic<std::uint64_t> ran{0};
  bool caught = false;
  try {
    pool.parallel_for(400, [&](std::size_t i) {
      if (i == 7)
        throw std::runtime_error("index 7");
      ran.fetch_add(1, std::memory_order_relaxed);
    });
  } catch (const std::runtime_error&) {
    caught = true;
  }
  KCD2S_CHECK(caught);
  // The other chunks ran to their end; the throwing chunk (indices 0-99) stopped at 7.
  KCD2S_CHECK_EQ(ran.load(), std::uint64_t{307});
  // The pool still works afterwards.
  std::atomic<std::uint64_t> after{0};
  pool.parallel_for(100, [&](std::size_t) { after.fetch_add(1, std::memory_order_relaxed); });
  KCD2S_CHECK_EQ(after.load(), std::uint64_t{100});
}

KCD2S_TEST(threading, repeated_parallel_for_reuses_pool) {
  ThreadPool pool(4);
  std::atomic<std::uint64_t> total{0};
  for (int round = 0; round < 50; ++round)
    pool.parallel_for(1000, [&](std::size_t) { total.fetch_add(1, std::memory_order_relaxed); });
  KCD2S_CHECK_EQ(total.load(), std::uint64_t{50 * 1000});
}
