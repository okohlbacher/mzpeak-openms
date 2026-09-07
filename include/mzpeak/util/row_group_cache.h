/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace MzPeak::Util {

/// A decoded row group: the record batches Parquet hands back for it, plus
/// what a reader needs to find a row in them WITHOUT touching Arrow.
///
/// The extra vectors are filled once, by the thread that decoded the group,
/// and then only read.  That matters twice over: Arrow's accessors
/// (RecordBatch::column, StructArray::field) are not free even when the child
/// is already boxed -- libstdc++ implements the atomic shared_ptr load they
/// use with a pool of 16 process-wide mutexes -- and a reader that can pick
/// its batch arithmetically never calls them for the batches it skips.
struct RowGroupBatches {
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;

  /// First row index of each batch, relative to the row group.
  std::vector<int64_t> row_offset;

  /// First and last value of the column this file declares sorted, per batch.
  /// Empty unless the group declares exactly one sorted column and it decodes
  /// to int64 -- the only case the reader's fast path uses.
  std::vector<int64_t> key_first;
  std::vector<int64_t> key_last;

  bool has_keys() const { return !key_first.empty(); }
};

/******************************************************************************/
/// One decoded copy of each row group, shared by every reader over an archive.
///
/// A reader per thread is the library's concurrency model, and each reader
/// used to keep its own two decoded groups.  With sixteen readers over a
/// seven-group file that is up to thirty-two decoded copies of seven groups:
/// a gigabyte of duplicates.  This cache lives on the archive's Manager, so
/// the readers share it, and a group is decoded ONCE however many readers
/// want it.  The second reader that asks while the first is still decoding
/// waits for that decode rather than starting its own.
///
/// Decoding itself is not serialised here: every Parquet object has its own
/// file handle, so two readers decoding two different groups run in parallel.
/// The lock covers only the bookkeeping.
///
/// Bounded by BYTES in two places, because eviction alone cannot hold a
/// budget: a group still being decoded is not evictable, so N readers used to
/// pin N groups however small the budget was, and memory followed the READER
/// COUNT rather than the budget.
///
/// So a decode is also ADMITTED: a reader that misses waits until the bytes
/// already in flight leave room for its group (half the budget is reserved
/// for them, the rest stays available to hold decoded groups).  A single
/// reader is always admitted, so a group larger than the budget still makes
/// progress.  Peak decoded memory therefore follows THE BUDGET, whatever the
/// thread count -- which is what lets a caller run every core on tagging
/// while reading stays bounded.
///
/// A budget smaller than the working set thrashes (a group is decoded again
/// each time a reader comes back to it) but stays correct.
class RowGroupCache final {
public:
  using Batches = std::shared_ptr<const RowGroupBatches>;
  using Decode = std::function<Batches()>;

  /// 1 GiB: room for ~30 groups of a typical archive, two of a chunked Astral
  /// one.  Consumers that know their thread count set their own.
  static constexpr std::size_t kDefaultBudget = std::size_t(1) << 30;

  explicit RowGroupCache(std::size_t budget_bytes = kDefaultBudget);

  /// The decoded group @p group of @p file: cached, in flight (wait for it),
  /// or decoded now through @p decode.  @p bytes is the group's size for the
  /// budget; Parquet's uncompressed row-group size is the right estimate.
  /// Rethrows whatever @p decode threw, for waiters too, and forgets the
  /// entry so a later call tries again.
  Batches get(const std::string& file,
              int32_t group,
              std::size_t bytes,
              const Decode& decode);

  void set_budget(std::size_t bytes);
  std::size_t budget() const;

  struct Stats {
    std::size_t decodes = 0;   ///< groups decoded (each miss, once)
    std::size_t hits = 0;      ///< served from a decoded group
    std::size_t waits = 0;     ///< served by waiting for another reader's decode
    std::size_t admission_waits = 0; ///< decodes held back to stay in budget
    std::size_t evictions = 0;
    std::size_t held_bytes = 0; ///< budget currently accounted for
  };
  Stats stats() const;

private:
  using Key = std::pair<std::string, int32_t>;
  struct Entry {
    std::shared_future<Batches> future;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
    bool ready = false;
  };

  /// Evict least-recently-used READY entries, never @p keep, until the budget
  /// holds or nothing evictable is left.  Caller holds mutex_.
  void evict_locked_(const Key& keep);

  mutable std::mutex mutex_;
  std::condition_variable admit_;
  std::map<Key, Entry> entries_;
  std::size_t budget_;
  std::size_t held_ = 0;
  std::size_t in_flight_ = 0; ///< bytes of entries still decoding
  std::uint64_t tick_ = 0;
  Stats stats_;
};

} // namespace MzPeak::Util
