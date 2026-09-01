/*

This file is part of the package mzpeak.  It is subject to the license
in the LICENSE file found in the top-level directory of this project.

*/

#include "mzpeak/util/projection.h"
#define BOOST_TEST_MODULE Executor
#include <boost/test/included/unit_test.hpp>

#include <arrow/api.h>

#include "mzpeak/open.h"
#include "mzpeak/util/executor.h"
#include "mzpeak/util/manager.h" // IWYU pragma: keep
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/planner.h"
#include "mzpeak/util/query.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_find_spectrum)
{
  using namespace MzPeak;
  auto index = MzPeak::open("../test/files/small.mzpeak");

  auto entry = std::ranges::find(index.files(), Schema::EntityType::Spectrum,
                                 &Schema::File::entity_type);

  BOOST_TEST((entry != index.files().end()));

  auto parquet = index.manager()->parquet(*entry);

  auto index_field = parquet->field("point", "spectrum_index");
  BOOST_TEST(index_field.has_value());

  auto mz_field = parquet->field("point", "mz");
  BOOST_TEST(mz_field.has_value());

  auto query = Util::Query::Builder(index_field.value()).eq<uint64_t>(1ul);

  Util::Planner planner = parquet->planner(query);
  auto plan = planner.plan();
  BOOST_TEST(plan.ranges.size() == 1ul);

  Util::Projection projection;
  projection.project(mz_field.value());

  Util::Executor executor = parquet->executor(projection);
  const auto& slice = executor.execute(plan);
  BOOST_TEST((slice->fields() == projection.get()));
  BOOST_TEST(slice->has_column(mz_field.value()));

  // Decode, dropping null values.
  std::vector<double> mz;
  slice->array<Util::Decoders::Scalar<double>>(mz_field.value(), mz);
  BOOST_TEST(mz.size() == 15063);
  BOOST_TEST(mz[0] == 200.09, boost::test_tools::tolerance(0.001));
  BOOST_TEST(mz[mz.size() - 1] == 1999.81, boost::test_tools::tolerance(0.001));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_read_uint8_t)
{
  using namespace MzPeak;
  auto index = MzPeak::open("../test/files/small.mzpeak");

  auto entry = std::ranges::find(index.files(), "spectra_metadata.parquet",
                                 &Schema::File::file_name);

  BOOST_TEST((entry != index.files().end()));

  auto parquet = index.manager()->parquet(*entry);

  auto index_field = parquet->field("root", "index");
  BOOST_TEST(index_field.has_value());

  auto ms_level = parquet->field("root", "ms_level");
  BOOST_TEST(ms_level.has_value());

  auto query = Util::Query::Builder(index_field.value()).eq<uint64_t>(0ul);
  Util::Planner planner = parquet->planner(query);
  auto plan = planner.plan();
  BOOST_TEST(plan.ranges.size() == 1ul);

  Util::Projection projection;
  projection.project(ms_level.value());

  Util::Executor executor = parquet->executor(projection);
  const auto& slice = executor.execute(plan);

  BOOST_TEST((slice->fields() == projection.get()));
  BOOST_TEST(slice->has_column(ms_level.value()));

  std::vector<uint8_t> levels;
  slice->array<Util::Decoders::Scalar<uint8_t>>(ms_level.value(), levels);
  BOOST_TEST(levels.size() == 1);
  BOOST_TEST(levels[0] == 1);
}

/******************************************************************************/
// EqualityScan's binary-search fast path indexes `typed->raw_values()` directly
// over [0, typed->length()), and the array it receives is a SLICE of the row
// group's batch (Executor::execute calls batch->Slice before filter()).  That
// is only correct because Arrow's NumericArray::raw_values() returns a pointer
// that ALREADY has the slice offset applied:
//
//     values_ = raw_values_ + data_->offset;
//
// which is also the pointer Value(i) indexes, so the binary and linear paths
// agree element for element.
//
// Pinned here because it is an assumption about a THIRD-PARTY library that is
// invisible at our call site, and because two independent adversarial review
// runs disagreed about it -- one asserting raw_values() IGNORES the offset,
// which would mean every sliced fast-path read returned peaks from the wrong
// rows.  It does not, on Arrow 25.  If a future Arrow changes this, the failure
// surfaces loudly here instead of as silently wrong m/z values.
BOOST_AUTO_TEST_CASE(arrow_raw_values_is_offset_adjusted_for_a_slice)
{
  arrow::DoubleBuilder builder;
  for (int i = 1; i <= 8; ++i)
    BOOST_TEST_REQUIRE(builder.Append(double(i)).ok());

  std::shared_ptr<arrow::Array> array;
  BOOST_TEST_REQUIRE(builder.Finish(&array).ok());

  // Rows 4..7, i.e. the values 5, 6, 7, 8.
  auto typed = std::static_pointer_cast<arrow::DoubleArray>(array->Slice(4, 4));

  BOOST_TEST_REQUIRE(typed->offset() == 4);
  BOOST_TEST_REQUIRE(typed->length() == 4);

  // The first element of the SLICE, not of the underlying buffer.
  BOOST_TEST(typed->raw_values()[0] == 5.0);
  BOOST_TEST(typed->raw_values()[typed->length() - 1] == 8.0);

  // The two accessors the two scan paths use must agree element for element.
  for (int64_t i = 0; i < typed->length(); ++i) {
    BOOST_TEST(typed->raw_values()[i] == typed->Value(i));
  }
}
