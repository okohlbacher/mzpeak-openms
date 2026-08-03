/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Chunked
#include <boost/test/included/unit_test.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/schema/buffer_format.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
// Materialize the POINT reference spectrum `idx` (small.mzpeak) into plain
// vectors.  The reference is the oracle the chunked layouts must reproduce.
static void
reference_spectrum(std::size_t idx, std::vector<double>& mz, std::vector<float>& in)
{
  auto ref = MzPeak::open("../test/files/small.mzpeak");
  auto s = ref.spectra()[idx];
  mz = s.mz();
  in = s.intensity();
}

/******************************************************************************/
// CHUNKED, delta encoding (MS:1003089).  The reconstructed m/z and intensity
// of spectrum 0 must match the POINT file exactly (delta is lossless).
BOOST_AUTO_TEST_CASE(reads_delta_chunked_spectrum)
{
  using namespace MzPeak;

  std::vector<double> ref_mz;
  std::vector<float> ref_in;
  reference_spectrum(0, ref_mz, ref_in);

  auto mzpeak = MzPeak::open("../test/files/small.chunked.mzpeak");
  auto spectra = mzpeak.spectra();
  BOOST_TEST(spectra.size() == 48);

  auto s = spectra[0];
  const auto& mz = s.mz();
  const auto& in = s.intensity();

  BOOST_TEST_REQUIRE(mz.size() == ref_mz.size());
  BOOST_TEST_REQUIRE(in.size() == ref_in.size());

  for (std::size_t i = 0; i < mz.size(); ++i) {
    BOOST_TEST(mz[i] == ref_mz[i], boost::test_tools::tolerance(1e-6));
  }
  for (std::size_t i = 0; i < in.size(); ++i) {
    BOOST_TEST(in[i] == ref_in[i], boost::test_tools::tolerance(1e-3f));
  }
}

/******************************************************************************/
// Delta reconstruction must hold for spectra whose first chunk does not start
// at row 0 of the record batch and that cross many chunk boundaries.
BOOST_AUTO_TEST_CASE(reads_delta_chunked_later_spectra)
{
  using namespace MzPeak;

  for (std::size_t idx : {std::size_t(1), std::size_t(7)}) {
    // Read the chunked spectrum first, materializing it into a vector, then
    // open the reference.  (Opening a second archive disturbs the first
    // reader's lazy state, so we never hold both open simultaneously.)
    std::vector<double> cmz;
    {
      auto chunked = MzPeak::open("../test/files/small.chunked.mzpeak");
      cmz = chunked.spectra()[idx].mz();
    }

    std::vector<double> rmz;
    std::vector<float> rin;
    reference_spectrum(idx, rmz, rin);

    BOOST_TEST_REQUIRE(cmz.size() == rmz.size());
    for (std::size_t i = 0; i < cmz.size(); ++i) {
      BOOST_TEST(cmz[i] == rmz[i], boost::test_tools::tolerance(1e-6));
    }
  }
}

/******************************************************************************/
// CHUNKED, numpress (m/z linear MS:1002312, intensity SLOF MS:1002314).
// numpress is lossy: ~1e-4 absolute on m/z, ~1e-3 relative on intensity.
BOOST_AUTO_TEST_CASE(reads_numpress_chunked_spectrum)
{
  using namespace MzPeak;

  std::vector<double> ref_mz;
  std::vector<float> ref_in;
  reference_spectrum(0, ref_mz, ref_in);

  auto mzpeak = MzPeak::open("../test/files/small.numpress.mzpeak");
  auto spectra = mzpeak.spectra();
  BOOST_TEST(spectra.size() == 48);

  auto s = spectra[0];
  const auto& mz = s.mz();
  const auto& in = s.intensity();

  BOOST_TEST_REQUIRE(mz.size() == ref_mz.size());
  BOOST_TEST_REQUIRE(in.size() == ref_in.size());

  // numpress linear m/z: small absolute error.
  for (std::size_t i = 0; i < mz.size(); ++i) {
    BOOST_TEST(std::abs(mz[i] - ref_mz[i]) < 1e-4);
  }

  // numpress SLOF intensity: ~0.05% relative error.  Compare relatively for
  // non-trivial values; near-zero values stay near zero.
  for (std::size_t i = 0; i < in.size(); ++i) {
    float a = in[i], b = ref_in[i];
    if (std::abs(b) < 1.0f) {
      BOOST_TEST(std::abs(a - b) < 1.0f);
    } else {
      BOOST_TEST(std::abs(a - b) / std::abs(b) < 2e-3f);
    }
  }
}

