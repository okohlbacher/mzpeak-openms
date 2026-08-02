/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Spectra
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_read_spectra)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.dir");
  auto spectra = mzpeak.spectra();

  BOOST_TEST((spectra.size() == 48));

  auto spectrum = spectra[0];
  auto mz = spectrum.mz();

  BOOST_TEST(mz.size() == 13589);
  BOOST_TEST(mz[0] == 202.607, boost::test_tools::tolerance(0.001));
  BOOST_TEST(mz[mz.size() - 1] == 1999.840, boost::test_tools::tolerance(0.001));

  // Test some NULL values.
  BOOST_TEST(mz[7] == 202.608, boost::test_tools::tolerance(0.001));
  BOOST_TEST(mz[8] == 202.609, boost::test_tools::tolerance(0.001));
  BOOST_TEST(mz[14] == 204.761, boost::test_tools::tolerance(0.001));
  BOOST_TEST(mz[15] == 204.762, boost::test_tools::tolerance(0.001));

  // The m/z values should be monotonically increasing.
  for (std::size_t i : std::views::iota(1ul, mz.size())) {
    BOOST_TEST(mz[i] > mz[i - 1]);
  }

  auto intensity = spectrum.intensity();
  BOOST_TEST((intensity.size() == mz.size()));
  BOOST_TEST(intensity[0] == 0.0, boost::test_tools::tolerance(0.001));
  BOOST_TEST(intensity[1] == 1938.12, boost::test_tools::tolerance(0.001));
  BOOST_TEST(intensity[intensity.size() - 1] == 0.0,
             boost::test_tools::tolerance(0.001));

  BOOST_TEST(spectrum.ms_level() == 1u);
}

/******************************************************************************/
// Regression: profile spectra whose first matching row is not at the start of
// its record batch were over-read (data_arrays.cpp slice length used an
// absolute end index instead of a row count).  Spectra 1 and 7 exercise this;
// spectrum 0 (which starts at row 0) always read correctly.
BOOST_AUTO_TEST_CASE(reads_profile_arrays_beyond_first_spectrum)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto s1 = spectra[1].mz();
  BOOST_TEST(s1.size() == 18177);
  BOOST_TEST(s1.front() == 200.0909, boost::test_tools::tolerance(0.001));
  BOOST_TEST(s1.back() == 1999.8182, boost::test_tools::tolerance(0.001));

  auto s7 = spectra[7].mz();
  BOOST_TEST(s7.size() == 10329);
  BOOST_TEST(s7.back() == 1832.2386, boost::test_tools::tolerance(0.001));
}

/******************************************************************************/
// Regression: null-marked INTENSITY values must decode to 0, not be run through
// the m/z delta-model interpolator.
//
// small.mzpeak spectrum 0 has 2376 nulls in both the mz and intensity columns
// at identical positions (7, 8, 14, 15, 21, 22, ... — pyarrow verified).  The
// reference writer tags the intensity array MS:1003902, which the PSI-MS CV
// defines as the m/z-INTERPOLATING transform, so keying the delta-model
// decision off the transform accession interpolated intensity and produced
// physically impossible negative values (-827.55 at position 8, -2721.54 at 15).
// Null-marking reconstruction must follow sorting_rank == 0 instead.
BOOST_AUTO_TEST_CASE(null_marked_intensity_reads_as_zero_not_interpolated)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();
  auto spectrum = spectra[0];
  const auto& inten = spectrum.intensity();
  BOOST_TEST_REQUIRE(inten.size() == 13589u);

  // Positions that are null in the source intensity column must read as 0.
  for (std::size_t i : {7u, 8u, 14u, 15u, 21u, 22u}) {
    BOOST_TEST(inten[i] == 0.0f);
  }

  // And no intensity anywhere may be negative.
  for (std::size_t i = 0; i < inten.size(); ++i) {
    BOOST_TEST(inten[i] >= 0.0f);
  }
}

/******************************************************************************/
// Regression: intensity was decoded as Int32 while the array is float32, so
// the FloatArray bytes were reinterpreted as integers (garbage).  Spectrum 0
// reads correctly regardless of the slice bug, isolating the type fix.
BOOST_AUTO_TEST_CASE(decodes_intensity_as_float32)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto inten = spectra[0].intensity();
  BOOST_TEST(inten.size() == 13589);
  BOOST_TEST(inten[1] == 1938.1174f, boost::test_tools::tolerance(0.01f));
  BOOST_TEST(inten[2] == 2572.8389f, boost::test_tools::tolerance(0.01f));
}
