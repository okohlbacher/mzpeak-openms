/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Eic
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
// RDR-17: extracted-ion chromatogram.  Ground truth computed independently in
// python (pyarrow) over test/files/small.mzpeak.
//
// The RT window [0.011, 0.11] selects 10 spectra (ascending time): indices
// {2,3,4,5,6,7,8,9,10,11}.  Indices 7 and 8 are MS1 (profile, spectra_data);
// the other eight are MS2 (centroid, spectra_peaks).  The m/z window
// [400, 410] summed over the eight MS2 scans gives (double-accumulated):
//   idx  time            summed intensity
//   2    0.011218333333  1686.5167...
//   3    0.022838333333  1890.8547...
//   4    0.034925        418.7325...
//   5    0.04862         579.9818...
//   6    0.061923333333  1812.8755...
//   9    0.081203333333  2145.2410...
//   10   0.092903333333  1539.2138...
//   11   0.104803333333  363.21295...
BOOST_AUTO_TEST_CASE(eic_ms2_window_values)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // Restrict to MS2 so the points come straight from the peaks table (raw,
  // directly-stored m/z and intensity) — matches the python ground truth.
  auto eic = spectra.extract_ion_chromatogram(400.0, 410.0, 0.011, 0.11, 2);

  const std::vector<std::size_t> expect_idx{2, 3, 4, 5, 6, 9, 10, 11};
  const std::vector<double> expect_int{1686.5167, 1890.8547, 418.7325,  579.9818,
                                       1812.8755, 2145.2410, 1539.2138, 363.21295};

  BOOST_TEST(eic.size() == expect_idx.size());

  for (std::size_t i = 0; i < eic.size(); ++i) {
    BOOST_TEST(eic[i].spectrum_index == expect_idx[i]);
    // Ascending time order.
    if (i > 0) BOOST_TEST(eic[i - 1].time < eic[i].time);
    // Per-scan summed intensity within a relative tolerance for float sums.
    BOOST_TEST(eic[i].intensity == expect_int[i],
               boost::test_tools::tolerance(1e-3));
  }
}

/******************************************************************************/
// RDR-17: ms_level filtering.  The same RT window without a filter spans all
// 10 scans (8 MS2 + 2 MS1); restricting to ms_level==1 keeps only the two MS1
// scans (indices 7, 8), and ms_level==2 keeps the eight MS2 scans.
BOOST_AUTO_TEST_CASE(eic_ms_level_filter)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto all = spectra.extract_ion_chromatogram(400.0, 410.0, 0.011, 0.11);
  BOOST_TEST(all.size() == 10u);

  auto ms1 = spectra.extract_ion_chromatogram(400.0, 410.0, 0.011, 0.11, 1);
  BOOST_TEST(ms1.size() == 2u);
  BOOST_TEST(ms1[0].spectrum_index == 7u);
  BOOST_TEST(ms1[1].spectrum_index == 8u);

  auto ms2 = spectra.extract_ion_chromatogram(400.0, 410.0, 0.011, 0.11, 2);
  BOOST_TEST(ms2.size() == 8u);
}

/******************************************************************************/
// RDR-17: dense-trace policy.  A narrow m/z window [1500, 1510] over the same
// eight MS2 scans leaves most scans empty; the EIC still emits one point per
// selected scan (intensity 0 for empty ones).  Python ground truth: only
// indices 3 (214.417...) and 10 (168.340...) carry intensity, the rest are 0.
BOOST_AUTO_TEST_CASE(eic_emits_dense_zeros)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto eic = spectra.extract_ion_chromatogram(1500.0, 1510.0, 0.011, 0.11, 2);

  // One point per selected MS2 scan, even the empty ones.
  BOOST_TEST(eic.size() == 8u);
  std::size_t zeros = 0, nonzeros = 0;
  for (const auto& p : eic) {
    if (p.intensity == 0.0) ++zeros;
    else
      ++nonzeros;
  }
  BOOST_TEST(nonzeros == 2u);
  BOOST_TEST(zeros == 6u);

  // The two carriers are indices 3 and 10 with the expected sums.
  for (const auto& p : eic) {
    if (p.spectrum_index == 3u)
      BOOST_TEST(p.intensity == 214.4174, boost::test_tools::tolerance(1e-3));
    if (p.spectrum_index == 10u)
      BOOST_TEST(p.intensity == 168.3408, boost::test_tools::tolerance(1e-3));
  }
}

