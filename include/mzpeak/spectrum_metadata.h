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
};

} // namespace MzPeak
