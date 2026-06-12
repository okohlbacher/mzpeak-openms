/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE ParquetWriter
#include <boost/test/included/unit_test.hpp>

#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>
#include <cstdio>
#include <map>
#include <memory>
#include <parquet/arrow/reader.h>
#include <string>
#include <vector>

#include "mzpeak/util/parquet_writer.h"

namespace {

// Small RAII helper that removes a temporary file on scope exit.
struct TempFile {
  explicit TempFile(std::string p) : path(std::move(p)) {}
  ~TempFile() { std::remove(path.c_str()); }
  std::string path;
};

} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(round_trips_point_spectra_data)
{
  using namespace MzPeak::Util;

  // 2 spectra: index 0 with 3 points, index 1 with 2 points.
  std::vector<uint64_t> spectrum_index{0, 0, 0, 1, 1};
  std::vector<double> mz{100.0, 200.5, 300.25, 150.0, 250.75};
  std::vector<float> intensity{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

  std::map<std::string, std::string> file_kv{
      {"spectrum_count", "2"},
      {"spectrum_data_point_count", "5"},
      {"spectrum_array_index", "{\"prefix\":\"point\"}"},
  };

  TempFile tmp("parquet_writer_test.parquet");
  write_point_spectra_data(tmp.path, spectrum_index, mz, intensity, file_kv);

  // Read it back with parquet::arrow::FileReader.
  auto infile_result(arrow::io::ReadableFile::Open(tmp.path));
  BOOST_TEST(infile_result.ok());
  std::shared_ptr<arrow::io::ReadableFile> infile(infile_result.ValueOrDie());

  auto reader_result(
      parquet::arrow::OpenFile(infile, arrow::default_memory_pool()));
  BOOST_TEST(reader_result.ok());
  std::unique_ptr<parquet::arrow::FileReader> reader(
      std::move(reader_result).ValueOrDie());

  // Row count.
  auto table_result(reader->ReadTable());
  BOOST_TEST(table_result.ok());
  std::shared_ptr<arrow::Table> table(table_result.ValueOrDie());
  BOOST_TEST(table->num_rows() == 5);
  BOOST_TEST(table->num_columns() == 1);

  // Schema round-trips to the SAME types (proves store_schema works).
  auto schema(table->schema());
  auto point_field(schema->field(0));
  BOOST_TEST(point_field->name() == "point");
  BOOST_TEST((point_field->type()->id() == arrow::Type::STRUCT));

  auto struct_type(
      std::static_pointer_cast<arrow::StructType>(point_field->type()));
  BOOST_TEST(struct_type->num_fields() == 3);

  BOOST_TEST(struct_type->field(0)->name() == "spectrum_index");
  BOOST_TEST((struct_type->field(0)->type()->id() == arrow::Type::UINT64));
  BOOST_TEST(struct_type->field(1)->name() == "mz");
  BOOST_TEST((struct_type->field(1)->type()->id() == arrow::Type::DOUBLE));
  BOOST_TEST(struct_type->field(2)->name() == "intensity");
  BOOST_TEST((struct_type->field(2)->type()->id() == arrow::Type::FLOAT));

  // Struct child values match exactly.
  auto column(table->column(0));
  auto chunk(column->chunk(0));
  auto point(std::static_pointer_cast<arrow::StructArray>(chunk));

  auto idx_arr(std::static_pointer_cast<arrow::UInt64Array>(point->field(0)));
  auto mz_arr(std::static_pointer_cast<arrow::DoubleArray>(point->field(1)));
  auto int_arr(std::static_pointer_cast<arrow::FloatArray>(point->field(2)));

  BOOST_TEST(idx_arr->length() == 5);
  for (int64_t i = 0; i < idx_arr->length(); ++i) {
    BOOST_TEST(idx_arr->Value(i) == spectrum_index[i]);
    BOOST_TEST(mz_arr->Value(i) == mz[i]);
    BOOST_TEST(int_arr->Value(i) == intensity[i]);
  }

  // file_kv keys are present in the file-level key_value_metadata.
  auto fmd(reader->parquet_reader()->metadata());
  auto kv(fmd->key_value_metadata());
  BOOST_TEST((kv != nullptr));
  BOOST_TEST(kv->Contains("spectrum_count"));
  BOOST_TEST(kv->Contains("spectrum_data_point_count"));
  BOOST_TEST(kv->Contains("spectrum_array_index"));

  auto count_result(kv->Get("spectrum_count"));
  BOOST_TEST(count_result.ok());
  BOOST_TEST(count_result.ValueOrDie() == "2");
}
