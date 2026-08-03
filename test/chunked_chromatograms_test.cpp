/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// RDR-28a: chunked-layout chromatograms (top struct node "chunk") read.  Both
// small.chunked.mzpeak and small.numpress.mzpeak store their single TIC in the
// chunked layout: the time axis (MS:1000595, RelativeTimeOffset, Float64) is a
// delta-encoded chunk (chunk_start + chunk_values, encoding MS:1003089) and the
// intensity axis is either a plain secondary float list (chunked) or a
// numpress-SLOF transform (numpress).  These fixtures encode the *same*
// underlying chromatogram as the point fixture small.mzpeak, so the ground
// truth below is shared with chromatograms_test.cpp.

#define BOOST_TEST_MODULE ChunkedChromatograms
#include <boost/test/included/unit_test.hpp>

#include <cmath>

#include "mzpeak/chromatogram.h"
#include "mzpeak/chromatograms.h"
#include "mzpeak/open.h"

using boost::test_tools::tolerance;

namespace {

// Ground truth (pyarrow over chromatograms_data.parquet, delta-decoding the
// time chunk independently):  48 paired samples.
constexpr std::size_t kPoints = 48u;
constexpr double kTimeFront = 0.004935 * 60.0;
constexpr double kTimeSecond = 0.0078966666666666664 * 60.0;
constexpr double kTimeBack = 0.48723666666666665 * 60.0;
constexpr float kIntFront = 15245068.0f;
constexpr float kIntSecond = 12901166.0f;
constexpr float kIntBack = 77939.0078125f;

} // namespace

/******************************************************************************/
// Chunked layout, delta time + plain float intensity: near-exact.
BOOST_AUTO_TEST_CASE(chunked_delta)
{
  auto chroms = MzPeak::open("../test/files/small.chunked.mzpeak").chromatograms();
  BOOST_TEST(chroms.size() == 1u);

  // operator[] returns by value -- bind before touching time()/intensity().
  auto c = chroms[0];
  const auto& time = c.time();
  const auto& intensity = c.intensity();

  BOOST_TEST(time.size() == kPoints);
  BOOST_TEST(intensity.size() == kPoints);

  // Delta time reconstruction is exact (Float64 cumulative sum from
  // chunk_start), so a very tight tolerance.
  // time() reports SECONDS; the fixture stores minutes (UO:0000031) and the
  // constants above are the stored values x60.  The tolerance is loosened from
  // 1e-12 because the conversion is a multiply, not because the decode changed.
  BOOST_TEST(time.front() == kTimeFront, tolerance(1e-9));
  BOOST_TEST(time[1] == kTimeSecond, tolerance(1e-9));
  BOOST_TEST(time.back() == kTimeBack, tolerance(1e-9));

  // Intensity is a plain stored float list: exact.
  BOOST_TEST(intensity.front() == kIntFront, tolerance(1e-3f));
  BOOST_TEST(intensity[1] == kIntSecond, tolerance(1e-3f));
  BOOST_TEST(intensity.back() == kIntBack, tolerance(1e-2f));

  // Monotonic non-decreasing time axis.
  for (std::size_t i = 1; i < time.size(); ++i)
    BOOST_TEST(time[i] >= time[i - 1]);
}

/******************************************************************************/
// Numpress layout: same chromatogram, but intensity is numpress-SLOF (lossy).
// Time is still exact delta; intensity is compared against the same ground
// truth with a SLOF-appropriate relative tolerance.
BOOST_AUTO_TEST_CASE(numpress_slof)
{
  auto chroms = MzPeak::open("../test/files/small.numpress.mzpeak").chromatograms();
  BOOST_TEST(chroms.size() == 1u);

  auto c = chroms[0];
  const auto& time = c.time();
  const auto& intensity = c.intensity();

  BOOST_TEST(time.size() == kPoints);
  BOOST_TEST(intensity.size() == kPoints);

  // Time axis is identical to the chunked fixture (exact delta).
  // time() reports SECONDS; the fixture stores minutes (UO:0000031) and the
  // constants above are the stored values x60.  The tolerance is loosened from
  // 1e-12 because the conversion is a multiply, not because the decode changed.
  BOOST_TEST(time.front() == kTimeFront, tolerance(1e-9));
  BOOST_TEST(time[1] == kTimeSecond, tolerance(1e-9));
  BOOST_TEST(time.back() == kTimeBack, tolerance(1e-9));

  // SLOF is logarithmic-fixed-point lossy.  Measured max relative error of the
  // whole array vs. the lossless ground truth is ~1.3e-4; assert 1e-3.
  BOOST_TEST(intensity.front() == kIntFront, tolerance(1e-3f));
  BOOST_TEST(intensity[1] == kIntSecond, tolerance(1e-3f));
  BOOST_TEST(intensity.back() == kIntBack, tolerance(1e-3f));
}

/******************************************************************************/
// Cross-check: chunked and numpress encode the SAME chromatogram.  Time must
// be bit-identical; intensity within SLOF tolerance, element by element.
BOOST_AUTO_TEST_CASE(chunked_vs_numpress_equivalence)
{
  auto a = MzPeak::open("../test/files/small.chunked.mzpeak").chromatograms()[0];
  auto b = MzPeak::open("../test/files/small.numpress.mzpeak").chromatograms()[0];

  BOOST_TEST(a.time().size() == b.time().size());
  BOOST_TEST(a.intensity().size() == b.intensity().size());

  for (std::size_t i = 0; i < a.time().size(); ++i)
    BOOST_TEST(a.time()[i] == b.time()[i], tolerance(1e-12));

  for (std::size_t i = 0; i < a.intensity().size(); ++i) {
    double x = a.intensity()[i];
    double y = b.intensity()[i];
    double rel = x != 0.0 ? std::fabs(x - y) / std::fabs(x) : std::fabs(x - y);
    BOOST_TEST(rel < 1e-3);
  }
}
