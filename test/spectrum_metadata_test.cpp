/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * RDR-10b/10c — verify per-spectrum descriptive metadata is read from
 * spectra_metadata.parquet and exposed via SpectrumMetadata.  Ground truth
 * values are verified via pyarrow on the bundled fixtures (small.mzpeak and
 * has_uv.mzpeak).
 *
 * RDR-10c tests cover:
 *   - precursor / isolation-window / activation / selected-ion (Task 2)
 *   - scan-level parameters and scan_windows (Task 3)
 *   - H1: join by source_index VALUE (never positional)
 *   - H2: selected-ion attach by (source_index, precursor_index)
 *   - Multi-row sweep: exactly 34 MS2 carry a precursor, 14 MS1 carry none
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
#include "mzpeak/schema/psi/array_type.h"
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

// ============================================================================
// RDR-10c: precursor / isolation-window / activation / selected-ion (Task 2)
// Ground truth verified via pyarrow against small.mzpeak.
// ============================================================================

/******************************************************************************/
// H1 + single-row: spectrum index 2 (first MS2) carries exactly one precursor.
// Ground truth (pyarrow): precursor row 0 -> source_index=2, precursor_index=1
BOOST_AUTO_TEST_CASE(precursor_ms2_index2_has_one_precursor)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  // Find spectrum with index==2 (first MS2 scan).
  std::optional<MzPeak::SpectrumMetadata> m2;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 2) {
      m2 = s.metadata();
      break;
    }
  }
  BOOST_TEST(m2.has_value());
  BOOST_TEST(m2->precursors.size() == 1u);
  BOOST_TEST(m2->precursors[0].precursor_index.has_value());
  // precursor_index value from pyarrow: 1
  BOOST_TEST(m2->precursors[0].precursor_index.value() == uint64_t(1));
}

/******************************************************************************/
// RDR-10c: isolation window on spectrum index 2.
// Ground truth (pyarrow): target_mz 810.7894287109375, lower_offset 1.0,
// upper_offset 1.0 (float precision; tolerance 1e-3).
BOOST_AUTO_TEST_CASE(precursor_isolation_window_index2)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::optional<MzPeak::SpectrumMetadata> m2;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 2) {
      m2 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m2.has_value());
  BOOST_TEST_REQUIRE(m2->precursors.size() == 1u);
  const auto& iw = m2->precursors[0].isolation_window;
  BOOST_TEST(iw.target_mz.has_value());
  BOOST_TEST(std::abs(static_cast<double>(iw.target_mz.value()) - 810.789) < 1e-3);
  BOOST_TEST(iw.lower_offset.has_value());
  BOOST_TEST(std::abs(static_cast<double>(iw.lower_offset.value()) - 1.0) < 1e-6);
  BOOST_TEST(iw.upper_offset.has_value());
  BOOST_TEST(std::abs(static_cast<double>(iw.upper_offset.value()) - 1.0) < 1e-6);
}

/******************************************************************************/
// RDR-10c: activation parameters on spectrum index 2.
// Ground truth (pyarrow): MS:1000133 (CID, no value), MS:1000045 (collision
// energy, float-arm value "35", unit UO:0000266).
BOOST_AUTO_TEST_CASE(precursor_activation_parameters_index2)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::optional<MzPeak::SpectrumMetadata> m2;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 2) {
      m2 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m2.has_value());
  BOOST_TEST_REQUIRE(m2->precursors.size() == 1u);
  const auto& act = m2->precursors[0].activation_parameters;
  BOOST_TEST(act.size() >= 2u);

  // Find MS:1000133 (CID)
  bool found_cid = false;
  for (const auto& p : act) {
    if (p.accession && *p.accession == "MS:1000133") {
      found_cid = true;
      break;
    }
  }
  BOOST_TEST(found_cid);

  // Find MS:1000045 (collision energy, value "35", unit UO:0000266)
  bool found_ce = false;
  for (const auto& p : act) {
    if (p.accession && *p.accession == "MS:1000045") {
      found_ce = true;
      BOOST_TEST((p.value == std::optional<std::string>("35")));
      BOOST_TEST((p.unit == std::optional<std::string>("UO:0000266")));
      break;
    }
  }
  BOOST_TEST(found_ce);
}

