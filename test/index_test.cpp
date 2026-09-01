/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Index
#include <boost/test/included/unit_test.hpp>

#include <ranges>

#include "mzpeak/exception.h"
#include "mzpeak/index.h"
#include "mzpeak/open.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_parse_json)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& files = index.files();
  BOOST_TEST(!files.empty(), "files should not be empty");
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
    if (f.entity_type() != MzPeak::Schema::EntityType::Spectrum) continue;
    if (f.data_kind().type() == MzPeak::Schema::DataKind::Scans) ++scans;
    if (f.data_kind().type() == MzPeak::Schema::DataKind::Precursors) ++precursors;
    if (f.data_kind().type() == MzPeak::Schema::DataKind::SelectedIons)
      ++selected_ions;
  }
  BOOST_TEST(scans == 1u);
  BOOST_TEST(precursors == 1u);
  BOOST_TEST(selected_ions == 1u);

  // The scan facet binds MS:1000016 (scan start time) to a plain column, and
  // declares its unit as minutes.
  bool checked = false;
  for (const auto& f : index.files()) {
    if (f.data_kind().type() != MzPeak::Schema::DataKind::Scans) continue;
    if (f.entity_type() != MzPeak::Schema::EntityType::Spectrum) continue;
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
  auto index = MzPeak::open("../test/files/legacy/small.mzpeak");
  for (const auto& f : index.files()) {
    BOOST_TEST(f.columns().empty());
    BOOST_TEST((f.data_kind().type() != MzPeak::Schema::DataKind::Scans));
    BOOST_TEST((f.data_kind().type() != MzPeak::Schema::DataKind::Precursors));
    BOOST_TEST((f.data_kind().type() != MzPeak::Schema::DataKind::SelectedIons));
  }
}

/******************************************************************************/
// The split-metadata layout must produce the SAME metadata as the older nested
// layout.  test/files/legacy/small.mzpeak is the ORIGINAL nested-layout
// archive, preserved when upstream regenerated every bundled fixture into the
// split layout; every field below is cross-checked against it rather than
// against hardcoded numbers — that catches a rename
// silently reading back as nullopt, which is how this layout fails.
BOOST_AUTO_TEST_CASE(split_metadata_layout_matches_the_nested_layout)
{
  auto v1 = MzPeak::open("../test/files/legacy/small.mzpeak").spectra();
  auto v2 = MzPeak::open("../test/files/v2/small.mzpeak").spectra();
  BOOST_TEST_REQUIRE(v1.size() == v2.size());

  std::size_t compared = 0, with_precursor = 0;
  for (std::size_t i = 0; i < v1.size(); ++i) {
    auto a = v1[i];
    auto b = v2[i];
    const auto& ma = a.metadata();
    const auto& mb = b.metadata();

    BOOST_TEST(ma.index == mb.index);
    BOOST_TEST((ma.ms_level == mb.ms_level));
    BOOST_TEST(ma.representation == mb.representation);
    BOOST_TEST((ma.number_of_data_points == mb.number_of_data_points));
    BOOST_TEST((ma.number_of_peaks == mb.number_of_peaks));

    // Retention time in SECONDS: the newer layout declares the unit
    // (UO:0000031, minutes) in column_mapping instead of the column name.
    BOOST_TEST_REQUIRE(ma.retention_time.has_value() ==
                       mb.retention_time.has_value());
    if (ma.retention_time)
      BOOST_TEST(std::abs(*ma.retention_time - *mb.retention_time) < 1e-6);

    // Precursors: exercises the isolation_window_target and peak_intensity
    // renames, which a plain de-prefixing port would miss.
    BOOST_TEST_REQUIRE(ma.precursors.size() == mb.precursors.size());
    if (!ma.precursors.empty()) {
      ++with_precursor;
      const auto& pa = ma.precursors[0];
      const auto& pb = mb.precursors[0];
      BOOST_TEST((pa.isolation_window.target_mz == pb.isolation_window.target_mz));
      BOOST_TEST(
          (pa.isolation_window.lower_offset == pb.isolation_window.lower_offset));
      BOOST_TEST(
          (pa.isolation_window.upper_offset == pb.isolation_window.upper_offset));
      BOOST_TEST_REQUIRE(pa.selected_ions.size() == pb.selected_ions.size());
      if (!pa.selected_ions.empty()) {
        BOOST_TEST((pa.selected_ions[0].selected_ion_mz ==
                    pb.selected_ions[0].selected_ion_mz));
        BOOST_TEST((pa.selected_ions[0].intensity == pb.selected_ions[0].intensity));
      }
    }

    // Scan windows come from the separate scans file in the newer layout.
    BOOST_TEST_REQUIRE(ma.scan_windows.size() == mb.scan_windows.size());
    if (!ma.scan_windows.empty()) {
      BOOST_TEST((ma.scan_windows[0].lower_limit == mb.scan_windows[0].lower_limit));
      BOOST_TEST((ma.scan_windows[0].upper_limit == mb.scan_windows[0].upper_limit));
    }
    ++compared;
  }
  BOOST_TEST(compared == 48u);
  // 34 MS2 carry a precursor; if the facet join silently produced nothing this
  // would be 0 while every per-spectrum check above still passed.
  BOOST_TEST(with_precursor == 34u);
}

/******************************************************************************/
// Peak decode must work on the split layout too: the profile/centroid dispatch
// used to re-query the metadata file through a `spectrum` struct group, which
// the flat layout does not have.
BOOST_AUTO_TEST_CASE(split_metadata_layout_decodes_peaks)
{
  auto v1 = MzPeak::open("../test/files/legacy/small.mzpeak").spectra();
  auto v2 = MzPeak::open("../test/files/v2/small.mzpeak").spectra();

  for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
    auto a = v1[i];
    auto b = v2[i];
    const auto& mza = a.mz();
    const auto& mzb = b.mz();
    BOOST_TEST_REQUIRE(mza.size() == mzb.size());
    for (std::size_t k = 0; k < mza.size(); ++k) {
      if (std::abs(mza[k] - mzb[k]) > 1e-9) {
        BOOST_TEST(mza[k] == mzb[k]); // report the first offender
        break;
      }
    }
  }
}
