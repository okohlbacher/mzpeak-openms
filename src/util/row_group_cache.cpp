/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/row_group_cache.h"

#include <exception>

namespace MzPeak::Util {

/******************************************************************************/
RowGroupCache::RowGroupCache(std::size_t budget_bytes) : budget_(budget_bytes) {}

/******************************************************************************/
RowGroupCache::Batches RowGroupCache::get(const std::string& file,
                                          int32_t group,
                                          std::size_t bytes,
                                          const Decode& decode)
{
  const Key key(file, group);
  std::promise<Batches> promise;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    // Both on arrival and again after an admission wait: another reader may
    // have claimed this very group while this one waited.
    for (;;) {
      if (auto it = entries_.find(key); it != entries_.end()) {
        it->second.used = ++tick_;
        if (it->second.ready) {
          ++stats_.hits;
        } else {
          ++stats_.waits;
        }
        std::shared_future<Batches> future = it->second.future;
        lock.unlock();
        // Outside the lock: a wait here must not stop other readers decoding
        // OTHER groups meanwhile.
        return future.get();
      }

      // Miss.  Admission control, and the reason peak memory follows the
      // budget rather than the reader count: an entry being decoded is not
      // evictable, so without this N readers pin N groups whatever the budget
      // says.  Half the budget is reserved for decodes in flight; the rest
      // stays available to HOLD decoded groups, which is what stops a fully
      // admitted cache from evicting everything it just decoded.
      //
      // The in_flight_ == 0 escape guarantees progress: a group bigger than
      // the budget is still decoded, alone.
      if (in_flight_ == 0 || in_flight_ + bytes <= budget_ / 2) break;
      ++stats_.admission_waits;
      admit_.wait(lock);
    }

    // Claim the slot before decoding, so a second reader arriving during the
    // decode finds it in flight and waits instead of decoding it again.
    ++stats_.decodes;
    entries_.emplace(key, Entry{promise.get_future().share(), bytes, ++tick_, false});
    held_ += bytes;
    in_flight_ += bytes;
    evict_locked_(key);
  }

  Batches batches;
  try {
    batches = decode();
  } catch (...) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      in_flight_ -= bytes;
      if (auto it = entries_.find(key); it != entries_.end()) {
        held_ -= it->second.bytes;
        entries_.erase(it);
      }
    }
    admit_.notify_all();
    // Waiters holding the future get the same exception.
    promise.set_exception(std::current_exception());
    throw;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    in_flight_ -= bytes;
    if (auto it = entries_.find(key); it != entries_.end()) it->second.ready = true;
    // Now evictable: the entry that could not be reclaimed while it decoded.
    evict_locked_(key);
  }
  admit_.notify_all();
  promise.set_value(batches);
  return batches;
}

/******************************************************************************/
void RowGroupCache::evict_locked_(const Key& keep)
{
  while (held_ > budget_) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (!it->second.ready || it->first == keep) continue;
      if (victim == entries_.end() || it->second.used < victim->second.used) victim = it;
    }
    if (victim == entries_.end()) return; // everything left is in use; overshoot
    held_ -= victim->second.bytes;
    entries_.erase(victim);
    ++stats_.evictions;
  }
}

/******************************************************************************/
void RowGroupCache::set_budget(std::size_t bytes)
{
  std::lock_guard<std::mutex> guard(mutex_);
  budget_ = bytes;
  evict_locked_(Key());
  admit_.notify_all(); // a raised budget may admit a waiting decode
}

std::size_t RowGroupCache::budget() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return budget_;
}

RowGroupCache::Stats RowGroupCache::stats() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  Stats s = stats_;
  s.held_bytes = held_;
  return s;
}

} // namespace MzPeak::Util
