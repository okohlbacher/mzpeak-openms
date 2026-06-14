/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mzpeak/run_metadata.h"

namespace MzPeak {

/**
 * Isolation window for a precursor, stored as the target m/z and half-width
 * offsets that the file carries.  The conversion from offsets to absolute
 * lower/upper bounds is Phase 2's responsibility (CONTEXT.md); this struct
 * preserves the file-native representation.
 */
struct IsolationWindow {
  /// `MS_1000827_isolation_window_target_mz` — centre of the window (float).
  std::optional<float> target_mz;

  /// `MS_1000828_isolation_window_lower_offset` — lower half-width (float).
  std::optional<float> lower_offset;

  /// `MS_1000829_isolation_window_upper_offset` — upper half-width (float).
  std::optional<float> upper_offset;

  /// Additional CV parameters on the isolation_window struct.
  /// Empty in bundled fixtures; reserved for extended metadata.
  std::vector<CvParam> parameters;
};

/**
 * One selected ion within a precursor entry.
 *
 * Ion-mobility fields (`ion_mobility_value`, `ion_mobility_type`) are present
 * in the schema but NULL in every bundled fixture.  They are retained as
 * typed members to preserve the API shape; their decode path is DEFERRED until
 * a fixture with populated IM values is available (see RDR-10c ion-mobility in
 * STATE.md Deferred Items).  Callers must treat them as always-nullopt for now.
 */
struct SelectedIonInfo {
  /// `MS_1000744_selected_ion_mz` — selected-ion m/z (double).
  std::optional<double> selected_ion_mz;

  /// `MS_1000041_charge_state` — charge state (int32; NULL in bundled data).
  std::optional<int> charge_state;

  /// `MS_1000042_intensity` — selected-ion intensity (float).
  std::optional<float> intensity;

  /// Ion mobility value (double) — NULL in ALL bundled fixtures.
  /// @note DEFERRED: decode path awaits an IM fixture; always nullopt today.
  std::optional<double> ion_mobility_value;

  /// Ion mobility type (string) — NULL in ALL bundled fixtures.
  /// @note DEFERRED: decode path awaits an IM fixture; always nullopt today.
  std::optional<std::string> ion_mobility_type;

  /// Additional CV parameters on this selected-ion entry.
  std::vector<CvParam> parameters;
};

/**
 * One precursor entry for a spectrum.
 *
 * `precursor_index` is the WITHIN-SPECTRUM key used to attach selected ions
 * to this precursor by (source_index, precursor_index) matching.  It is NOT
 * a global index and NOT an array position.  This makes the attach correct
 * under DIA / multi-precursor and multi-ion-per-precursor scenarios (H2 fix).
 *
 * For DIA / multi-precursor spectra, `SpectrumMetadata::precursors` may hold
 * more than one `PrecursorInfo` entry.
 */
struct PrecursorInfo {
  /// Within-spectrum key for (source_index, precursor_index) ion attach (H2).
  /// Absent when the source row carries a NULL precursor_index.
  std::optional<uint64_t> precursor_index;

  /// Native precursor identifier string.
  std::string precursor_id;

  /// Isolation window (target m/z and half-width offsets).
  IsolationWindow isolation_window;

  /// Activation CV parameters (e.g. MS:1000133 CID, MS:1000045 collision energy).
  std::vector<CvParam> activation_parameters;

  /// Selected ions belonging to this precursor, attached by (source_index,
  /// precursor_index) matching.  For DDA/DIA, multiple ions are valid.
  std::vector<SelectedIonInfo> selected_ions;
};

/**
 * One scan window (m/z range) within a scan's scan_windows list.
 * The Parquet schema stores lower/upper limits as float32; the type matches
 * the actual Arrow column type to avoid a narrowing conversion.
 */
struct ScanWindow {
  /// `MS_1000501_scan_window_lower_limit_unit_MS_1000040` — lower m/z bound
  /// (float, as stored in the Parquet schema).
  std::optional<float> lower_limit;

  /// `MS_1000500_scan_window_upper_limit_unit_MS_1000040` — upper m/z bound
  /// (float, as stored in the Parquet schema).
  std::optional<float> upper_limit;

