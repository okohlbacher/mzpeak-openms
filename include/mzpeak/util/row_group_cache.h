/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
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

/// A decoded row group: the record batches Parquet hands back for it.
using RowGroupBatches = std::vector<std::shared_ptr<arrow::RecordBatch>>;

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
/// Bounded by BYTES, evicting the least recently used group that is not
/// currently being decoded.  Memory therefore follows the number of distinct
/// groups in flight -- at most one per reader plus a boundary -- rather than
/// the number of readers.  A caller with many readers over huge groups should
/// size its reader count so that in-flight groups fit the budget; a budget
/// smaller than that thrashes (a group is decoded again each time a reader
/// comes back to it) but stays correct.
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
  std::map<Key, Entry> entries_;
  std::size_t budget_;
  std::size_t held_ = 0;
  std::uint64_t tick_ = 0;
  Stats stats_;
};

} // namespace MzPeak::Util
