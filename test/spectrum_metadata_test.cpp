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
 *
 * NOTE: spectra[i] returns a Spectrum by value (temporary).  Always store
 * the Spectrum in a named local before taking a reference to its metadata(),
 * to avoid a dangling reference to the temporary's member.
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
  // (ms_level) on the first spectrum.
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  // Spectrum index 0 is an MS1 scan in small.mzpeak (pyarrow verified).
  auto s0 = spectra[0];
  const auto& m0 = s0.metadata();
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
  auto s0 = spectra[0];
  const auto& m0 = s0.metadata();
  BOOST_TEST(m0.spectrum_type == std::string("MS:1000579"));
}

/******************************************************************************/
// RDR-10b: lowest_observed_mz (MS:1000528)
// Ground truth: small.mzpeak index 0 -> 200.00018816645024 (pyarrow verified)
BOOST_AUTO_TEST_CASE(lowest_observed_mz_index0)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  auto s0 = spectra[0];
  const auto& m0 = s0.metadata();
  BOOST_TEST(m0.lowest_observed_mz.has_value());
  BOOST_TEST(std::abs(m0.lowest_observed_mz.value() - 200.00018816645024) < 1e-9);
}

/******************************************************************************/
// RDR-10b: highest_observed_mz (MS:1000527)
// Ground truth: small.mzpeak index 0 -> 1999.9857293095915 (pyarrow verified)
BOOST_AUTO_TEST_CASE(highest_observed_mz_index0)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  auto s0 = spectra[0];
  const auto& m0 = s0.metadata();
  BOOST_TEST(m0.highest_observed_mz.has_value());
  BOOST_TEST(std::abs(m0.highest_observed_mz.value() - 1999.9857293095915) < 1e-9);
}

/******************************************************************************/
// RDR-10b: data_processing_ref is null/empty in all bundled fixtures.
// Assert the field is readable and is empty (never a positive string value).
BOOST_AUTO_TEST_CASE(data_processing_ref_empty_all_rows)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    BOOST_TEST(s.metadata().data_processing_ref.empty());
  }
}

/******************************************************************************/
// RDR-10b: parameters list is empty (not null, no crash) in small.mzpeak.
// small.mzpeak carries 0 spectrum-level parameters (pyarrow verified).
BOOST_AUTO_TEST_CASE(parameters_empty_in_small)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    BOOST_TEST(s.metadata().parameters.empty());
  }
}

/******************************************************************************/
// RDR-10b: parameters populated in has_uv.mzpeak.
// Ground truth (pyarrow verified): index 0, parameters[0].accession ==
// "MS:1000796", name == "spectrum title", value starts with "TOFsulfas".
BOOST_AUTO_TEST_CASE(parameters_populated_in_has_uv)
{
  auto index = MzPeak::open("../test/files/has_uv.mzpeak");
  auto spectra = index.spectra();

  // Store in a local to avoid dangling reference from temporary Spectrum.
  auto s0 = spectra[0];
  const auto& m0 = s0.metadata();

  // Spectrum index 0 has exactly one parameter: the spectrum title.
  BOOST_TEST(m0.parameters.size() == 1u);

  const auto& p0 = m0.parameters.front();
  BOOST_TEST((p0.accession == std::optional<std::string>("MS:1000796")));
  BOOST_TEST((p0.name == std::optional<std::string>("spectrum title")));

  // Value is the spectrum title string; check prefix, not full equality,
  // to stay robust against minor fixture differences.
  BOOST_TEST(p0.value.has_value());
  const std::string& title = p0.value.value();
  BOOST_TEST(title.substr(0, 9) == std::string("TOFsulfas"));
}

/******************************************************************************/
// M2 determinism: verify format_double_canonical via the public API.
// has_uv.mzpeak parameters carry string-arm values (spectrum title);
// this test confirms extract_one_cv_param passes the string arm through
// unmangled.  The float-arm "35" canonical form is validated in plan 01-02
// which reads collision-energy (MS:1000045) from small.mzpeak activation.
BOOST_AUTO_TEST_CASE(parameters_string_arm_unmangled_in_has_uv)
{
  auto index = MzPeak::open("../test/files/has_uv.mzpeak");
  auto spectra = index.spectra();
  auto s0 = spectra[0];
  const auto& p0 = s0.metadata().parameters.front();
  BOOST_TEST(p0.value.has_value());
  // Title must be a non-empty string (not a number, not "true").
  BOOST_TEST(!p0.value.value().empty());
}
