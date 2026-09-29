/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE DecodePool
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "mzpeak/util/decode_pool.h"

using MzPeak::Util::DecodePool;

namespace {

constexpr int64_t KB = 1024;
constexpr int64_t MB = 1024 * KB;

/// A buffer the tests own, with the pattern it was filled with.
struct Buf {
  uint8_t* p = nullptr;
  int64_t size = 0;
  uint8_t seed = 0;
};

uint8_t pattern(const Buf& b, int64_t i)
{
  return static_cast<uint8_t>(b.seed * 7 + i / 4096 + (i & 4095));
}

void fill(const Buf& b)
{
  // The first byte of every page and the last byte, not all of it: enough to
  // catch two owners of one block without making the stress test memory-bound.
  for (int64_t i = 0; i < b.size; i += 4096) b.p[i] = pattern(b, i);
  if (b.size > 0) b.p[b.size - 1] = pattern(b, b.size - 1);
}

/// Whether the first @p upto bytes still hold fill()'s pattern.
bool intact(const Buf& b, int64_t upto)
{
  upto = std::min(upto, b.size);
  for (int64_t i = 0; i < upto; i += 4096) {
    if (b.p[i] != pattern(b, i)) return false;
  }
  return upto < b.size || upto == 0 || b.p[upto - 1] == pattern(b, upto - 1);
}

/// No Boost.Test assertions here: it is called on worker threads too, and
/// the framework's checks are not thread-safe.  A failure leaves p null.
Buf make(DecodePool& pool, int64_t size, uint8_t seed)
{
  Buf b{nullptr, size, seed};
  if (!pool.Allocate(size, &b.p).ok() || reinterpret_cast<uintptr_t>(b.p) % 64 != 0) {
    return Buf{};
  }
  fill(b);
  return b;
}

Buf alloc(DecodePool& pool, int64_t size, uint8_t seed)
{
  Buf b = make(pool, size, seed);
  BOOST_REQUIRE(b.p != nullptr);
  return b;
}

/// The pool's bound: cached bytes never exceed the floor, a quarter of the
/// live peak, or the room below it; mapped bytes are exactly live plus cached.
/// @p peak_mapped is the all-time peak, never below the pool's recent one.
void check_bound(const DecodePool& pool, int64_t floor, int64_t live_mapped, int64_t peak_mapped)
{
  const int64_t cached = pool.bytes_cached();
  BOOST_CHECK_LE(cached, std::max({floor, peak_mapped / 4, peak_mapped - live_mapped}));
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), live_mapped + cached);
}

} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(size_classes)
{
  int64_t last = 0;
  for (int64_t s = DecodePool::kMinBlock; s < 64 * MB; s += s / 7 + 4093) {
    const int64_t b = DecodePool::block_size(s);
    BOOST_CHECK_GE(b, s);
    BOOST_CHECK_LE(b, s + s / 4 + DecodePool::kMinBlock); // <= 25% over, or one granule
    BOOST_CHECK_EQUAL(b % DecodePool::kMinBlock, 0);
    BOOST_CHECK_GE(b, last);
    last = b;
  }
  // Parquet's batch buffers are exact powers of two; they must not be rounded.
  for (int64_t s : {64 * KB, 256 * KB, 512 * KB, 1 * MB, 4 * MB}) {
    BOOST_CHECK_EQUAL(DecodePool::block_size(s), s);
  }
  BOOST_CHECK_EQUAL(DecodePool::block_size(4 * MB + 1), 5 * MB);
}

/******************************************************************************/
/// Blocks of 2 MB and up are mapped 2 MB-aligned, so the kernel can back them
/// with huge pages; the alignment trim must leave exactly the block mapped.
BOOST_AUTO_TEST_CASE(huge_blocks)
{
  DecodePool pool;
  Buf a = alloc(pool, 4 * MB - 12345, 5);
  Buf b = alloc(pool, 9 * MB, 6);
#ifdef __linux__
  BOOST_CHECK_EQUAL(reinterpret_cast<uintptr_t>(a.p) % DecodePool::kHugePage, 0u);
  BOOST_CHECK_EQUAL(reinterpret_cast<uintptr_t>(b.p) % DecodePool::kHugePage, 0u);
#endif
  std::memset(a.p, 1, static_cast<std::size_t>(a.size));
  std::memset(b.p, 2, static_cast<std::size_t>(b.size));
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), DecodePool::block_size(a.size) + DecodePool::block_size(b.size));
  pool.Free(a.p, a.size);
  pool.Free(b.p, b.size);
  pool.ReleaseUnused();
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 0);
}

