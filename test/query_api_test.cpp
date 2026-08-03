/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * Selection / batch reading API: by-native-id lookup, retention-time range
 * selection, extracted-ion chromatograms and batch reads.
 *
 * Ground truth for small.mzpeak comes from the metadata table read with
 * pyarrow: 48 spectra (14 MS1, 34 MS2), ids of the form
 * "controllerType=0 controllerNumber=1 scan=N", and retention times spanning
 * 0.2961 s .. 29.2342 s (the file stores minutes; the reader exposes seconds).
 */
#define BOOST_TEST_MODULE Query
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"

namespace {
constexpr const char* kFile = "../test/files/small.mzpeak";
} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(resolves_native_id_to_index)
{
  auto spectra = MzPeak::open(kFile).spectra();

  auto first = spectra.index_for_id("controllerType=0 controllerNumber=1 scan=1");
  BOOST_TEST_REQUIRE(first.has_value());
  BOOST_TEST(*first == 0u);

  auto third = spectra.index_for_id("controllerType=0 controllerNumber=1 scan=3");
  BOOST_TEST_REQUIRE(third.has_value());
  BOOST_TEST(*third == 2u);

  // An unknown id must report absence rather than guessing a neighbour.
  BOOST_TEST(!spectra.index_for_id("no such spectrum").has_value());
}

/******************************************************************************/
// by_id must return the SAME spectrum as positional access, and must throw
// (not return an empty spectrum) for an unknown id — silently handing back an
// empty spectrum would look like a real but featureless scan.
BOOST_AUTO_TEST_CASE(by_id_matches_positional_access_and_throws_when_unknown)
{
  auto spectra = MzPeak::open(kFile).spectra();

  auto by_id = spectra.by_id("controllerType=0 controllerNumber=1 scan=1");
  auto positional = spectra[0];
  BOOST_TEST(by_id.metadata().index == positional.metadata().index);
  BOOST_TEST(by_id.mz().size() == positional.mz().size());
  BOOST_TEST_REQUIRE(!by_id.mz().empty());
  BOOST_TEST(by_id.mz().front() == positional.mz().front());

  BOOST_CHECK_THROW(spectra.by_id("no such spectrum"), MzPeak::ParquetError);
}

/******************************************************************************/
// Retention-time selection is in SECONDS and inclusive at both ends.
// Ground truth: the first five spectra sit at 0.2961, 0.4738, 0.6731, 1.3703
// and 2.0955 s, so [0.5, 2.0] selects exactly two (0.6731 and 1.3703).
BOOST_AUTO_TEST_CASE(time_range_is_seconds_inclusive_and_time_ordered)
{
  auto spectra = MzPeak::open(kFile).spectra();

  auto selected = spectra.indices_in_time_range(0.5, 2.0);
  BOOST_TEST(selected.size() == 2u);

  // Results must be in ascending time order.
  double previous = -1.0;
  for (std::size_t index : selected) {
    auto s = spectra[index];
    BOOST_TEST_REQUIRE(s.retention_time().has_value());
    const double t = *s.retention_time();
    BOOST_TEST(t >= 0.5);
    BOOST_TEST(t <= 2.0);
    BOOST_TEST(t >= previous);
    previous = t;
  }

  // The whole run: 0.2961 .. 29.2342 s, so a covering window selects all 48.
  BOOST_TEST(spectra.indices_in_time_range(0.0, 100.0).size() == 48u);

  // Empty window selects nothing; reversed bounds are normalised, not treated
  // as empty (a silently empty result is the dangerous outcome here).
  BOOST_TEST(spectra.indices_in_time_range(1000.0, 2000.0).empty());
  BOOST_TEST(spectra.indices_in_time_range(2.0, 0.5).size() == 2u);

  // Minutes-vs-seconds guard: if the API ever reverted to exposing minutes,
  // the whole run would fit inside [0, 0.5] and this would select everything.
  BOOST_TEST(spectra.indices_in_time_range(0.0, 0.5).size() < 48u);
}

