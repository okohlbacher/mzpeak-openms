/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE WavelengthSpectra
#include <boost/test/included/unit_test.hpp>

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