  /// Additional CV parameters on this scan window.
  std::vector<CvParam> parameters;
};

/**
 * One auxiliary data array carried by a spectrum (e.g. additional ion-mobility
 * or intensity channels beyond the primary m/z + intensity pair).
 *
 * The Arrow schema column is `spectrum.auxiliary_arrays`:
 * large_list<struct{data:large_list<uint8>, name:struct<CvParam>,
 *   data_type:string, compression:string, unit:string,
 *   parameters:large_list<CvParam>, data_processing_ref:large_string}>.
 *
 * The field `data_type` is an opaque, lowercase Arrow dtype string
 * (e.g. "float32") — it is NOT routed through any PSI enum (Pitfall 5).
 *
 * @note DECODED vs UNDECODED contract — callers MUST check `values_decoded`:
 *   - `values_decoded == true  && values.empty()` — the source `data` buffer
 *     was successfully decoded and contains zero elements (a legitimately empty
 *     auxiliary array).
 *   - `values_decoded == false` — the raw bytes were present but the VALUE
 *     decode is a fixture-gated follow-up (UNVERIFIED against any bundled
 *     fixture) or the byte-length guard rejected a misaligned buffer.  Phase 2
 *     MUST NOT treat an empty `values` + `values_decoded == false` as "decoded
 *     empty"; it means "not decoded".
 *
 * (RDR-9b)
 */
struct AuxiliaryArray {
  /// CV term identifying the array (a lone CvParam from the `name` struct
  /// child; decoded via `extract_one_cv_param`).
  CvParam name;

  /// Arrow dtype name, lowercase (e.g. "float32", "float64", "int32").
  /// Treated as an opaque string — NOT routed through any PSI enum (Pitfall 5).
  std::string data_type;

  /// Compression scheme name (e.g. "none").
  std::string compression;

  /// Unit name for the decoded values (empty if absent).
  std::string unit;

  /// Additional CV parameters on this auxiliary array entry.
  std::vector<CvParam> parameters;

  /// Reference to the data-processing chain applied to this array.
  /// Empty when null in the source.
  std::string data_processing_ref;

  /**
   * Decoded values (float).  The decode path converts each element to float
   * regardless of the stored data_type.
   *
   * Empty when (a) the source `data` buffer has zero bytes (decoded-empty,
   * `values_decoded` is set to `true`), (b) the byte-length guard rejected
   * a misaligned buffer (`values_decoded` is `false`), or (c) the raw-byte
   * VALUE decode is a fixture-gated follow-up and was not performed
   * (`values_decoded` is `false`).
   *
   * @note Callers MUST check `values_decoded` to distinguish (a) from (b)/(c).
   */
  std::vector<float> values;

  /**
   * Decoded-vs-undecoded discriminator.
   *
   * `true` when the raw `data` buffer was actually decoded into `values`
   * (including the legitimate zero-element case: the source buffer was empty
   * and `values.clear()` was called, marking the decode as complete).
   *
   * `false` when the bytes were present but the VALUE decode was not performed
   * (fixture-gated follow-up — no bundled fixture carries aux bytes, so this
   * path is UNVERIFIED) or the byte-length guard rejected a misaligned buffer.
   *
   * Phase 2 MUST NOT interpret `values.empty() && !values_decoded` as a
   * successfully decoded empty array.
   */
  bool values_decoded = false;
};

/**
 * Per-spectrum descriptive (scalar) metadata, read from the top-level
 * `spectrum` struct of the spectra_metadata Parquet table.
 *
 * RDR-10b extends the original scalar fields with spectrum_type,
 * lowest/highest observed m/z, data_processing_ref, and the per-spectrum
 * parameters CV-param list.  Nullable fields use std::optional and are absent
 * when the source value is null.
 *
 * API contract notes:
 *   - `data_processing_ref` is a std::string and cannot distinguish SQL-null
 *     from empty (null in every bundled fixture today; if Phase 2 needs the
 *     null-vs-empty distinction, revisit as optional<string>).
 *   - An empty `parameters` means "decoded successfully, zero params" — it is
 *     the same observable state as a null/absent list.  There is no separate
 *     "failed to decode" sentinel; callers must not treat empty as undecoded.
 */
struct SpectrumMetadata final {
  /// `spectrum.index` (uint64).
  uint64_t index = 0;