/******************************************************************************/
// The EIC is a DENSE trace: one sample per selected scan, including scans with
// no signal in the m/z window. Dropping empty scans would close up gaps and
// silently distort peak shape.
BOOST_AUTO_TEST_CASE(eic_is_dense_and_time_ordered)
{
  auto spectra = MzPeak::open(kFile).spectra();

  const auto selected = spectra.indices_in_time_range(0.0, 100.0);
  const auto trace = spectra.extract_ion_chromatogram(200.0, 2000.0, 0.0, 100.0);
  BOOST_TEST(trace.size() == selected.size());

  double previous = -1.0;
  for (const auto& point : trace) {
    BOOST_TEST(point.time >= previous);
    previous = point.time;
    BOOST_TEST(point.intensity >= 0.0);
  }

  // A window with no signal still yields a sample per scan, all zero.
  const auto empty_window = spectra.extract_ion_chromatogram(1e9, 2e9, 0.0, 100.0);
  BOOST_TEST(empty_window.size() == selected.size());
  for (const auto& point : empty_window)
    BOOST_TEST(point.intensity == 0.0);

  // Narrowing the m/z window can only remove signal, never add it.
  const auto narrow = spectra.extract_ion_chromatogram(800.0, 820.0, 0.0, 100.0);
  BOOST_TEST_REQUIRE(narrow.size() == trace.size());
  for (std::size_t i = 0; i < narrow.size(); ++i) {
    BOOST_TEST(narrow[i].intensity <= trace[i].intensity);
  }
}

/******************************************************************************/
// The ms-level filter must be applied from metadata, and must restrict the
// trace to exactly the scans at that level.
BOOST_AUTO_TEST_CASE(eic_ms_level_filter_selects_only_that_level)
{
  auto spectra = MzPeak::open(kFile).spectra();

  const auto ms1 = spectra.extract_ion_chromatogram(200.0, 2000.0, 0.0, 100.0, 1);
  const auto ms2 = spectra.extract_ion_chromatogram(200.0, 2000.0, 0.0, 100.0, 2);

  // Ground truth: 14 MS1 and 34 MS2 in small.mzpeak.
  BOOST_TEST(ms1.size() == 14u);
  BOOST_TEST(ms2.size() == 34u);
  BOOST_TEST(ms1.size() + ms2.size() == 48u);

  for (const auto& point : ms1) {
    auto s = spectra[point.spectrum_index];
    BOOST_TEST(s.ms_level() == 1u);
  }
}

/******************************************************************************/
// Batch reads must preserve the caller's ORDER even though the underlying
// reads are issued in ascending index order.  Returning file order instead
// would silently mis-pair results with whatever the caller zips them against.
BOOST_AUTO_TEST_CASE(batch_preserves_requested_order)
{
  auto spectra = MzPeak::open(kFile).spectra();

  const std::vector<std::size_t> request{7, 0, 3, 1};
  const auto batch = spectra.get_spectra_batch(request);
  BOOST_TEST_REQUIRE(batch.size() == request.size());

  for (std::size_t i = 0; i < request.size(); ++i) {
    auto direct = spectra[request[i]];
    BOOST_TEST(batch[i].metadata().index == direct.metadata().index);
    BOOST_TEST(batch[i].mz().size() == direct.mz().size());
  }
}

/******************************************************************************/
// An out-of-range index yields a default spectrum rather than throwing or
// shifting the remaining results.
BOOST_AUTO_TEST_CASE(batch_tolerates_out_of_range_indices)
{
  auto spectra = MzPeak::open(kFile).spectra();

  const std::vector<std::size_t> request{0, 9999, 1};
  const auto batch = spectra.get_spectra_batch(request);
  BOOST_TEST_REQUIRE(batch.size() == 3u);

  BOOST_TEST(!batch[0].mz().empty());
  BOOST_TEST(batch[1].mz().empty()); // the out-of-range slot
  BOOST_TEST(!batch[2].mz().empty());

  // Positions 0 and 2 must still be the spectra that were asked for.
  BOOST_TEST(batch[0].metadata().index == spectra[0].metadata().index);
  BOOST_TEST(batch[2].metadata().index == spectra[1].metadata().index);

  BOOST_TEST(spectra.get_spectra_batch({}).empty());
}
