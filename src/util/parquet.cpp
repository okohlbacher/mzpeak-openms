/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/parquet.h"

#include <arrow/array.h>
#include <arrow/record_batch.h>
#include <arrow/util/key_value_metadata.h>
#include <boost/json.hpp>
#include <charconv>
#include <memory>
#include <mutex>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <ranges>

#include "mzpeak/exception.h"
#include "mzpeak/util/arrow.h"

namespace MzPeak::Util {

/******************************************************************************/
std::optional<std::string> get_kv_string(const Parquet::file_metadata_t& fmd,
                                         const std::string_view& key)
{
  auto result(fmd->key_value_metadata()->Get(key));

  if (result.ok()) {
    return result.ValueOrDie();
  } else {
    return {};
  }
}

/******************************************************************************/
std::optional<std::size_t> get_kv_uint(const Parquet::file_metadata_t& fmd,
                                       const std::string_view& key)
{
  return get_kv_string(fmd, key).and_then(
      [](const std::string& s) -> std::optional<std::size_t> {
        std::size_t r{};
        auto [ptr, ec]{std::from_chars(s.data(), s.data() + s.size(), r)};

        // Require a clean, FULL parse: a partial match like "0junk" must not
        // be accepted as a present 0 (that would suppress the record_count
        // statistics fallback and report a wrong count -- RDR-11).
        if (ec == std::errc() && ptr == s.data() + s.size()) {
          return r;
        } else {
          return std::nullopt;
        }
      });
}

/******************************************************************************/
struct Parquet::Impl {
  Impl(std::unique_ptr<IO::File> data, Schema::File file)
      : file_(std::move(file))
      , arrow_(std::make_unique<Arrow>(std::move(data)))
      , reader_(nullptr)
      , groups_(std::make_shared<Schema::GroupMap>())
  {
    auto raf = arrow_->reader();

    auto reader_builder = parquet::arrow::FileReaderBuilder();
    auto status = reader_builder.Open(std::move(raf));

    if (!status.ok()) {
      std::string msg("while opening file: " + file.file_name + ": ");
      throw ParquetError(msg + status.ToString());
    }

    // NOT set_use_threads(true): measured flat (8.83 vs 8.78 ms/spectrum over
    // three runs each).  Arrow parallelises across COLUMNS within a read, and a
    // point signal table has three leaves, so there is almost nothing to
    // overlap.  Worth revisiting only for a file with many auxiliary arrays.
    std::unique_ptr<parquet::arrow::FileReader> reader;
    status = reader_builder.Build(&reader);

    if (!status.ok()) {
      std::string msg("while reading file: " + file.file_name + ": ");
      throw ParquetError(msg + status.ToString());
    }

    reader_ = std::move(reader);
    stats_ = std::make_shared<StatsIndex>(reader_->parquet_reader()->metadata());
    parse_schema();
  }

  ~Impl() = default;

  void error(const std::string& error)
  {
    std::string msg("file accessing " + file_.file_name + ": " + error);
    throw ParquetError(msg);
  }

  /// Load the schema.
  void parse_schema();

  /// Decode a row group in full and return its batches, caching the result.
  std::shared_ptr<const Parquet::RowGroupBatches> row_group(int32_t index);

  Schema::File file_;
  std::unique_ptr<Arrow> arrow_;
  std::shared_ptr<parquet::arrow::FileReader> reader_;
  std::shared_ptr<Schema::GroupMap> groups_;

  /// Most-recently-decoded row groups, newest last.  Bounded because a decoded
  /// group is tens of megabytes; two is enough for a forward pass, where the
  /// only backward reach is an entity straddling a group boundary.
  static constexpr std::size_t kCachedGroups = 2;
  std::vector<std::pair<int32_t, std::shared_ptr<const Parquet::RowGroupBatches>>>
      cache_;

  /// Guards cache_ AND the decode, which shares one file position.  See
  /// row_group().
  std::mutex cache_mutex_;