/******************************************************************************/
/// Small buffers and alignments above 64 go to the system allocator: no blocks.
BOOST_AUTO_TEST_CASE(passthrough)
{
  DecodePool pool;
  uint8_t* small = nullptr;
  BOOST_REQUIRE(pool.Allocate(1000, &small).ok());
  uint8_t* aligned = nullptr;
  BOOST_REQUIRE(pool.Allocate(1 * MB, 4096, &aligned).ok());
  BOOST_CHECK_EQUAL(reinterpret_cast<uintptr_t>(aligned) % 4096, 0u);
  std::memset(aligned, 7, 1 * MB);
  BOOST_REQUIRE(pool.Reallocate(1 * MB, 2 * MB, 4096, &aligned).ok());
  BOOST_CHECK_EQUAL(reinterpret_cast<uintptr_t>(aligned) % 4096, 0u);
  BOOST_CHECK_EQUAL(aligned[1 * MB - 1], 7);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 1000 + 2 * MB);

  pool.Free(aligned, 2 * MB, 4096);
  pool.Free(small, 1000);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 0);

  uint8_t* zero = nullptr;
  BOOST_REQUIRE(pool.Allocate(0, &zero).ok());
  pool.Free(zero, 0);
}

/******************************************************************************/
/// Arrow's accounting, in requested bytes, whichever path served a buffer.
BOOST_AUTO_TEST_CASE(accounting_is_truthful)
{
  DecodePool pool;
  Buf a = alloc(pool, 300 * KB, 1);
  Buf b = alloc(pool, 10 * KB, 2);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 310 * KB);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), DecodePool::block_size(300 * KB));
  BOOST_REQUIRE(pool.Reallocate(300 * KB, 900 * KB, &a.p).ok());
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 910 * KB);
  BOOST_CHECK_EQUAL(pool.max_memory(), 910 * KB);
  BOOST_CHECK_EQUAL(pool.total_bytes_allocated(), 910 * KB);
  BOOST_CHECK_EQUAL(pool.num_allocations(), 3);
  pool.Free(a.p, 900 * KB);
  pool.Free(b.p, 10 * KB);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 0);
  BOOST_CHECK_EQUAL(pool.max_memory(), 910 * KB);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), pool.bytes_cached());
}

