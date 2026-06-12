/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <algorithm>
#include <arrow/array/array_nested.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
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

/******************************************************************************/
// Write an Arrow table to a Parquet sink with the project's standard
// properties: ZSTD, statistics, page index, store_schema, a sorting column
// on the first leaf (the entity index), a bounded row-group size, and
// optional file-level key/value metadata.  Shared by the data and metadata
// table writers so their on-disk shape stays identical (the reference reader
// selects rows via the index column's page index in both).
void write_table_to_sink(const std::shared_ptr<arrow::io::OutputStream>& sink,
                         const std::shared_ptr<arrow::Table>& table,
                         const std::map<std::string, std::string>& file_kv)
{
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

  auto writer_result(parquet::arrow::FileWriter::Open(
      *table->schema(), arrow::default_memory_pool(), sink, writer_props,
      arrow_props));
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

  // Embed file-level key/value metadata after the data, before Close().
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

/******************************************************************************/
// Read the bytes of an in-memory Parquet buffer sink into a string.
std::string finish_to_string(
    const std::shared_ptr<arrow::io::BufferOutputStream>& sink)
{
  auto buffer_result(sink->Finish());
  if (!buffer_result.ok()) {
    throw ParquetError("finish in-memory buffer sink: " +
                       buffer_result.status().ToString());
  }
  std::shared_ptr<arrow::Buffer> buffer(buffer_result.ValueOrDie());
  return std::string(reinterpret_cast<const char*>(buffer->data()),
                     static_cast<std::size_t>(buffer->size()));
}

/******************************************************************************/
// Build the point-layout spectra table and write it to the given Arrow
// output sink.  Shared by the file-path and in-memory-buffer entry points
// so the schema, writer properties and metadata handling stay identical.
void write_point_spectra_data_to_sink(
    const std::shared_ptr<arrow::io::OutputStream>& sink,
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

  write_table_to_sink(sink, table, file_kv);
}

/******************************************************************************/
// Build the spectra metadata table (single `spectrum` struct column whose
// first child is the uint64 index) and write it to the given sink.
void write_spectra_metadata_to_sink(
    const std::shared_ptr<arrow::io::OutputStream>& sink,
    const std::vector<SpectrumMetaRow>& rows)
{
  std::vector<uint64_t> index;
  std::vector<std::string> id;
  std::vector<uint8_t> ms_level;
  std::vector<uint64_t> n_points;
  std::vector<uint64_t> n_peaks;
  std::vector<std::string> representation;
  index.reserve(rows.size());
  id.reserve(rows.size());
  ms_level.reserve(rows.size());
  n_points.reserve(rows.size());
  n_peaks.reserve(rows.size());
  representation.reserve(rows.size());
  for (const auto& r : rows) {
    index.push_back(r.index);
    id.push_back(r.id);
    ms_level.push_back(r.ms_level);
    n_points.push_back(r.number_of_data_points);
    n_peaks.push_back(r.number_of_peaks);
    representation.push_back(r.representation);
  }

  // `index` MUST be the first child: the reference reader accesses it
  // positionally and selects rows via its page index.
  arrow::FieldVector spectrum_fields{
      arrow::field("index", arrow::uint64(), /*nullable=*/true),
      arrow::field("id", arrow::large_utf8(), /*nullable=*/true),
      arrow::field("MS_1000511_ms_level", arrow::uint8(), /*nullable=*/true),
      arrow::field("MS_1003060_number_of_data_points", arrow::uint64(),
                   /*nullable=*/true),
      arrow::field("MS_1003059_number_of_peaks", arrow::uint64(),
                   /*nullable=*/true),
      arrow::field("MS_1000525_spectrum_representation", arrow::utf8(),
                   /*nullable=*/true),
  };

  std::vector<std::shared_ptr<arrow::Array>> children{
      build_array<arrow::UInt64Builder>(index),
      build_array<arrow::LargeStringBuilder>(id),
      build_array<arrow::UInt8Builder>(ms_level),
      build_array<arrow::UInt64Builder>(n_points),
      build_array<arrow::UInt64Builder>(n_peaks),
      build_array<arrow::StringBuilder>(representation),
  };

  auto spectrum_result(arrow::StructArray::Make(children, spectrum_fields));
  if (!spectrum_result.ok()) {
    throw ParquetError("build spectrum struct: " +
                       spectrum_result.status().ToString());
  }
  std::shared_ptr<arrow::Array> spectrum_array(spectrum_result.ValueOrDie());

  auto schema(arrow::schema(
      {arrow::field("spectrum", spectrum_array->type(), /*nullable=*/true)}));
  auto table(arrow::Table::Make(schema, {spectrum_array}));

  write_table_to_sink(sink, table, /*file_kv=*/{});
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
  // Output sink.
  auto sink_result(arrow::io::FileOutputStream::Open(path));
  if (!sink_result.ok()) {
    throw ParquetError("open output file " + path + ": " +
                       sink_result.status().ToString());
  }
  std::shared_ptr<arrow::io::FileOutputStream> sink(sink_result.ValueOrDie());

  write_point_spectra_data_to_sink(sink, spectrum_index, mz, intensity,
                                   file_kv);
}

/******************************************************************************/
std::string point_spectra_data_bytes(
    const std::vector<uint64_t>& spectrum_index,
    const std::vector<double>& mz,
    const std::vector<float>& intensity,
    const std::map<std::string, std::string>& file_kv)
{
  // In-memory sink: the same table/schema/properties path as the file
  // writer, but the bytes are returned instead of landing on disk.
  auto sink_result(arrow::io::BufferOutputStream::Create());
  if (!sink_result.ok()) {
    throw ParquetError("create in-memory buffer sink: " +
                       sink_result.status().ToString());
  }
  std::shared_ptr<arrow::io::BufferOutputStream> sink(sink_result.ValueOrDie());

  write_point_spectra_data_to_sink(sink, spectrum_index, mz, intensity,
                                   file_kv);

  return finish_to_string(sink);
}

/******************************************************************************/
void write_spectra_metadata(const std::string& path,
                            const std::vector<SpectrumMetaRow>& rows)
{
  auto sink_result(arrow::io::FileOutputStream::Open(path));
  if (!sink_result.ok()) {
    throw ParquetError("open output file " + path + ": " +
                       sink_result.status().ToString());
  }
  std::shared_ptr<arrow::io::FileOutputStream> sink(sink_result.ValueOrDie());

  write_spectra_metadata_to_sink(sink, rows);
}

/******************************************************************************/
std::string spectra_metadata_bytes(const std::vector<SpectrumMetaRow>& rows)
{
  auto sink_result(arrow::io::BufferOutputStream::Create());
  if (!sink_result.ok()) {
    throw ParquetError("create in-memory buffer sink: " +
                       sink_result.status().ToString());
  }
  std::shared_ptr<arrow::io::BufferOutputStream> sink(sink_result.ValueOrDie());

  write_spectra_metadata_to_sink(sink, rows);

  return finish_to_string(sink);
}

} // namespace MzPeak::Util
