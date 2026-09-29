/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE RowGroupCache
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mzpeak/util/row_group_cache.h"

using MzPeak::Util::RowGroupCache;
using MzPeak::Util::RowGroupBatches;

namespace {

/// A decode that does no Parquet work but is observable: it records how many
/// decodes overlap, and how many bytes they claim while they do.
struct Probe {
  std::mutex mutex;
  std::size_t in_flight_bytes = 0;
  std::size_t peak_bytes = 0;
  int concurrent = 0;
  int peak_concurrent = 0;
  std::atomic<int> decodes{0};

  RowGroupCache::Batches run(std::size_t bytes, std::chrono::milliseconds hold)
  {
    {
      std::lock_guard<std::mutex> guard(mutex);
      in_flight_bytes += bytes;
      peak_bytes = std::max(peak_bytes, in_flight_bytes);
      peak_concurrent = std::max(peak_concurrent, ++concurrent);
    }
    ++decodes;
    std::this_thread::sleep_for(hold);
    {
      std::lock_guard<std::mutex> guard(mutex);
      in_flight_bytes -= bytes;
      --concurrent;
    }
    return std::make_shared<const RowGroupBatches>();
  }
};

} // namespace

/******************************************************************************/
/// A group is decoded once however many readers ask for it.
BOOST_AUTO_TEST_CASE(one_decode_per_group)
{
  RowGroupCache cache(64 * 1024 * 1024);
  Probe probe;

  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&] {
      cache.get("f", 7, 1024, [&] { return probe.run(1024, std::chrono::milliseconds(20)); });
    });
  }
  for (auto& t : threads) t.join();

  BOOST_CHECK_EQUAL(probe.decodes.load(), 1);
  BOOST_CHECK_EQUAL(cache.stats().decodes, 1u);
  BOOST_CHECK_EQUAL(cache.stats().hits + cache.stats().waits, 15u);
}

/******************************************************************************/
/// THE POINT OF ADMISSION CONTROL: many readers over DISTINCT groups must not
/// pin more than the budget, however many of them there are.  Without it every
/// thread claims a group that eviction cannot touch while it decodes, and peak
/// memory follows the thread count instead.
BOOST_AUTO_TEST_CASE(in_flight_bytes_stay_within_budget)
{
  constexpr std::size_t kGroup = 1024 * 1024; // 1 MB
  constexpr std::size_t kBudget = 8 * kGroup; // half of it may decode at once
  constexpr int kThreads = 32;                // four times what the budget allows

  RowGroupCache cache(kBudget);
  Probe probe;

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      cache.get("f", i, kGroup, [&] { return probe.run(kGroup, std::chrono::milliseconds(30)); });
    });
  }
  for (auto& t : threads) t.join();

  // Every group really was decoded -- the bound must not come from skipping work.
  BOOST_CHECK_EQUAL(probe.decodes.load(), kThreads);
  // And never more than half the budget was in flight at once.
  BOOST_CHECK_LE(probe.peak_bytes, kBudget / 2);
  BOOST_CHECK_LE(probe.peak_concurrent, static_cast<int>(kBudget / 2 / kGroup));
  // Which only means something if the threads really did overlap.
  BOOST_CHECK_GT(probe.peak_concurrent, 1);
  BOOST_CHECK_GT(cache.stats().admission_waits, 0u);
}

/******************************************************************************/
/// A group larger than the whole budget still gets decoded, alone, rather than
/// waiting for room that can never appear.
BOOST_AUTO_TEST_CASE(oversized_group_makes_progress)
{
  RowGroupCache cache(1024); // smaller than one group
  Probe probe;

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      cache.get("f", i, 4 * 1024 * 1024,
                [&] { return probe.run(4 * 1024 * 1024, std::chrono::milliseconds(5)); });
    });
  }
  for (auto& t : threads) t.join();

  BOOST_CHECK_EQUAL(probe.decodes.load(), 8);
  BOOST_CHECK_EQUAL(probe.peak_concurrent, 1); // serialised, but never stuck
}

/******************************************************************************/
/// A throwing decode reaches its waiters, frees its admission, and leaves the
/// entry retryable -- a leaked in-flight claim would wedge every later reader.
BOOST_AUTO_TEST_CASE(failed_decode_releases_admission)
{
  RowGroupCache cache(4 * 1024 * 1024);

  BOOST_CHECK_THROW(cache.get("f", 1, 1024 * 1024,
                              []() -> RowGroupCache::Batches {
                                throw std::runtime_error("decode failed");
                              }),
                    std::runtime_error);

  // The budget is whole again: a full-budget decode is admitted immediately.
  bool decoded = false;
  cache.get("f", 2, 4 * 1024 * 1024, [&] {
    decoded = true;
    return std::make_shared<const RowGroupBatches>();
  });
  BOOST_CHECK(decoded);

  // And the failed entry was forgotten rather than cached as a failure.
  bool retried = false;
  cache.get("f", 1, 1024 * 1024, [&] {
    retried = true;
    return std::make_shared<const RowGroupBatches>();
  });
  BOOST_CHECK(retried);
}