/******************************************************************************/
/// Freed on another thread, reused on a third: the block does not belong to
/// the thread that mapped it.
BOOST_AUTO_TEST_CASE(cross_thread_free_and_reuse)
{
  DecodePool pool;
  Buf b;
  std::thread([&] { b = make(pool, 2 * MB, 3); }).join();
  BOOST_REQUIRE(b.p != nullptr);

  bool was_intact = false;
  std::thread([&] {
    was_intact = intact(b, b.size);
    pool.Free(b.p, b.size);
  }).join();
  BOOST_CHECK(was_intact);
  BOOST_CHECK_EQUAL(pool.bytes_cached(), 2 * MB);

  uint8_t* again = nullptr;
  bool ok = false;
  std::thread([&] { ok = pool.Allocate(2 * MB - 100, &again).ok(); }).join();
  BOOST_REQUIRE(ok);
  BOOST_CHECK(again == b.p); // same class: the cached block, not a new mapping
  BOOST_CHECK_EQUAL(pool.bytes_cached(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 2 * MB);
  pool.Free(again, 2 * MB - 100);
}

/******************************************************************************/
/// THE CASE THAT CRASHED ARROW'S ALLOCATORS: the allocating thread has exited
/// (joined) before its buffer is freed, and before the block is used again.
BOOST_AUTO_TEST_CASE(free_after_allocating_thread_exited)
{
  DecodePool pool;
  std::vector<Buf> bufs;
  std::thread([&] {
    for (int i = 0; i < 8; ++i) bufs.push_back(make(pool, (i + 1) * 256 * KB, uint8_t(i)));
  }).join();
  const int64_t mapped = pool.bytes_mapped();

  for (const auto& b : bufs) {
    BOOST_REQUIRE(b.p != nullptr);
    BOOST_CHECK(intact(b, b.size));
    pool.Free(b.p, b.size);
  }
  // And the blocks go on to serve the next thread, without new mappings.
  int bad = 0;
  std::thread([&] {
    for (int i = 0; i < 8; ++i) {
      Buf b = make(pool, (i + 1) * 256 * KB, uint8_t(i + 100));
      if (!b.p || !intact(b, b.size)) ++bad;
      if (b.p) pool.Free(b.p, b.size);
    }
  }).join();
  BOOST_CHECK_EQUAL(bad, 0);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), mapped);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 0);
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(reallocate)
{
  DecodePool pool;
  Buf b = alloc(pool, 300 * KB, 9);

  // Within a class (300 KB to 320 KB is one) a block has the room: in place.
  uint8_t* before = b.p;
  BOOST_REQUIRE(pool.Reallocate(300 * KB, 320 * KB, &b.p).ok());
  BOOST_CHECK(b.p == before);
  BOOST_REQUIRE(pool.Reallocate(320 * KB, 256 * KB + 1, &b.p).ok());
  BOOST_CHECK(b.p == before);
  BOOST_CHECK(intact(b, 256 * KB));
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 320 * KB);

  // Across classes: moved, contents kept, the old block cached.
  BOOST_REQUIRE(pool.Reallocate(256 * KB + 1, 3 * MB, &b.p).ok());
  BOOST_CHECK(b.p != before);
  BOOST_CHECK(intact(b, 256 * KB));
  BOOST_CHECK_EQUAL(pool.bytes_cached(), 320 * KB);
  b.size = 3 * MB;
  fill(b);
  BOOST_REQUIRE(pool.Reallocate(3 * MB, 700 * KB, &b.p).ok());
  BOOST_CHECK(intact(b, 700 * KB));
  b.size = 700 * KB;
  fill(b);

  // Down to the system allocator and back up.
  BOOST_REQUIRE(pool.Reallocate(700 * KB, 20 * KB, &b.p).ok());
  BOOST_CHECK(intact(b, 20 * KB));
  b.size = 20 * KB;
  fill(b);
  BOOST_REQUIRE(pool.Reallocate(20 * KB, 5 * MB, &b.p).ok());
  BOOST_CHECK(intact(b, 20 * KB));
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 5 * MB);

  // To and from zero, as an empty PoolBuffer does.
  BOOST_REQUIRE(pool.Reallocate(5 * MB, 0, &b.p).ok());
  BOOST_REQUIRE(pool.Reallocate(0, 1 * MB, &b.p).ok());
  std::memset(b.p, 1, 1 * MB);
  pool.Free(b.p, 1 * MB);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), pool.bytes_cached());
}

/******************************************************************************/
/// Churn with sizes that rarely repeat (what defeated exact 64 KB classes):
/// the cache stays within its bound, and what exceeds it is unmapped.
BOOST_AUTO_TEST_CASE(bound_holds_under_churn)
{
  const int64_t floor = 4 * MB;
  DecodePool pool(floor);
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int64_t> size(64 * KB, 6 * MB);
  std::vector<Buf> live;
  int64_t live_mapped = 0, peak_mapped = 0;

  for (int round = 0; round < 4000; ++round) {
    // Phases: grow the live set, then shrink it, as a cache filling and an
    // eviction burst would.
    const bool grow = (round / 250) % 2 == 0;
    if (grow || live.empty()) {
      Buf b = alloc(pool, size(rng), uint8_t(round));
      live.push_back(b);
      live_mapped += DecodePool::block_size(b.size);
    } else {
      const std::size_t i = rng() % live.size();
      BOOST_REQUIRE(intact(live[i], live[i].size));
      pool.Free(live[i].p, live[i].size);
      live_mapped -= DecodePool::block_size(live[i].size);
      live[i] = live.back();
      live.pop_back();
    }
    peak_mapped = std::max(peak_mapped, live_mapped);
    check_bound(pool, floor, live_mapped, peak_mapped);
  }
  for (const auto& b : live) pool.Free(b.p, b.size);
  check_bound(pool, floor, 0, peak_mapped);
  pool.ReleaseUnused();
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), 0);
}

