/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Chunked
#include <boost/test/included/unit_test.hpp>

#include <cmath>
#include <vector>

#include "mzpeak/open.h"
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
