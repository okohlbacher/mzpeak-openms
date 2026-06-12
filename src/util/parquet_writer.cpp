/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <algorithm>
#include <arrow/array/array_nested.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>
#include <memory>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/types.h>

#include "mzpeak/exception.h"
#include "mzpeak/util/parquet_writer.h"

namespace MzPeak::Util {

namespace {

/******************************************************************************/
// Throw a ParquetError if the given status is not OK.
void check(const arrow::Status& status, const std::string& what)
{
  if (!status.ok()) throw ParquetError(what + ": " + status.ToString());
}

/******************************************************************************/
// Build a typed primitive Arrow array from a vector of values.
template <typename Builder, typename T>
std::shared_ptr<arrow::Array> build_array(const std::vector<T>& values)
{
  Builder builder;
  check(builder.Reserve(static_cast<int64_t>(values.size())), "reserve array");
  check(builder.AppendValues(values), "append values");

  std::shared_ptr<arrow::Array> array;
  check(builder.Finish(&array), "finish array");
  return array;
}

} // namespace

/******************************************************************************/
void write_point_spectra_data(
    const std::string& path,
    const std::vector<uint64_t>& spectrum_index,
    const std::vector<double>& mz,
    const std::vector<float>& intensity,
    const std::map<std::string, std::string>& file_kv)
{
  if (spectrum_index.size() != mz.size() || mz.size() != intensity.size()) {
    throw ParquetError("write_point_spectra_data: input vectors must have "
                       "the same length");
  }

  // Leaf arrays for the three children of the `point` struct.
  auto index_array(build_array<arrow::UInt64Builder>(spectrum_index));
  auto mz_array(build_array<arrow::DoubleBuilder>(mz));
  auto intensity_array(build_array<arrow::FloatBuilder>(intensity));

  // Struct fields.  These mirror the reference file: each leaf is a
  // nullable field of the matching type.
  arrow::FieldVector point_fields{
      arrow::field("spectrum_index", arrow::uint64(), /*nullable=*/true),
      arrow::field("mz", arrow::float64(), /*nullable=*/true),
      arrow::field("intensity", arrow::float32(), /*nullable=*/true),
  };

  auto point_result(arrow::StructArray::Make(
      {index_array, mz_array, intensity_array}, point_fields));
  if (!point_result.ok()) {
    throw ParquetError("build point struct: " +
                       point_result.status().ToString());
  }
  std::shared_ptr<arrow::Array> point_array(point_result.ValueOrDie());

  // Top-level schema: a single struct column named `point`.
  auto schema(arrow::schema(
      {arrow::field("point", point_array->type(), /*nullable=*/true)}));

  auto table(arrow::Table::Make(schema, {point_array}));

  // Parquet writer properties: ZSTD, statistics, page index and a
  // sorting column on the `point.spectrum_index` leaf (leaf index 0).
  parquet::WriterProperties::Builder props_builder;
  props_builder.compression(arrow::Compression::ZSTD);
  props_builder.enable_statistics();
  props_builder.enable_write_page_index();
  props_builder.set_sorting_columns({parquet::SortingColumn{
      /*column_idx=*/0, /*descending=*/false, /*nulls_first=*/false}});
  auto writer_props(props_builder.build());

  // Store the Arrow schema so the struct/types round-trip exactly.
  auto arrow_props(
      parquet::ArrowWriterProperties::Builder().store_schema()->build());

  // Output sink.
  auto sink_result(arrow::io::FileOutputStream::Open(path));
  if (!sink_result.ok()) {
    throw ParquetError("open output file " + path + ": " +
                       sink_result.status().ToString());
  }
  std::shared_ptr<arrow::io::FileOutputStream> sink(sink_result.ValueOrDie());

  auto writer_result(parquet::arrow::FileWriter::Open(
      *schema, arrow::default_memory_pool(), sink, writer_props, arrow_props));
  if (!writer_result.ok()) {
    throw ParquetError("open parquet writer: " +
                       writer_result.status().ToString());
  }
  std::unique_ptr<parquet::arrow::FileWriter> writer(
      std::move(writer_result).ValueOrDie());

  // Bound the row-group size: one giant row group defeats page/row-group
  // pruning and is memory-hungry for large files.  Keep it positive even
  // for an empty table (WriteTable rejects a zero chunk size).
  constexpr int64_t kMaxRowGroup = 1 << 20; // ~1M rows
  int64_t row_group_size =
      table->num_rows() > 0 ? std::min<int64_t>(table->num_rows(), kMaxRowGroup)
                            : kMaxRowGroup;
  check(writer->WriteTable(*table, row_group_size), "write table");

  // Embed the file-level key/value metadata.  This must be done after
  // writing the data but before Close().
  if (!file_kv.empty()) {
    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(file_kv.size());
    values.reserve(file_kv.size());
    for (const auto& [key, value] : file_kv) {
      keys.push_back(key);
      values.push_back(value);
    }
    auto kv(std::make_shared<arrow::KeyValueMetadata>(keys, values));
    check(writer->AddKeyValueMetadata(kv), "add key/value metadata");
  }

  check(writer->Close(), "close parquet writer");
}

} // namespace MzPeak::Util
