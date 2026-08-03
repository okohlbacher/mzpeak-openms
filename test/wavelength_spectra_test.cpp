/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE WavelengthSpectra
#include <boost/test/included/unit_test.hpp>

#include <algorithm>

#include "mzpeak/open.h"
#include "mzpeak/wavelength_spectra.h"
#include "mzpeak/wavelength_spectrum.h"

/******************************************************************************/
// has_uv.mzpeak contains 520 wavelength spectra.  Ground truth for wavelength
// spectrum index 0, extracted with pyarrow from wavelength_spectra_data.parquet
// (filtering point.wavelength_spectrum_index == 0):
//   size                = 96
//   wavelength[0]       = 210.0
//   wavelength[95]      = 400.0
//   intensity[0]        = -0.10919570922851562f
//   intensity[95]       = -0.003337860107421875f
// The count (520) comes from the `wavelength_spectrum_count` KV key in
// wavelength_spectra_metadata.parquet.
BOOST_AUTO_TEST_CASE(can_read_wavelength_spectra)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/has_uv.mzpeak");
  auto wavelength_spectra = mzpeak.wavelength_spectra();

  BOOST_TEST(wavelength_spectra.size() > 0u);
  BOOST_TEST(wavelength_spectra.size() == 520u);

  auto wavelength_spectrum = wavelength_spectra[0];

  const auto& wavelength = wavelength_spectrum.wavelength();
  const auto& intensity = wavelength_spectrum.intensity();

  BOOST_TEST(wavelength.size() == 96u);
  BOOST_TEST(intensity.size() == 96u);

  BOOST_TEST(wavelength.front() == 210.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(wavelength.back() == 400.0, boost::test_tools::tolerance(1e-9));

  BOOST_TEST(intensity.front() == -0.10919570922851562f,
             boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(intensity.back() == -0.003337860107421875f,
             boost::test_tools::tolerance(1e-6f));
}

/******************************************************************************/
// Metadata for the same fixture.  Ground truth from
// wavelength_spectra_metadata.parquet; times are stored in MINUTES and the API
// reports SECONDS, so the numbers below are the stored value x60.
BOOST_AUTO_TEST_CASE(reads_wavelength_spectrum_metadata)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/has_uv.mzpeak");
  auto spectra = mzpeak.wavelength_spectra();

  BOOST_TEST_REQUIRE(spectra.size() == 520u);

  const WavelengthSpectrumMetadata& first = spectra[0].metadata();
  BOOST_TEST(first.index == 0u);
  BOOST_TEST(first.id == "merged=212 row=0");
  BOOST_TEST(first.spectrum_type == "MS:1000804"); // electromagnetic radiation

  // 0.0018333333 min x 60 = 0.11 s.  Reading this as minutes would put the
  // scan 60x later than it is.
  BOOST_TEST_REQUIRE(first.time.has_value());
  BOOST_TEST(*first.time == 0.11, boost::test_tools::tolerance(1e-6));

  BOOST_TEST_REQUIRE(first.lowest_observed_wavelength.has_value());
  BOOST_TEST_REQUIRE(first.highest_observed_wavelength.has_value());
  BOOST_TEST(*first.lowest_observed_wavelength == 210.0,
             boost::test_tools::tolerance(1e-9));
  BOOST_TEST(*first.highest_observed_wavelength == 400.0,
             boost::test_tools::tolerance(1e-9));

  BOOST_TEST_REQUIRE(first.number_of_data_points.has_value());
  BOOST_TEST(*first.number_of_data_points == 96u);
  BOOST_TEST(spectra[0].wavelength().size() == *first.number_of_data_points);

  // lambda_max is the one column the legacy writer named with a literal colon
  // in its unit suffix ("..._unit_UO:0000018") instead of an underscore.  If
  // field resolution stops tolerating that spelling, this value goes absent.
  BOOST_TEST_REQUIRE(first.lambda_max.has_value());
  BOOST_TEST(*first.lambda_max == 374.0, boost::test_tools::tolerance(1e-9));

  auto index = spectra.index_for_id("merged=212 row=0");
  BOOST_TEST_REQUIRE(index.has_value());
  BOOST_TEST(*index == 0u);
  BOOST_TEST(!spectra.index_for_id("no such spectrum").has_value());
}

/******************************************************************************/
// Spectrum 104 is entirely negative -- ordinary for a baseline-subtracted
// absorbance trace.  The reference writer seeds its running maximum at zero, so
// it recorded lambda_max 0 and base peak intensity 0 rather than the true
// maximum of about -0.0863 at 362 nm.  Total ion current, which needs no
// comparison, is correct.
//
// This pins the defect so nobody "fixes" the reader to invent a plausible
// value: the file genuinely does not carry one.
BOOST_AUTO_TEST_CASE(all_negative_uv_spectrum_has_uncomputed_summaries)
{
  auto mzpeak = MzPeak::open("../test/files/has_uv.mzpeak");
  auto spectra = mzpeak.wavelength_spectra();

  // Bind the spectrum, not a reference into a temporary: operator[] returns by
  // value, and intensity() hands back a reference to that object's member.
  auto spectrum = spectra[104];
  const auto& md = spectrum.metadata();
  BOOST_TEST_REQUIRE(md.base_peak_intensity.has_value());
  BOOST_TEST(*md.base_peak_intensity == 0.0f);
  BOOST_TEST_REQUIRE(md.lambda_max.has_value());
  BOOST_TEST(*md.lambda_max == 0.0);

  BOOST_TEST_REQUIRE(md.total_ion_current.has_value());
  BOOST_TEST(*md.total_ion_current == -22.72892f,
             boost::test_tools::tolerance(1e-4f));

  // Every intensity really is negative, so a zero maximum cannot be right.
  const auto& intensity = spectrum.intensity();
  BOOST_TEST_REQUIRE(!intensity.empty());
  BOOST_TEST(*std::max_element(intensity.begin(), intensity.end()) < 0.0f);
}
