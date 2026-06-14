/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace MzPeak {

/**
 * Per-spectrum descriptive (scalar) metadata, read from the top-level
 * `spectrum` struct of the spectra_metadata Parquet table.
 *
 * Only the top-level scalar fields are exposed here; nested facets such as
 * scan / precursor / selected_ion are out of scope.  Nullable fields use
 * std::optional and are absent when the source value is null.
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
};

} // namespace MzPeak
