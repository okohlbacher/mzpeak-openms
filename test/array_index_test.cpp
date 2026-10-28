/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE ArrayIndex
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/data/signals.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/util/manager.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_get_array_index)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");

  auto entry = mzpeak.find_file(Schema::EntityType::Type::Spectrum,
                                Schema::DataKind::DataArray);

  BOOST_TEST((entry != mzpeak.files().end()));

  auto parquet = mzpeak.manager()->parquet(*entry);
  Data::Signals data(std::move(parquet));
  std::shared_ptr<Data::ArrayIndex> index(data.array_index());

  BOOST_TEST(index->prefix() == "point");
  BOOST_TEST(index->entries().size() == 2ul);

  const auto mz_it = std::ranges::find(index->entries(), "m/z array",
                                       &Data::ArrayIndex::Entry::array_name);
  BOOST_TEST((mz_it != index->entries().end()));

  const std::optional<Schema::PSI::Transform> interpolation(
      Schema::PSI::Transform::ZeroIntensityInterpolation);
  const Schema::PSI::DataType f64(Schema::PSI::DataType::Float64);

  BOOST_TEST((mz_it->buffer_format == Schema::BufferFormat::Point));
  BOOST_TEST((mz_it->context.type() == Schema::EntityType::Spectrum));
  BOOST_TEST((mz_it->path == "point.mz"));
  BOOST_TEST((mz_it->data_type == f64));
  BOOST_TEST((mz_it->array_type == Schema::PSI::ArrayType::Mz));
  BOOST_TEST((mz_it->unit == "MS:1000040"));
  BOOST_TEST((mz_it->buffer_priority));
  BOOST_TEST((mz_it->sorting_rank == std::optional{0}));
  BOOST_TEST((mz_it->data_processing_id == std::nullopt));
  BOOST_TEST((mz_it->transform == interpolation));

  const auto intensity_it = std::ranges::find(index->entries(), "intensity array",
                                              &Data::ArrayIndex::Entry::array_name);
  BOOST_TEST((intensity_it != index->entries().end()));

  const std::optional<Schema::PSI::Transform> trim(
      Schema::PSI::Transform::ZeroIntensityTrim);
  BOOST_TEST((intensity_it->transform == trim));

  std::size_t count = index->num_entities().value_or(0);
  BOOST_TEST(count == 48ul);
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_read_mz_array)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");

  auto entry = mzpeak.find_file(Schema::EntityType::Type::Spectrum,
                                Schema::DataKind::DataArray);

  BOOST_TEST((entry != mzpeak.files().end()));

  auto parquet = mzpeak.manager()->parquet(*entry);

  Data::Signals data(std::move(parquet));

  auto index_field = data.column("spectrum_index");
  auto mz_column = data.column("mz");

  BOOST_TEST(index_field.has_value());
  BOOST_TEST(mz_column.has_value());
}

/******************************************************************************/
// Writers since the reference implementation's "add index buffer format" list
// the key and time columns in the array index with buffer_format `index` and a
// null unit.  small.index.dir is the reference's small.unpacked.mzpeak at that
// change; it must decode exactly as small.dir, written before it.
BOOST_AUTO_TEST_CASE(index_buffer_entries_are_not_signal_arrays)
{
  using namespace MzPeak;

  auto with = MzPeak::open("../test/files/small.index.dir").spectra();
  auto without = MzPeak::open("../test/files/small.dir").spectra();
  BOOST_REQUIRE_EQUAL(with.size(), without.size());
  for (std::size_t i = 0; i < with.size(); ++i) {
    auto a = with[i];
    auto b = without[i];
    BOOST_TEST(a.mz() == b.mz(), "m/z of spectrum " << i);
    BOOST_TEST(a.intensity() == b.intensity(), "intensity of spectrum " << i);
    BOOST_TEST((a.retention_time() == b.retention_time()), "RT of spectrum " << i);
  }
}