/******************************************************************************/
// RDR-18: batch read returns spectra in the SAME order as the input indices
// (not sorted read order).  Sizes from pyarrow: index 0 -> 13589, index 1 ->
// 18177, index 2 -> 485.
BOOST_AUTO_TEST_CASE(batch_preserves_input_order)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto batch = spectra.get_spectra_batch({2, 0, 1});
  BOOST_TEST(batch.size() == 3u);
  BOOST_TEST(batch[0].mz().size() == 485u);   // index 2
  BOOST_TEST(batch[1].mz().size() == 13589u); // index 0
  BOOST_TEST(batch[2].mz().size() == 18177u); // index 1
}

/******************************************************************************/
// RDR-18: out-of-range policy.  An out-of-range index degrades to an empty
// Spectrum while in-range indices read normally, preserving positions.
BOOST_AUTO_TEST_CASE(batch_out_of_range_yields_empty)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // 9999 is past the 48-spectrum count; it maps to an empty Spectrum.
  auto batch = spectra.get_spectra_batch({0, 9999, 2});
  BOOST_TEST(batch.size() == 3u);
  BOOST_TEST(batch[0].mz().size() == 13589u); // index 0
  BOOST_TEST(batch[1].mz().empty());          // out-of-range -> empty
  BOOST_TEST(batch[1].intensity().empty());
  BOOST_TEST(batch[2].mz().size() == 485u); // index 2
}

/******************************************************************************/
// RDR-17: out-of-range m/z window.  A window whose lo bound exceeds the
// maximum m/z value present in any spectrum returns an EIC where every
// intensity is 0.0.  The dense-trace policy still emits one point per
// selected scan, so the vector length equals the number of selected scans
// (8 MS2 scans in the RT window [0.011, 0.11]).
BOOST_AUTO_TEST_CASE(eic_empty_for_out_of_range_mz)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto eic = spectra.extract_ion_chromatogram(99999.0, 100000.0, 0.011, 0.11, 2);

  // Dense-trace policy: one point per selected MS2 scan even when the window
  // is entirely empty.
  BOOST_TEST(eic.size() == 8u);
  for (const auto& p : eic) {
    BOOST_TEST(p.intensity == 0.0);
  }
}

/******************************************************************************/
// RDR-17: full m/z window equals no filter.  A window [0, 1e9] covers every
// peak in every spectrum.  Over the eight MS2 scans in RT [0.011, 0.11] the
// EIC must contain exactly 8 points, all intensities must be non-negative, and
// the points must be in strictly ascending RT order.
BOOST_AUTO_TEST_CASE(eic_full_mz_window_equals_no_filter)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto eic = spectra.extract_ion_chromatogram(0.0, 1e9, 0.011, 0.11, 2);

  BOOST_TEST(eic.size() == 8u);
  for (const auto& p : eic) {
    BOOST_TEST(p.intensity >= 0.0);
  }
  // Verify strictly ascending RT order.
  for (std::size_t i = 1; i < eic.size(); ++i) {
    BOOST_TEST(eic[i - 1].time < eic[i].time);
  }
}

/******************************************************************************/
// TC-13: an ms_level filter for a level absent in the file (e.g. MS3) yields
// an empty EIC, not a crash or spurious points.
BOOST_AUTO_TEST_CASE(eic_empty_for_absent_ms_level)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // small.mzpeak has only MS1 and MS2 scans; MS3 is absent.
  auto eic = spectra.extract_ion_chromatogram(400.0, 410.0, 0.0, 1.0, 3);
  BOOST_TEST(eic.empty());
}

/******************************************************************************/
// FC-07: extract_ion_chromatogram must throw std::invalid_argument when
// mz_low > mz_high (inverted range).
BOOST_AUTO_TEST_CASE(eic_rejects_inverted_mz_range)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  BOOST_CHECK_THROW(spectra.extract_ion_chromatogram(410.0, 400.0, 0.0, 1.0),
                    std::invalid_argument);
}