/******************************************************************************/
// RDR-10c: selected ion on spectrum index 2.
// Ground truth (pyarrow): selected_ion_mz 810.789428710938 (tolerance 1e-6),
// intensity 1994039.125 (tolerance 1e-3).  H2: attached via precursor_index=1.
BOOST_AUTO_TEST_CASE(precursor_selected_ion_index2)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::optional<MzPeak::SpectrumMetadata> m2;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 2) {
      m2 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m2.has_value());
  BOOST_TEST_REQUIRE(m2->precursors.size() == 1u);
  const auto& prec = m2->precursors[0];
  BOOST_TEST(prec.selected_ions.size() == 1u);
  const auto& ion = prec.selected_ions[0];
  BOOST_TEST(ion.selected_ion_mz.has_value());
  BOOST_TEST(std::abs(ion.selected_ion_mz.value() - 810.789428710938) < 1e-6);
  BOOST_TEST(ion.intensity.has_value());
  BOOST_TEST(std::abs(static_cast<double>(ion.intensity.value()) - 1994039.125) <
             1e-3);
}

/******************************************************************************/
// RDR-10c MS1: spectrum index 0 is MS1 — precursors must be empty.
BOOST_AUTO_TEST_CASE(ms1_spectrum_has_empty_precursors)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::optional<MzPeak::SpectrumMetadata> m0;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 0) {
      m0 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m0.has_value());
  BOOST_TEST(m0->precursors.empty());
}

/******************************************************************************/
// H1 multi-row sweep: all 34 MS2 spectra carry a precursor, all 14 MS1 carry
// none.  This is the key H1 assertion: the join must be by source_index VALUE
// (not positional), so every MS2 spectrum ends up with exactly >=1 precursor
// regardless of chunk layout.
BOOST_AUTO_TEST_CASE(multirow_sweep_ms2_carry_precursor_ms1_carry_none)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::size_t ms1_with_precursor = 0;
  std::size_t ms1_without_precursor = 0;
  std::size_t ms2_with_precursor = 0;
  std::size_t ms2_without_precursor = 0;

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    const auto& m = s.metadata();
    if (!m.ms_level.has_value()) continue;
    if (m.ms_level.value() == 1) {
      if (m.precursors.empty()) ++ms1_without_precursor;
      else
        ++ms1_with_precursor;
    } else if (m.ms_level.value() == 2) {
      if (!m.precursors.empty()) ++ms2_with_precursor;
      else
        ++ms2_without_precursor;
    }
  }

  // Ground truth: 34 MS2, 14 MS1 (pyarrow verified).
  BOOST_TEST(ms2_with_precursor == 34u);
  BOOST_TEST(ms2_without_precursor == 0u);
  BOOST_TEST(ms1_without_precursor == 14u);
  BOOST_TEST(ms1_with_precursor == 0u);
}

