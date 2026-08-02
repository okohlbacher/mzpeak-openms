/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Index
#include <boost/test/included/unit_test.hpp>

#include <ranges>

#include "mzpeak/index.h"
#include "mzpeak/open.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_parse_json)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& files = index.files();
  BOOST_TEST(!files.empty(), "files should not be empty but is");
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(is_associated_with)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& files = index.files();

  const auto& spectra = std::ranges::find(files, "spectra_data.parquet",
                                          &MzPeak::Schema::File::file_name);

  BOOST_TEST((spectra != files.end()), "missing spectra_data.parquet");

  auto matches = [&](const auto& other) -> bool {
    return other != *spectra && spectra->is_associated_with(other);
  };

  for (const auto& other : files | std::views::filter(matches)) {
    BOOST_TEST_CONTEXT(spectra->file_name << " should not be associated with "
                                          << other.file_name)
    {
      BOOST_TEST((other.file_name == "spectra_metadata.parquet"));
    }
  }
}

/******************************************************************************/
// Newer writers split metadata into per-facet files and name columns plainly,
// declaring the CV term in a per-file `column_mapping` instead of encoding it
// in the column name.  Both must survive the index layer:
//   - the `scans` / `precursors` / `selected_ions` data kinds must not collapse
//     to DataKind::Other (which made the facet files invisible), and
//   - the CV -> column path/unit binding must be resolvable.
//
// The unit matters: retention time is declared UO:0000031 (minutes) and the API
// exposes seconds, so losing the unit is a 60x error.
BOOST_AUTO_TEST_CASE(parses_split_metadata_facets_and_column_mapping)
{
  auto index = MzPeak::open("../test/files/v2/small.mzpeak");

  std::size_t scans = 0, precursors = 0, selected_ions = 0;
  for (const auto& f : index.files()) {
    if (f.entity_type != MzPeak::Schema::EntityType::Spectrum) continue;
    if (f.data_kind == MzPeak::Schema::DataKind::Scans) ++scans;
    if (f.data_kind == MzPeak::Schema::DataKind::Precursors) ++precursors;
    if (f.data_kind == MzPeak::Schema::DataKind::SelectedIons) ++selected_ions;
  }
  BOOST_TEST(scans == 1u);
  BOOST_TEST(precursors == 1u);
  BOOST_TEST(selected_ions == 1u);

  // The scan facet binds MS:1000016 (scan start time) to a plain column, and
  // declares its unit as minutes.
  bool checked = false;
  for (const auto& f : index.files()) {
    if (f.data_kind != MzPeak::Schema::DataKind::Scans) continue;
    if (f.entity_type != MzPeak::Schema::EntityType::Spectrum) continue;
    auto path = f.path_for("MS:1000016");
    BOOST_TEST_REQUIRE(path.has_value());
    BOOST_TEST(*path == std::string("scan_start_time"));
    auto unit = f.unit_for("MS:1000016");
    BOOST_TEST_REQUIRE(unit.has_value());
    BOOST_TEST(*unit == std::string("UO:0000031"));
    checked = true;
  }
  BOOST_TEST(checked);
}

/******************************************************************************/
// The older single-table layout must keep parsing exactly as before: no facet
// files, and no column_mapping (those writers encode the CV term in the column
// name instead).
BOOST_AUTO_TEST_CASE(older_layout_has_no_facets_or_column_mapping)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  for (const auto& f : index.files()) {
    BOOST_TEST(f.column_mapping.empty());
    BOOST_TEST((f.data_kind != MzPeak::Schema::DataKind::Scans));
    BOOST_TEST((f.data_kind != MzPeak::Schema::DataKind::Precursors));
    BOOST_TEST((f.data_kind != MzPeak::Schema::DataKind::SelectedIons));
  }
}
