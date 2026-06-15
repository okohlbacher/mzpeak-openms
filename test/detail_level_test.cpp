/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE DetailLevel
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
// RDR-25: metadata-only reading.  Opening with DetailLevel::MetadataOnly emits
// per-spectrum scalar metadata (id, ms_level, retention_time, ...) with EMPTY
// m/z + intensity arrays and reads no array data.  Ground truth from
// spectra_metadata.parquet: spectrum 0 has id
// "controllerType=0 controllerNumber=1 scan=1", ms_level 1, time ~0.004935;
// spectrum 2 has ms_level 2; the file holds 48 spectra.
BOOST_AUTO_TEST_CASE(metadata_only_skips_arrays)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra(DetailLevel::MetadataOnly);

  BOOST_TEST(spectra.size() == 48u);

  // operator[] returns a Spectrum BY VALUE; bind before member calls.
  auto s0 = spectra[0];
  BOOST_TEST(s0.metadata().id == "controllerType=0 controllerNumber=1 scan=1");
  BOOST_TEST(s0.ms_level().has_value());
  BOOST_TEST(s0.ms_level().value() == 1);
  BOOST_TEST(s0.retention_time().has_value());
  BOOST_TEST(s0.retention_time().value() == 0.004935,
             boost::test_tools::tolerance(1e-5));

  // No array data was decoded in metadata-only mode.
  BOOST_TEST(s0.mz().empty());
  BOOST_TEST(s0.intensity().empty());

  // A different spectrum carries its own scalar metadata (ms_level 2).
  auto s2 = spectra[2];
  BOOST_TEST(s2.ms_level().has_value());
  BOOST_TEST(s2.ms_level().value() == 2);
  BOOST_TEST(s2.mz().empty());
  BOOST_TEST(s2.intensity().empty());
}

/******************************************************************************/
// RDR-25: metadata-only mode must not affect Full mode.  Re-opening with the
// default (Full) decodes the arrays as before — spectrum 0 has 13589 m/z
// points.
BOOST_AUTO_TEST_CASE(full_mode_unaffected)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra(); // default DetailLevel::Full

  auto s0 = spectra[0];
  BOOST_TEST(s0.mz().size() == 13589u);
  BOOST_TEST(s0.intensity().size() == 13589u);
}

/******************************************************************************/
// RDR-25: by-id and time-range queries use metadata, not decoded arrays, so
// they keep working in metadata-only mode (returning array-less spectra).
BOOST_AUTO_TEST_CASE(queries_work_in_metadata_only)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra(DetailLevel::MetadataOnly);

  // by-id still resolves and reads (array-less in this mode).
  const std::string id0 = "controllerType=0 controllerNumber=1 scan=1";
  auto idx = spectra.index_for_id(id0);
  BOOST_TEST(idx.has_value());
  BOOST_TEST(idx.value() == 0u);

  auto byid = spectra.by_id(id0);
  BOOST_TEST(byid.metadata().id == id0);
  BOOST_TEST(byid.mz().empty());

  // time-range query is metadata-driven and unchanged.
  auto sub = spectra.indices_in_time_range(0.011218333333, 0.04862);
  std::vector<std::size_t> expected{2, 3, 4, 5};
  BOOST_TEST(sub == expected, boost::test_tools::per_element());
}

/******************************************************************************/
// TC-12: metadata-only mode on a CHUNKED file skips all array decoding.
// Chunked layout requires its own decode path; MetadataOnly must not attempt
// any array I/O regardless of layout type.
BOOST_AUTO_TEST_CASE(chunked_metadata_only_skips_arrays)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.chunked.mzpeak");
  auto spectra = mzpeak.spectra(DetailLevel::MetadataOnly);

  BOOST_TEST(spectra.size() == 48u);

  auto s0 = spectra[0];
  // No array data must be decoded in MetadataOnly mode.
  BOOST_TEST(s0.mz().empty());
  BOOST_TEST(s0.intensity().empty());

  // Scalar metadata is still available.
  BOOST_TEST(s0.ms_level().has_value());
  BOOST_TEST(s0.ms_level().value() == 1);
}