  /// `spectrum.id` (the native spectrum identifier).
  std::string id;

  /// `MS_1000511_ms_level` (e.g. 1 for MS1, 2 for MS2).
  std::optional<int> ms_level;

  /// `time` — the scan / retention time (seconds, as stored).
  std::optional<double> retention_time;

  /// `MS_1000465_scan_polarity` (+1 positive, -1 negative).
  std::optional<int> polarity;

  /// `MS_1000525_spectrum_representation` (CURIE, e.g. "MS:1000128"
  /// profile, "MS:1000127" centroid).
  std::string representation;

  /// `MS_1003060_number_of_data_points`.
  std::optional<uint64_t> number_of_data_points;

  /// `MS_1003059_number_of_peaks`.
  std::optional<uint64_t> number_of_peaks;

  /// `MS_1000504_base_peak_mz`.
  std::optional<double> base_peak_mz;

  /// `MS_1000505_base_peak_intensity`.
  std::optional<float> base_peak_intensity;

  /// `MS_1000285_total_ion_current`.
  std::optional<float> total_ion_current;

  // ---- RDR-10b additions --------------------------------------------------

  /// `MS_1000559_spectrum_type` (CURIE, e.g. "MS:1000579" MS1,
  /// "MS:1000580" MS2).  Empty string when null in the source.
  std::string spectrum_type;

  /// `MS_1000528_lowest_observed_mz_unit_MS_1000040` — lowest observed m/z
  /// in the spectrum.  Absent when the source value is null.
  std::optional<double> lowest_observed_mz;

  /// `MS_1000527_highest_observed_mz_unit_MS_1000040` — highest observed m/z
  /// in the spectrum.  Absent when the source value is null.
  std::optional<double> highest_observed_mz;

  /// `data_processing_ref` — reference to the data-processing chain applied
  /// to this spectrum.  Empty string when null (null in every bundled fixture).
  /// @note Cannot distinguish SQL-null from empty; see API contract above.
  std::string data_processing_ref;

  /// Per-spectrum CV parameters (`spectrum.parameters` large_list<CvParam>).
  /// Empty when the source list is null or has zero elements.
  /// @note An empty vector means "decoded successfully, zero params".
  std::vector<CvParam> parameters;

  // ---- RDR-9b additions ---------------------------------------------------

  /**
   * Auxiliary data arrays carried by this spectrum (e.g. ion-mobility drift
   * or additional intensity channels).
   *
   * Empty in ALL bundled fixtures (number_of_auxiliary_arrays == 0 in every
   * row of every bundled file).  Schema parsing and empty-list handling are
   * validated structurally against those fixtures.  The raw-byte VALUE decode
   * (Part B of the decode path) is a FIXTURE-GATED follow-up: it is NOT
   * shipped as a validated path — see `values_decoded` on `AuxiliaryArray`.
   * (RDR-9b)
   */
  std::vector<AuxiliaryArray> auxiliary_arrays;

  // ---- RDR-10c additions --------------------------------------------------

  /// Precursor list (empty for MS1 spectra; one entry per precursor for MS2;
  /// may hold >1 entry for DIA / multi-precursor spectra — DIA-ready).
  /// Each entry is joined by source_index VALUE (H1) and its selected ions are
  /// attached by (source_index, precursor_index) matching (H2).
  std::vector<PrecursorInfo> precursors;

  /// Scan-level CV parameters (e.g. MS:1000800 mass resolving power).
  /// Decoded from the `scan.parameters` large_list column via source_index join.
  /// Empty when the source list is absent or has zero elements (accepted add-on).
  std::vector<CvParam> scan_parameters;

  /// Scan windows for this spectrum (m/z lower/upper bounds per window).
  /// Decoded from `scan.scan_windows` via source_index join (accepted add-on).
  /// Empty when no scan windows are present (e.g. for MS2 zoomed scans).
  std::vector<ScanWindow> scan_windows;
};

} // namespace MzPeak