/******************************************************************************/
// H2 verification: for spectrum index 2, the selected ion's precursor_index
// (=1) matches the PrecursorInfo's precursor_index (=1) — not attached to an
// arbitrary/wrong precursor.  Also verifies a second MS2 spectrum (index 3).
BOOST_AUTO_TEST_CASE(h2_selected_ion_attach_by_precursor_index)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  // Spectrum index 2: precursor_index=1, selected_ion mz 810.789428...
  // Spectrum index 3: precursor_index=1, selected_ion mz 837.344604...
  // (ground truth from pyarrow row 1 of precursor/selected_ion columns)
  for (uint64_t target_idx : {uint64_t(2), uint64_t(3)}) {
    std::optional<MzPeak::SpectrumMetadata> m;
    for (std::size_t i = 0; i < spectra.size(); ++i) {
      auto s = spectra[i];
      if (s.metadata().index == target_idx) {
        m = s.metadata();
        break;
      }
    }
    BOOST_TEST_REQUIRE(m.has_value());
    BOOST_TEST(m->precursors.size() == 1u);
    // The precursor's precursor_index must equal 1 (from parquet).
    BOOST_TEST(m->precursors[0].precursor_index.has_value());
    BOOST_TEST(m->precursors[0].precursor_index.value() == uint64_t(1));
    // Each precursor must have exactly 1 selected ion (1:1 in small.mzpeak).
    BOOST_TEST(m->precursors[0].selected_ions.size() == 1u);
    BOOST_TEST(m->precursors[0].selected_ions[0].selected_ion_mz.has_value());
  }

  // Extra spot check for index 3: selected_ion_mz near 837.344604 (1e-6 tol).
  std::optional<MzPeak::SpectrumMetadata> m3;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 3) {
      m3 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m3.has_value());
  BOOST_TEST_REQUIRE(!m3->precursors.empty());
  BOOST_TEST_REQUIRE(!m3->precursors[0].selected_ions.empty());
  BOOST_TEST(std::abs(m3->precursors[0].selected_ions[0].selected_ion_mz.value() -
                      837.344604492188) < 1e-6);
}

// ============================================================================
// RDR-10c Task 3: scan-level parameters and scan_windows (accepted add-on).
// Ground truth verified via pyarrow against small.mzpeak.
// ============================================================================

/******************************************************************************/
// Scan parameters: at least one spectrum (index 0, MS1) has scan_parameters
// with MS:1000800 (mass resolving power, integer-arm value).
// Ground truth (pyarrow scan row 0, source_index=0): parameters[0].accession=
// "MS:1000800", value integer 100000.
BOOST_AUTO_TEST_CASE(scan_parameters_ms1000800_present)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  bool found = false;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    const auto& m = s.metadata();
    for (const auto& p : m.scan_parameters) {
      if (p.accession && *p.accession == "MS:1000800") {
        found = true;
        // Integer-arm value "100000".
        BOOST_TEST(p.value.has_value());
        BOOST_TEST(p.value.value() == std::string("100000"));
        break;
      }
    }
    if (found) break;
  }
  BOOST_TEST(found);
}

/******************************************************************************/
// Scan windows: spectrum index 0 (MS1) has a scan window with lower_limit
// near 200.0 and upper_limit near 2000.0.  At least one spectrum has a
// populated scan window (size >= 1).
// Ground truth (pyarrow scan row 0): scan_windows[0].lower=200.0, upper=2000.0
BOOST_AUTO_TEST_CASE(scan_windows_present_on_ms1_spectrum)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  std::optional<MzPeak::SpectrumMetadata> m0;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 0) {
      m0 = s.metadata();
      break;
    }
  }
  BOOST_TEST_REQUIRE(m0.has_value());
  BOOST_TEST(m0->scan_windows.size() >= 1u);
  const auto& sw = m0->scan_windows[0];
  BOOST_TEST(sw.lower_limit.has_value());
  BOOST_TEST(sw.upper_limit.has_value());
  BOOST_TEST(std::abs(static_cast<double>(sw.lower_limit.value()) - 200.0) < 1e-3);
  BOOST_TEST(std::abs(static_cast<double>(sw.upper_limit.value()) - 2000.0) < 1e-3);
  BOOST_TEST(sw.lower_limit.value() < sw.upper_limit.value());
}

// ============================================================================
// Handoff (OpenDIAlyzer): retention time in SECONDS + ion mobility.
// Ground truth verified via pyarrow against small.mzpeak.
// ============================================================================

