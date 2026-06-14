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
