/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// RDR-4a: the entity-index column (spectrum_index / chromatogram_index / ...)
// is stored UNSIGNED 64-bit (Parquet physical INT64 + logical
// Int(64, isSigned=false), read back as arrow::UInt64Array).  The reader used
// to model it as signed Int64 and reinterpret the bits, which is bit-identical
// only for indices < 2^63.  This test builds a single-row-group Parquet column
// whose index value is ABOVE INT64_MAX (0xFFFFFFFFFFFFFF00) and asserts the
// unsigned path is correct end to end at the helper/Query level:
//
//   (a) parquet_array_cast<UInt64> reads the value back exactly.
//   (b) A Query built with Builder::eq<uint64_t>(kBigIndex) keeps the row
//       group (stats-range path must treat the value as > INT64_MAX, not as a
//       negative signed int).
//   (c) A query for kBigIndex+1 (above the row-group max) prunes the group.

#define BOOST_TEST_MODULE UnsignedIndex
#include <boost/test/included/unit_test.hpp>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/statistics.h>

#include "mzpeak/directory.h"
#include "mzpeak/query.h"
#include "mzpeak/schema/array_index.h"
#include "mzpeak/schema/entity_type.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/parquet_types.h"
#include "mzpeak/util/parquet_writer.h"

using namespace MzPeak;
using DataType = Schema::PSI::DataType;

namespace {

// A genuine unsigned index well above INT64_MAX.  As a *signed* int64 the same
// bit pattern is -256, which is the wrong-answer trap this whole change closes.
constexpr uint64_t kBigIndex = 0xFFFFFFFFFFFFFF00ull;
static_assert(kBigIndex > static_cast<uint64_t>(INT64_MAX),
              "test value must exceed INT64_MAX to exercise the unsigned path");

// Small RAII helper that removes a temporary file on scope exit.
struct TempFile {
  explicit TempFile(std::string p)
      : path(std::move(p))
  {
  }
  ~TempFile() { std::remove(path.c_str()); }
  std::string path;
};

// Write a one-column (uint64) Parquet table in memory, with statistics
// enabled, and return the buffer.  Used only for the array-cast test (a).
std::shared_ptr<arrow::Buffer> write_uint64_column(const std::vector<uint64_t>& vals)
{
  arrow::UInt64Builder builder;
  BOOST_REQUIRE(builder.AppendValues(vals).ok());
  std::shared_ptr<arrow::Array> array;
  BOOST_REQUIRE(builder.Finish(&array).ok());

  auto schema(arrow::schema(
      {arrow::field("spectrum_index", arrow::uint64(), /*nullable=*/true)}));
  auto table(arrow::Table::Make(schema, {array}));

  auto sink_res(arrow::io::BufferOutputStream::Create());
  BOOST_REQUIRE(sink_res.ok());
  auto sink(sink_res.ValueOrDie());

  parquet::WriterProperties::Builder props;
  props.enable_statistics();
  auto writer_props(props.build());
  auto arrow_props(
      parquet::ArrowWriterProperties::Builder().store_schema()->build());

  BOOST_REQUIRE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                           sink, /*chunk_size=*/1 << 20,
                                           writer_props, arrow_props)
                    .ok());

  return sink->Finish().ValueOrDie();
}

// Write a point-spectra Parquet file with the given spectrum_index values and
// return a Util::Parquet reader for it.  The KV metadata includes the required
// spectrum_array_index JSON so Parquet::field() can locate the index column.
Util::Parquet make_parquet(const std::string& file_name,
                           const std::vector<uint64_t>& spectrum_index)
{
  std::vector<double> mz(spectrum_index.size(), 100.0);
  std::vector<float> intensity(spectrum_index.size(), 1.0f);

  // Minimal array_index JSON — no entries so the reader builds the synthetic
  // spectrum_index column from the struct's first child (positionally).
  const std::string kAI{"{\"prefix\":\"point\",\"entries\":[]}"};
  std::map<std::string, std::string> kv{
      {"spectrum_array_index", kAI},
  };

  Util::write_point_spectra_data(file_name, spectrum_index, mz, intensity, kv);

  Directory dir(".");
  std::unique_ptr<File> data(dir.read_file(file_name));

  Schema::File sf(file_name);
  sf.entity_type = Schema::EntityType::Spectrum;

  return Util::Parquet(std::move(data), sf);
}

} // namespace

/******************************************************************************/
// (a) parquet_array_cast<UInt64> reads a > INT64_MAX value back exactly.
BOOST_AUTO_TEST_CASE(array_cast_reads_value_above_int64_max)
{
  auto buffer(write_uint64_column({0, kBigIndex, 42}));

  auto infile(std::make_shared<arrow::io::BufferReader>(buffer));
  std::unique_ptr<parquet::arrow::FileReader> reader;
  parquet::arrow::FileReaderBuilder builder;
  BOOST_REQUIRE(builder.Open(infile).ok());
  BOOST_REQUIRE(builder.Build(&reader).ok());

  auto table_res(reader->ReadTable());
  BOOST_REQUIRE(table_res.ok());
  std::shared_ptr<arrow::Table> table(table_res.ValueOrDie());

  // The column round-trips as a uint64 array.
  std::shared_ptr<arrow::Array> col(table->column(0)->chunk(0));
  BOOST_TEST((col->type_id() == arrow::Type::UINT64));

  auto typed(Util::parquet_array_cast<DataType::UInt64>(col));
  BOOST_TEST(typed->Value(0) == static_cast<uint64_t>(0));
  BOOST_TEST(typed->Value(1) == kBigIndex);
  BOOST_TEST(typed->Value(2) == static_cast<uint64_t>(42));

  // A signed reinterpretation of the same bits is the wrong answer (-256).
  BOOST_TEST(std::bit_cast<int64_t>(typed->Value(1)) == static_cast<int64_t>(-256));
}

/******************************************************************************/
// (b) + (c): the stats-range path keeps the row group for kBigIndex and prunes
// it for kBigIndex+1, with unsigned semantics end-to-end.
BOOST_AUTO_TEST_CASE(stats_range_keeps_group_for_value_above_int64_max)
{
  TempFile tmp("unsigned_index_test.parquet");

  // Row group spans indices [0, kBigIndex]: the unsigned max is kBigIndex.
  // As a *signed* int64 the max bits read as -256 — the trap RDR-4a closes.
  auto parquet(make_parquet(tmp.path, {0, kBigIndex}));

  auto dest = parquet.field("point", "spectrum_index");
  BOOST_REQUIRE(dest.has_value());

  // (b) EQ kBigIndex: the row group [0, kBigIndex] must NOT be pruned.
  auto eq = Query::Builder(*dest).eq<uint64_t>(kBigIndex);
  auto keep = parquet.find_row_groups(eq);
  BOOST_TEST(keep.size() == 1ul);

  // (c) EQ kBigIndex+1 (strictly above the group's unsigned max): must prune.
  auto eq_above = Query::Builder(*dest).eq<uint64_t>(kBigIndex + 1);
  auto prune = parquet.find_row_groups(eq_above);
  BOOST_TEST(prune.empty());
}