  // Shared by every planner over this file; see StatsIndex.
  std::shared_ptr<StatsIndex> stats_;
};

/******************************************************************************/
void Parquet::Impl::parse_schema()
{
  auto fmd = reader_->parquet_reader()->metadata();
  auto root = fmd->schema()->group_node();
  int32_t offset = 0;

  for (int32_t i : std::views::iota(0, root->field_count())) {
    auto node = root->field(i);

    // TODO: Should we emit a warning if there is a top-level
    // primitive column?
    if (node->is_group()) {
      std::shared_ptr<parquet::schema::GroupNode> group =
          std::static_pointer_cast<parquet::schema::GroupNode>(node);

      std::shared_ptr<Schema::Group> s =
          std::make_shared<Schema::Group>(*group, i, offset);
      (*groups_)[s->name()] = s;
      offset += group->field_count();
    }
  }
}

/******************************************************************************/
std::shared_ptr<const Parquet::RowGroupBatches>
Parquet::Impl::row_group(int32_t index)
{
  // The lock is held across the DECODE as well as the bookkeeping.
  //
  // Guarding only the cache would leave the real hazard untouched: the decode
  // reads through one parquet::arrow::FileReader whose source is a single
  // seek-and-read handle (Util::ArrowFile_, and for an archive one zip_file_t
  // driven by zip_fseek).  Two threads decoding different row groups would race
  // on that file position, and Parquet pages carry no CRC by default, so the
  // result is silently wrong rather than an error.
  //
  // The cost is real but the right way round: concurrent readers serialise on
  // decode instead of corrupting each other, an uncontended lock costs nothing,
  // and a thread that waits finds the group already cached rather than decoding
  // it a second time.
  std::lock_guard<std::mutex> guard(cache_mutex_);

  for (const auto& [cached, batches] : cache_) {
    if (cached == index) return batches;
  }

  // Evict BEFORE decoding, not after.  Holding the outgoing group while the
  // incoming one is built makes the transient peak three groups where two
  // suffice -- about 21 MB for a 1,048,576-row group of three leaves -- for no
  // benefit: nothing reads the evicted entry between here and the insert below,
  // and anything still using it holds its own shared_ptr.
  if (cache_.size() >= kCachedGroups) cache_.erase(cache_.begin());

  auto reader_result = reader_->GetRecordBatchReader({index});
  if (!reader_result.ok()) {
    throw ParquetError("read row group " + std::to_string(index) + ": " +
                       reader_result.status().ToString());
  }

  auto batches = std::make_shared<Parquet::RowGroupBatches>();
  for (auto maybe_batch : **reader_result) {
    if (!maybe_batch.ok()) {
      throw ParquetError("read row group " + std::to_string(index) + ": " +
                         maybe_batch.status().ToString());
    }
    batches->push_back(*maybe_batch);
  }

  cache_.emplace_back(index, batches);
  return batches;
}

/******************************************************************************/
Parquet::Parquet(std::unique_ptr<IO::File> data, Schema::File file)
    : impl_(std::make_unique<Impl>(std::move(data), std::move(file)))
{
}

/******************************************************************************/
Parquet::~Parquet() = default;

/******************************************************************************/
const Schema::File& Parquet::index_file() const { return impl_->file_; }

/******************************************************************************/
const std::shared_ptr<Schema::GroupMap>& Parquet::groups() const
{
  return impl_->groups_;
}

/******************************************************************************/
std::optional<Schema::Column>
Parquet::field(const std::string_view& group_name,
               const std::string_view& field_name) const
{
  auto group_ptr = impl_->groups_->find(std::string{group_name});
  if (group_ptr == impl_->groups_->end()) return {};

  auto field_ptr = group_ptr->second->field(std::move(field_name));
  if (!field_ptr.has_value()) return {};

  return std::make_pair(group_ptr->second, field_ptr.value());
}

/******************************************************************************/
Parquet::file_metadata_t Parquet::file_metadata() const
{
  return impl_->reader_->parquet_reader()->metadata();
}

/******************************************************************************/
std::optional<std::string> Parquet::kv_string(const file_metadata_t& fmd,
                                              const std::string_view& key) const
{
  return get_kv_string(fmd, key);
}

/******************************************************************************/
std::optional<std::size_t> Parquet::kv_size_t(const file_metadata_t& fmd,
                                              const std::string_view& key) const
{
  return get_kv_uint(fmd, key);
}

/******************************************************************************/
parquet::arrow::FileReader& Parquet::reader() const { return *impl_->reader_; }

/******************************************************************************/
std::shared_ptr<const Parquet::RowGroupBatches> Parquet::row_group(int32_t index)
{
  return impl_->row_group(index);
}

/******************************************************************************/
Planner Parquet::planner(const Query& q)
{
  return Planner(*impl_->reader_, q, impl_->stats_);
}

/******************************************************************************/
Executor Parquet::executor(const Projection& p) { return Executor(*this, p); }

} // namespace MzPeak::Util