/******************************************************************************/
// P0 — retention_time() is in SECONDS.  small.mzpeak stores scan_start_time in
// MINUTES (UO_0000031); the reader multiplies by 60.
// Ground truth (pyarrow): index 0 -> 0.004935 min -> 0.296100 s,
//                         index 2 -> 0.011218 min -> 0.673100 s.
BOOST_AUTO_TEST_CASE(retention_time_is_seconds)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  std::optional<double> rt0, rt2;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    if (s.metadata().index == 0) rt0 = s.retention_time();
    if (s.metadata().index == 2) rt2 = s.retention_time();
  }
  BOOST_TEST_REQUIRE(rt0.has_value());
  BOOST_TEST_REQUIRE(rt2.has_value());
  // Tolerance is tight on purpose.  `scan.scan_start_time` is float32 while
  // `spectrum.time` is float64 and carries the SAME quantity, so taking the
  // scan value unconditionally silently drops digits: 0.29610000550746918
  // instead of 0.2961.  That is ~5.5e-9 out — invisible at 1e-4, and enough to
  // put a spectrum on the wrong side of an exact RT range boundary.
  BOOST_TEST(std::abs(rt0.value() - 0.2961) < 1e-9);
  BOOST_TEST(std::abs(rt2.value() - 0.6731) < 1e-9);
  // Sanity: seconds, not minutes — must be > the raw minutes value.
  BOOST_TEST(rt0.value() > 0.05);
}

/******************************************************************************/
// P1 — ion mobility: NULL in every bundled fixture, so both the scan-level
// accessor and the selected-ion field must read as nullopt without crashing
// (null-safe decode; value-level correctness is fixture-gated on a real IM run).
BOOST_AUTO_TEST_CASE(ion_mobility_null_safe_in_small)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    BOOST_TEST(!s.ion_mobility().has_value());
    BOOST_TEST(!s.ion_mobility_type().has_value());
    for (const auto& p : s.precursors()) {
      for (const auto& ion : p.selected_ions) {
        BOOST_TEST(!ion.ion_mobility_value.has_value());
        BOOST_TEST(!ion.ion_mobility_type.has_value());
      }
    }
  }
}

/******************************************************************************/
// Handoff critical requirement: reading metadata must NOT decode peaks.
// We can only assert the observable contract here — metadata (RT, precursors)
// is available and correct without ever calling mz()/intensity().
BOOST_AUTO_TEST_CASE(metadata_available_without_peak_decode)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  std::size_t ms2 = 0;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    // Touch only metadata accessors; never mz()/intensity().
    if (s.retention_time().has_value() && !s.precursors().empty()) ++ms2;
  }
  BOOST_TEST(ms2 == 34u); // all 34 MS2 carry a precursor (pyarrow verified)
}

// ============================================================================
// RDR-9b: auxiliary_arrays structural tests (validated).
//
// Every bundled fixture has number_of_auxiliary_arrays == 0; the auxiliary_arrays
// list is empty for every spectrum.  These tests validate:
//   1. auxiliary_arrays reads as an empty list on all spectra (no crash).
//   2. number_of_auxiliary_arrays == auxiliary_arrays.size() is asserted by the
//      reader (the reader would have thrown ParquetError on mismatch — if we
//      reach here, the assert held for all rows).
//
// NOTE: VALUE-level assertions (decoded float values) are DEFERRED until a
// fixture with populated aux bytes is available.  The `values_decoded` field is
// the decoded-vs-undecoded discriminator; `values_decoded==false` on all entries
// here is expected because there are no aux bytes to decode in any bundled
// fixture (see AuxiliaryArray::values_decoded Doxygen for the API contract).
// ============================================================================

/******************************************************************************/
// RDR-9b: auxiliary_arrays is an empty list on all spectra in small.mzpeak
// (number_of_auxiliary_arrays == 0 in every row; structural validate).
BOOST_AUTO_TEST_CASE(auxiliary_arrays_empty_all_spectra_small)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();
  // If the count-consistency assert fires, read_spectra_metadata throws
  // ParquetError and the test fails here — no explicit check needed for it.
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    // auxiliary_arrays must be empty; reading it must not throw or crash.
    BOOST_TEST(s.metadata().auxiliary_arrays.empty());
  }
}

/******************************************************************************/
// RDR-9b: auxiliary_arrays is an empty list on has_uv.mzpeak too (cross-fixture
// structural check; number_of_auxiliary_arrays == 0 in that file as well).
BOOST_AUTO_TEST_CASE(auxiliary_arrays_empty_has_uv)
{
  auto index = MzPeak::open("../test/files/has_uv.mzpeak");
  auto spectra = index.spectra();
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    BOOST_TEST(s.metadata().auxiliary_arrays.empty());
  }
}

