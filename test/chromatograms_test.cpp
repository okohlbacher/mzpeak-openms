/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Chromatograms
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/chromatogram.h"
#include "mzpeak/chromatograms.h"
#include "mzpeak/open.h"

/******************************************************************************/
// small.mzpeak contains a single chromatogram (index 0) with 48 points.
// Ground truth extracted with pyarrow from chromatograms_data.parquet:
//   size                = 48
//   time[0]             = 0.004935
//   time[47]            = 0.48723666666666665
//   intensity[0]        = 15245068.0f
//   intensity[47]       = 77939.0078125f
BOOST_AUTO_TEST_CASE(can_read_chromatograms)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto chromatograms = mzpeak.chromatograms();

  BOOST_TEST(chromatograms.size() > 0u);
  BOOST_TEST(chromatograms.size() == 1u);

  auto chromatogram = chromatograms[0];

  const auto& time = chromatogram.time();
  const auto& intensity = chromatogram.intensity();

  BOOST_TEST(time.size() == 48u);
  BOOST_TEST(intensity.size() == 48u);

  BOOST_TEST(time.front() == 0.004935, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(time.back() == 0.48723666666666665,
             boost::test_tools::tolerance(1e-9));

  BOOST_TEST(intensity.front() == 15245068.0f,
             boost::test_tools::tolerance(1.0f));
  BOOST_TEST(intensity.back() == 77939.0078125f,
             boost::test_tools::tolerance(0.01f));
}