/******************************************************************************/
/// Memory held for a peak that has passed goes back to the OS: after two
/// quiet seconds only the floor is kept.
BOOST_AUTO_TEST_CASE(retention_decays)
{
  const int64_t floor = 2 * MB;
  DecodePool pool(floor);
  std::vector<Buf> bufs;
  for (int i = 0; i < 32; ++i) bufs.push_back(alloc(pool, 1 * MB, uint8_t(i)));
  for (const auto& b : bufs) pool.Free(b.p, b.size);
  BOOST_CHECK_EQUAL(pool.bytes_cached(), 32 * MB); // the peak was 32 MB: kept

  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  Buf b = alloc(pool, 1 * MB, 1);
  pool.Free(b.p, b.size);
  BOOST_CHECK_LE(pool.bytes_cached(), floor);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), pool.bytes_cached());
}

/******************************************************************************/
/// Many threads allocating, growing, handing buffers to each other and
/// freeing them: no block is ever handed to two owners, contents survive, and
/// the accounting returns to zero.
BOOST_AUTO_TEST_CASE(concurrent_stress)
{
  DecodePool pool(8 * MB);
  constexpr int kThreads = 32;
  constexpr int kOps = 3000;
  constexpr std::size_t kHeld = 8; // per thread, so the test stays ~1 GB
  std::mutex handoff_mutex;
  std::deque<Buf> handoff; // freed by whichever thread picks it up
  std::atomic<int> corrupt{0};
  std::atomic<int> failed{0};

  auto worker = [&](int t) {
    std::mt19937_64 rng(1000 + t);
    std::uniform_int_distribution<int> op(0, 9);
    std::uniform_int_distribution<int64_t> size(32 * KB, 3 * MB);
    std::vector<Buf> mine;
    for (int i = 0; i < kOps; ++i) {
      const int o = op(rng);
      if (mine.empty() || (o < 4 && mine.size() < kHeld)) {
        Buf b{nullptr, size(rng), uint8_t(t * 31 + i)};
        if (!pool.Allocate(b.size, &b.p).ok()) {
          ++failed;
          continue;
        }
        fill(b);
        mine.push_back(b);
      } else if (o < 6) {
        Buf& b = mine[rng() % mine.size()];
        const int64_t to = size(rng);
        if (!intact(b, b.size)) ++corrupt;
        if (!pool.Reallocate(b.size, to, &b.p).ok()) {
          ++failed;
          continue;
        }
        if (!intact(b, std::min(b.size, to))) ++corrupt;
        b.size = to;
        fill(b);
      } else if (o < 8) {
        std::lock_guard<std::mutex> g(handoff_mutex);
        handoff.push_back(mine.back());
        mine.pop_back();
      } else {
        Buf b;
        {
          std::lock_guard<std::mutex> g(handoff_mutex);
          if (handoff.empty()) continue;
          b = handoff.front();
          handoff.pop_front();
        }
        if (!intact(b, b.size)) ++corrupt;
        pool.Free(b.p, b.size);
      }
    }
    for (const auto& b : mine) {
      if (!intact(b, b.size)) ++corrupt;
      pool.Free(b.p, b.size);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();
  for (const auto& b : handoff) {
    if (!intact(b, b.size)) ++corrupt;
    pool.Free(b.p, b.size);
  }

  BOOST_CHECK_EQUAL(corrupt.load(), 0);
  BOOST_CHECK_EQUAL(failed.load(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_allocated(), 0);
  BOOST_CHECK_EQUAL(pool.bytes_mapped(), pool.bytes_cached());
  BOOST_CHECK_GT(pool.max_memory(), 0);
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(process_wide_instance)
{
  BOOST_CHECK(&MzPeak::Util::decode_pool() == &MzPeak::Util::decode_pool());
}