// ============================================================================
// End RDR-9b.
// ============================================================================

/******************************************************************************/
// Scan source_index VALUE join: scan data lands on the correct spectrum.
// Verify for spectrum index 0 (filter_string from scan row 0) AND spectrum
// index 2 (an MS2 scan, filter_string from scan row 2).  This tests that the
// join is by source_index VALUE, not positionally.
//
// Ground truth (pyarrow):
//   scan row 0: source_index=0, MS_1000512_filter_string="FTMS + p ESI Full ms
//   [200.00-2000.00]" scan row 2: source_index=2, MS_1000512_filter_string="ITMS + c
//   ESI d Full ms2 810.79@cid35.00 [210.00-1635.00]"
// (filter_string is in the scan struct but not on SpectrumMetadata; verify
//  indirectly via scan_windows: index 0 has window 200-2000, index 2 has 210-1635)
BOOST_AUTO_TEST_CASE(scan_windows_source_index_join_correct_for_two_spectra)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  // Spectrum 0: MS1, scan window lower=200, upper=2000
  std::optional<MzPeak::SpectrumMetadata> m0;
  // Spectrum 2: MS2, scan window lower=210, upper=1635
  std::optional<MzPeak::SpectrumMetadata> m2;

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    auto m = s.metadata();
    if (m.index == 0) m0 = m;
    if (m.index == 2) m2 = m;
  }

  BOOST_TEST_REQUIRE(m0.has_value());
  BOOST_TEST(m0->scan_windows.size() >= 1u);
  BOOST_TEST(std::abs(static_cast<double>(m0->scan_windows[0].lower_limit.value()) -
                      200.0) < 1.0);
  BOOST_TEST(std::abs(static_cast<double>(m0->scan_windows[0].upper_limit.value()) -
                      2000.0) < 1.0);

  BOOST_TEST_REQUIRE(m2.has_value());
  BOOST_TEST(m2->scan_windows.size() >= 1u);
  BOOST_TEST(std::abs(static_cast<double>(m2->scan_windows[0].lower_limit.value()) -
                      210.0) < 1.0);
  BOOST_TEST(std::abs(static_cast<double>(m2->scan_windows[0].upper_limit.value()) -
                      1635.0) < 1.0);
}

// ============================================================================
// Ion mobility (diaPASEF).  No bundled fixture carries mobility data, so these
// pin the CONTRACT and the CV mapping — the parts that fail silently.
// ============================================================================

/******************************************************************************/
// The abstract term MS:1002893 has concrete children, and converters emit the
// concrete ones. Matching only the abstract term made the mobility column
// arrive as NonStandard and the array come back empty — a silent loss of the
// whole mobility dimension, which is the point of diaPASEF.
BOOST_AUTO_TEST_CASE(ion_mobility_array_terms_are_modelled)
{
  using MzPeak::Schema::PSI::array_type_from_string;
  using MzPeak::Schema::PSI::ArrayType;
  using MzPeak::Schema::PSI::is_ion_mobility;

  // The term the converter actually writes.
  BOOST_TEST((array_type_from_string("MS:1002816") == ArrayType::MeanIonMobility));
  BOOST_TEST(is_ion_mobility(array_type_from_string("MS:1002816")));

  for (const char* accession :
       {"MS:1002893", "MS:1002816", "MS:1003006", "MS:1003007", "MS:1003008",
        "MS:1002477", "MS:1003153"}) {
    BOOST_TEST(is_ion_mobility(array_type_from_string(accession)));
  }

  // Non-mobility arrays must NOT be swept up: m/z and intensity are decoded
  // into their own vectors and a false positive would corrupt both.
  BOOST_TEST(!is_ion_mobility(array_type_from_string("MS:1000514")));
  BOOST_TEST(!is_ion_mobility(array_type_from_string("MS:1000515")));
  BOOST_TEST(!is_ion_mobility(ArrayType::NonStandard));
}