/******************************************************************************/
// TC-16: buffer_format_from_string() must return BufferFormat::Point for any
// unrecognised encoding CURIE.  New vocabulary entries written by a future
// mzPeak version appear as unknown strings here; falling back to Point
// preserves forward-compatibility instead of throwing or asserting.
BOOST_AUTO_TEST_CASE(unknown_buffer_format_curie_falls_back_to_point)
{
  using MzPeak::Schema::buffer_format_from_string;
  using MzPeak::Schema::BufferFormat;

  BOOST_CHECK(buffer_format_from_string("MS:9999999") == BufferFormat::Point);
  BOOST_CHECK(buffer_format_from_string("not_a_real_format") == BufferFormat::Point);
  BOOST_CHECK(buffer_format_from_string("") == BufferFormat::Point);
}

/******************************************************************************/
// The chunked file holds the SAME run as small.mzpeak, so the point-layout
// twin is the oracle: intensities must match exactly (both store them
// verbatim), and m/z must match exactly at real points.  Null-marked points are
// reconstructed from neighbouring runs and the two layouts can see slightly
// different run extents, so those get a tolerance far below any meaningful mass
// accuracy.
BOOST_AUTO_TEST_CASE(chunked_matches_the_point_layout_twin)
{
  auto chunked = MzPeak::open("../test/files/small.chunked.mzpeak").spectra();
  auto point = MzPeak::open("../test/files/small.mzpeak").spectra();
  BOOST_TEST_REQUIRE(chunked.size() == point.size());

  for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(7)}) {
    auto c = chunked[i];
    auto p = point[i];
    const auto& cmz = c.mz();
    const auto& pmz = p.mz();
    const auto& cin = c.intensity();
    const auto& pin = p.intensity();

    BOOST_TEST_REQUIRE(cmz.size() == pmz.size());
    BOOST_TEST_REQUIRE(cin.size() == pin.size());

    for (std::size_t k = 0; k < cmz.size(); ++k) {
      if (cin[k] != pin[k]) {
        BOOST_TEST(cin[k] == pin[k]);
        break;
      }
      // EXACT, including at reconstructed points.  Both layouts assemble the
      // whole entity before filling nulls, so neither depends on how the writer
      // happened to split chunks or record batches.  Any tolerance here would
      // hide exactly the boundary-dependence this asserts is gone.
      if (cmz[k] != pmz[k]) {
        BOOST_TEST(cmz[k] == pmz[k]);
        break;
      }
    }
  }
}

/******************************************************************************/
// Regression: spectrum 0 is PROFILE (MS:1000128) yet ALSO declares a peak
// count, because the file carries a centroid array for it as well.  Dispatching
// on "does it have peaks" rather than on the representation returned 1612
// centroids in place of 13589 profile points — the wrong array, silently, at a
// plausible length, and it hid the chunked path entirely.
BOOST_AUTO_TEST_CASE(profile_spectrum_reads_the_profile_array_not_the_peaks)
{
  auto chunked = MzPeak::open("../test/files/small.chunked.mzpeak").spectra();

  auto s = chunked[0];
  const auto& md = s.metadata();
  BOOST_TEST(md.representation == std::string("MS:1000128"));
  BOOST_TEST(md.number_of_data_points.value_or(0) == 13589u);
  BOOST_TEST(md.number_of_peaks.value_or(0) > 0u); // both arrays exist
  BOOST_TEST(s.mz().size() == 13589u);
}

/******************************************************************************/
// Null marking still applies once the chunks are assembled: a chunk boundary
// must not introduce a discontinuity, a NaN, or a negative intensity.
BOOST_AUTO_TEST_CASE(null_marking_applies_to_the_assembled_axis)
{
  auto spectra = MzPeak::open("../test/files/small.chunked.mzpeak").spectra();
  auto s = spectra[0];
  const auto& mz = s.mz();
  const auto& intensity = s.intensity();
  BOOST_TEST_REQUIRE(mz.size() == intensity.size());

  for (std::size_t k = 1; k < mz.size(); ++k) {
    if (!(mz[k] > mz[k - 1]) || !std::isfinite(mz[k])) {
      BOOST_TEST(mz[k] > mz[k - 1]);
      break;
    }
  }
  for (std::size_t k = 0; k < intensity.size(); ++k) {
    if (intensity[k] < 0.0f) {
      BOOST_TEST(intensity[k] >= 0.0f);
      break;
    }
  }
}

/******************************************************************************/
// has_uv stores two intensity columns for one logical array (detector counts
// and absorbance); each chromatogram populates exactly one.  Both are `point`
// format, so this must take the coalescing path rather than being mistaken for
// a chunked layout, and must yield as many intensities as time points.
BOOST_AUTO_TEST_CASE(multiple_point_columns_coalesce_into_one_array)
{
  auto index = MzPeak::open("../test/files/has_uv.mzpeak");
  auto chromatograms = index.chromatograms();
  BOOST_TEST_REQUIRE(chromatograms.size() >= 2u);

  for (std::size_t i = 0; i < 2; ++i) {
    auto c = chromatograms[i];
    BOOST_TEST(c.time().size() == c.intensity().size());
    BOOST_TEST(!c.time().empty());
  }
}