/******************************************************************************/
namespace {
RowGroupCache::Batches empty_group() { return std::make_shared<const RowGroupBatches>(); }

/// Readers of a file whose groups decode instantly, counting decodes ahead.
RowGroupCache::Ahead instant_ahead(int32_t groups, std::size_t bytes, std::atomic<int>& calls)
{
  RowGroupCache::Ahead ahead;
  ahead.groups = groups;
  ahead.bytes = [bytes](int32_t) { return bytes; };
  ahead.decode = [&calls](int32_t) {
    ++calls;
    return empty_group();
  };
  return ahead;
}

/// Starts decoding group 0 of "f" on another thread and returns once it is in
/// flight; the decode finishes after @p hold.
std::thread slow_first_decode(RowGroupCache& cache, std::size_t bytes,
                              std::chrono::milliseconds hold)
{
  std::atomic<bool> started{false};
  std::thread first([&cache, &started, bytes, hold] {
    cache.get("f", 0, bytes, [&] {
      started = true;
      std::this_thread::sleep_for(hold);
      return empty_group();
    });
  });
  while (!started) std::this_thread::yield();
  return first;
}
} // namespace

/******************************************************************************/
/// A reader that finds its group IN FLIGHT decodes the next groups nobody has
/// claimed instead of idling, and a later reader of one of those finds it
/// ready rather than decoding it again.
BOOST_AUTO_TEST_CASE(waiting_reader_decodes_ahead)
{
  constexpr std::size_t kGroup = 1024 * 1024;
  RowGroupCache cache(64 * kGroup);
  std::atomic<int> calls{0};
  const auto ahead = instant_ahead(4, kGroup, calls);

  std::thread first = slow_first_decode(cache, kGroup, std::chrono::milliseconds(200));
  cache.get("f", 0, kGroup,
            []() -> RowGroupCache::Batches { throw std::logic_error("decoded twice"); },
            &ahead);
  first.join();

  BOOST_CHECK_EQUAL(calls.load(), 3); // groups 1, 2 and 3: the rest of the file
  BOOST_CHECK_EQUAL(cache.stats().ahead_decodes, 3u);
  BOOST_CHECK_EQUAL(cache.stats().decodes, 4u);

  bool decoded = false;
  cache.get("f", 2, kGroup, [&] {
    decoded = true;
    return empty_group();
  });
  BOOST_CHECK(!decoded);
}

/******************************************************************************/
/// Decoding ahead stays inside the budget, and never by evicting what it
/// decoded ahead before: a waiting reader stops once the groups it would add
/// no longer fit next to the ones in flight or not yet asked for.
BOOST_AUTO_TEST_CASE(decoding_ahead_stays_within_budget)
{
  constexpr std::size_t kGroup = 1024 * 1024;
  RowGroupCache cache(4 * kGroup);
  std::atomic<int> calls{0};
  const auto ahead = instant_ahead(10, kGroup, calls);

  std::thread first = slow_first_decode(cache, kGroup, std::chrono::milliseconds(200));
  cache.get("f", 0, kGroup, []() -> RowGroupCache::Batches { return empty_group(); }, &ahead);
  first.join();

  // Group 0 in flight plus three decoded ahead fill the four-group budget.
  BOOST_CHECK_EQUAL(calls.load(), 3);
  BOOST_CHECK_EQUAL(cache.stats().evictions, 0u);
  BOOST_CHECK_LE(cache.stats().held_bytes, 4 * kGroup);
}

/******************************************************************************/
/// A group a reader still holds is never the one evicted: dropping it frees
/// nothing (the reader keeps it alive) and makes the next reader decode it
/// again.
BOOST_AUTO_TEST_CASE(held_groups_are_not_evicted)
{
  constexpr std::size_t kGroup = 1024 * 1024;
  RowGroupCache cache(kGroup); // room for one

  auto a = cache.get("f", 0, kGroup, empty_group);
  auto b = cache.get("f", 1, kGroup, empty_group);
  BOOST_CHECK_EQUAL(cache.stats().evictions, 0u); // both held: overshoot instead

  a.reset();
  cache.get("f", 2, kGroup, empty_group); // evicts the released group 0, not 1
  BOOST_CHECK_EQUAL(cache.stats().evictions, 1u);

  bool decoded = false;
  cache.get("f", 1, kGroup, [&] {
    decoded = true;
    return empty_group();
  });
  BOOST_CHECK(!decoded);
}