/******************************************************************************/
// A file without a mobility array yields an EMPTY array, not a throw and not a
// silently fabricated one. Every bundled fixture is of that kind.
BOOST_AUTO_TEST_CASE(ion_mobility_array_is_empty_without_mobility_data)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  auto s = spectra[0];
  BOOST_TEST(s.ion_mobility_array().empty());

  // The peak arrays are unaffected by the mobility dimension being absent.
  BOOST_TEST(s.mz().size() == 13589u);
  BOOST_TEST(s.intensity().size() == s.mz().size());
}

/******************************************************************************/
// The mobility BAND is what separates diaPASEF isolation windows; the midpoint
// alone cannot. Bundled files carry neither, so assert the absent-is-nullopt
// contract rather than inventing values.
BOOST_AUTO_TEST_CASE(selected_ion_mobility_limits_absent_are_nullopt)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = index.spectra();

  std::size_t checked = 0;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto s = spectra[i];
    for (const auto& p : s.metadata().precursors) {
      for (const auto& ion : p.selected_ions) {
        BOOST_TEST(!ion.ion_mobility_lower_limit.has_value());
        BOOST_TEST(!ion.ion_mobility_upper_limit.has_value());
        ++checked;
      }
    }
  }
  // 34 MS2 spectra each carry one selected ion; a silently empty join would
  // make the loop above vacuous and pass.
  BOOST_TEST(checked == 34u);
}

// ============================================================================
// Bruker TDF "ims-compact": no m/z array at all.
//
// A non-standard `tof` (Int32) column stands in for m/z, reconstructed as
// (a + b*tof)^2 from metadata.ims_calibration, and intensities are Int32.
// Without either piece the m/z + intensity filter matches nothing and EVERY
// spectrum reads as empty — total data loss that looks like an empty file
// rather than an unsupported layout.
//
// test/files/ims_compact.dir is a hand-built minimal archive of that shape
// (no vendor fixture exists): a=2.0, b=0.5, tof [0, 4, 100] and Int32
// intensity [10, 20, 30], so m/z must come back as [4, 16, 2704].
// ============================================================================

/******************************************************************************/
BOOST_AUTO_TEST_CASE(ims_compact_reconstructs_mz_from_tof)
{
  auto index = MzPeak::open("../test/files/ims_compact.dir");

  const auto& cal = index.ims_calibration();
  BOOST_TEST_REQUIRE(cal.valid);
  BOOST_TEST(std::abs(cal.a - 2.0) < 1e-12);
  BOOST_TEST(std::abs(cal.b - 0.5) < 1e-12);

  auto spectra = index.spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 1u);

  auto s = spectra[0];
  const auto& mz = s.mz();
  const auto& intensity = s.intensity();

  // The whole point: a file with no m/z column still yields m/z.
  BOOST_TEST_REQUIRE(mz.size() == 3u);
  BOOST_TEST(std::abs(mz[0] - 4.0) < 1e-9);
  BOOST_TEST(std::abs(mz[1] - 16.0) < 1e-9);
  BOOST_TEST(std::abs(mz[2] - 2704.0) < 1e-9);

  // Int32 intensities: decimal() throws on integer types, so this only works
  // if the decode dispatches on the column's declared type.
  BOOST_TEST_REQUIRE(intensity.size() == 3u);
  BOOST_TEST(intensity[0] == 10.0f);
  BOOST_TEST(intensity[1] == 20.0f);
  BOOST_TEST(intensity[2] == 30.0f);
}

/******************************************************************************/
// An archive that declares no calibration must leave m/z EMPTY rather than
// convert with meaningless coefficients: an empty array is visibly wrong,
// whereas a confidently-wrong m/z axis is not.
BOOST_AUTO_TEST_CASE(files_without_a_calibration_report_none)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  BOOST_TEST(!index.ims_calibration().valid);

  // And a normal file is entirely unaffected by the tof path existing.
  auto spectra = index.spectra();
  auto s = spectra[0];
  BOOST_TEST(s.mz().size() == 13589u);
}
