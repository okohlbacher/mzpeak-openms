/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * RDR-10b — verify per-spectrum descriptive metadata (spectrum_type,
 * lowest/highest observed m/z, data_processing_ref, and the parameters
 * CvParam list) is read from spectra_metadata.parquet and exposed via
 * SpectrumMetadata.  Ground truth values are verified via pyarrow on the
 * bundled fixtures (small.mzpeak and has_uv.mzpeak).
 */
#define BOOST_TEST_MODULE SpectrumMetadata
#include <boost/test/included/unit_test.hpp>

#include <cmath>
#include <string>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(opens_and_reads_metadata)
{
  // Smoke test: open small.mzpeak and verify an already-existing field
  // (ms_level) on the first spectrum.  This keeps the scaffold compiling
  // before the RDR-10b fields land in Task 2.
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  // Spectrum index 0 is an MS1 scan in small.mzpeak (pyarrow verified).
  const auto& m0 = spectra[0].metadata();
  BOOST_TEST(m0.ms_level.has_value());
  BOOST_TEST(m0.ms_level.value() == 1);
}

/******************************************************************************/
// RDR-10b: spectrum_type field (MS:1000559)
// Ground truth: small.mzpeak index 0 -> "MS:1000579" (pyarrow verified)
BOOST_AUTO_TEST_CASE(spectrum_type_ms1)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  const auto& m0 = spectra[0].metadata();
  BOOST_TEST(m0.spectrum_type == std::string("MS:1000579"));
}

/******************************************************************************/
// RDR-10b: lowest_observed_mz (MS:1000528)
// Ground truth: small.mzpeak index 0 -> 200.00018816645024 (pyarrow verified)
BOOST_AUTO_TEST_CASE(lowest_observed_mz_index0)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  const auto& m0 = spectra[0].metadata();
  BOOST_TEST(m0.lowest_observed_mz.has_value());
  BOOST_TEST(
      std::abs(m0.lowest_observed_mz.value() - 200.00018816645024) < 1e-9);
}

/******************************************************************************/
// RDR-10b: highest_observed_mz (MS:1000527)
// Ground truth: small.mzpeak index 0 -> 1999.9857293095915 (pyarrow verified)
BOOST_AUTO_TEST_CASE(highest_observed_mz_index0)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  const auto& m0 = spectra[0].metadata();
  BOOST_TEST(m0.highest_observed_mz.has_value());
  BOOST_TEST(
      std::abs(m0.highest_observed_mz.value() - 1999.9857293095915) < 1e-9);
}

/******************************************************************************/
// RDR-10b: data_processing_ref is null/empty in all bundled fixtures.
// Assert the field is readable and is empty (never a positive string value).
BOOST_AUTO_TEST_CASE(data_processing_ref_empty_all_rows)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    BOOST_TEST(spectra[i].metadata().data_processing_ref.empty());
  }
}
