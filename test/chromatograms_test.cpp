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
  BOOST_TEST(time.back() == 0.48723666666666665, boost::test_tools::tolerance(1e-9));

  BOOST_TEST(intensity.front() == 15245068.0f, boost::test_tools::tolerance(1.0f));
  BOOST_TEST(intensity.back() == 77939.0078125f,
             boost::test_tools::tolerance(0.01f));
}

/******************************************************************************/
// has_uv.mzpeak carries two chromatograms of DIFFERENT KINDS, which is what
// makes it worth testing: a mass-spectrometry TIC in detector counts and an
// Agilent DAD absorption trace in absorbance units.  Ground truth from
// chromatograms_metadata.parquet.
BOOST_AUTO_TEST_CASE(reads_chromatogram_metadata)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/has_uv.mzpeak");
  auto chromatograms = mzpeak.chromatograms();

  BOOST_TEST_REQUIRE(chromatograms.size() == 2u);

  const ChromatogramMetadata& tic = chromatograms[0].metadata();
  BOOST_TEST(tic.index == 0u);
  BOOST_TEST(tic.id == "TIC");
  BOOST_TEST(tic.chromatogram_type == "MS:1000235"); // total ion current
  BOOST_TEST(tic.number_of_data_points.has_value());
  BOOST_TEST(*tic.number_of_data_points == 212u);
  // The declared point count must agree with what actually decodes; a
  // disagreement means the metadata and signal tables describe different runs.
  BOOST_TEST(chromatograms[0].time().size() == *tic.number_of_data_points);

  const ChromatogramMetadata& dad = chromatograms[1].metadata();
  BOOST_TEST(dad.id == "DAD1 A: Sig=272,16 Ref=360,100");
  BOOST_TEST(dad.chromatogram_type == "MS:1000812"); // absorption chromatogram
  BOOST_TEST_REQUIRE(dad.number_of_data_points.has_value());
  BOOST_TEST(*dad.number_of_data_points == 526u);
  BOOST_TEST(chromatograms[1].time().size() == 526u);

  // Neither is an SRM trace, so neither loses a product.
  BOOST_TEST(!tic.has_unreadable_product);
  BOOST_TEST(!dad.has_unreadable_product);

  // Native ids resolve, and resolve to the right chromatogram.
  auto index = chromatograms.index_for_id("TIC");
  BOOST_TEST_REQUIRE(index.has_value());
  BOOST_TEST(*index == 0u);
  BOOST_TEST(chromatograms.by_id("TIC").metadata().id == "TIC");
  BOOST_TEST(!chromatograms.index_for_id("no such chromatogram").has_value());
}

/******************************************************************************/
// The reference writer emits 0 for an unknown scan polarity where the
// specification calls for null, so a 0 here means "the source could not say"
// rather than a real polarity.  Pinned so a future writer change is noticed.
BOOST_AUTO_TEST_CASE(chromatogram_polarity_is_the_writers_unknown_sentinel)
{
  auto mzpeak = MzPeak::open("../test/files/has_uv.mzpeak");
  auto chromatograms = mzpeak.chromatograms();

  const auto& polarity = chromatograms[0].metadata().polarity;
  BOOST_TEST_REQUIRE(polarity.has_value());
  BOOST_TEST(*polarity == 0);
}
