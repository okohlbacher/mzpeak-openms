/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <arrow/record_batch.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

#include "mzpeak/io/file.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/schema/group.h"
#include "mzpeak/util/executor.h"
#include "mzpeak/util/query.h"

namespace MzPeak::Util {

/**
 * Low-level wrapper around Parquet files.
 */
class Parquet final {
public:
  using file_metadata_t = std::shared_ptr<parquet::FileMetaData>;

  /// Constructor.
  Parquet(std::unique_ptr<MzPeak::IO::File>, Schema::File);

  /// Destructor.
  ~Parquet();

  /**
   * Return the file information from the MzPeak index.
   */
  const Schema::File& index_file() const;

  /**
   * Return the schema encoded as a map of Group objects.
   */
  const std::shared_ptr<Schema::GroupMap>& groups() const;

  /**
   * Return a Group and Field matching the given names.
   */
  std::optional<Schema::Column> field(const std::string_view&,
                                      const std::string_view&) const;

  /**
   * Access the file metadata.
   */
  file_metadata_t file_metadata() const;

  /**
   * Fetch a string value from the metadata key-value store.
   */
  std::optional<std::string> kv_string(const file_metadata_t&,
                                       const std::string_view&) const;

  /**
   * Fetch a std::size_t value from the metadata key-value store.
   */
  std::optional<std::size_t> kv_size_t(const file_metadata_t&,
                                       const std::string_view&) const;

  /**
   * Directly access the FileReader.  This reference is only valid
   * while this Parquet object exists.
   */
  parquet::arrow::FileReader& reader() const;

  /**
   * The record batches of one row group, decoded once and retained.
   *
   * A query that selects a single entity decodes the row group holding it and
   * slices that down to a few thousand rows.  Reading a run entity by entity
   * therefore decoded the same group hundreds of times -- on a 21-row-group
   * file, 13,009 decodes where 21 would do.  This hands back the same decoded
   * batches to every caller that lands in the group.
   *
   * Only the most recent few groups are kept: a decoded group is tens of
   * megabytes, and a forward pass never looks back.
   *
   * Returned by shared_ptr, not by reference: the cache evicts, and a reference
   * into it would dangle the moment a caller fetched a second group while still
   * holding the first.
   *
   * @note An arrow::Array sliced out of these batches SHARES their buffers, so
   * anything retaining such a slice keeps the whole decoded row group alive.
   * Spectrum copies its values out and drops the slice; Chromatogram keeps its
   * decoder, and so keeps a group resident for as long as the caller holds it.
   * That is bounded by how many entities the caller holds, not by the run.
   */
  using RowGroupBatches = std::vector<std::shared_ptr<arrow::RecordBatch>>;
  std::shared_ptr<const RowGroupBatches> row_group(int32_t);

  /**
   * `true` when @p row_group declares @p leaf_column sorted ascending with
   * nulls last.  See StatsIndex::sorted_ascending.
   */
  bool sorted_ascending(int32_t row_group, int32_t leaf_column) const;

  /**
   * Return a planner for the given query.
   */
  Planner planner(const Query&);

  /**
   * Return an executor that will capture the requested fields.
   */
  Executor executor(const Projection&);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace MzPeak::Util
