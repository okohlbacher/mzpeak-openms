/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace MzPeak::Util {

/**
 * Write a point-layout spectra data table to a Parquet file.
 *
 * The schema is a single top-level Arrow struct field named `point`
 * with children:
 *   - spectrum_index : uint64
 *   - mz             : double
 *   - intensity      : float
 *
 * The three input vectors are parallel: there is one entry in each per
 * data point.  They must all have the same length.
 *
 * The file is written with ZSTD compression, statistics and a page
 * index enabled, a sorting column declared on the `point.spectrum_index`
 * leaf (ascending, nulls last), and the Arrow schema stored so the
 * struct and leaf types round-trip exactly.
 *
 * @param path        Destination file path.
 * @param spectrum_index  Per-point spectrum index (uint64).
 * @param mz          Per-point m/z value (double).
 * @param intensity   Per-point intensity value (float).
 * @param file_kv     Extra file-level key/value metadata to embed
 *                    (e.g. "spectrum_array_index" -> JSON,
 *                    "spectrum_count" -> "N",
 *                    "spectrum_data_point_count" -> "M").
 *
 * @throws ParquetError on any Arrow/Parquet error, or if the input
 *         vectors do not all have the same length.
 */
void write_point_spectra_data(const std::string& path,
                              const std::vector<uint64_t>& spectrum_index,
                              const std::vector<double>& mz,
                              const std::vector<float>& intensity,
                              const std::map<std::string, std::string>& file_kv);

/**
 * Encode a point-layout spectra data table to an in-memory Parquet buffer
 * and return its raw bytes.
 *
 * This is the in-memory sibling of @ref write_point_spectra_data: it uses
 * the identical schema, writer properties (ZSTD, statistics, page index,
 * sorting column on `point.spectrum_index`) and key/value metadata, but
 * writes to an `arrow::io::BufferOutputStream` instead of a file. Used by
 * the ZIP archive writer so the Parquet member can be stored without
 * shelling out or touching the filesystem.
 *
 * @param spectrum_index  Per-point spectrum index (uint64).
 * @param mz          Per-point m/z value (double).
 * @param intensity   Per-point intensity value (float).
 * @param file_kv     Extra file-level key/value metadata to embed.
 * @return The Parquet file contents as a byte string.
 *
 * @throws ParquetError on any Arrow/Parquet error, or if the input
 *         vectors do not all have the same length.
 */
std::string
point_spectra_data_bytes(const std::vector<uint64_t>& spectrum_index,
                         const std::vector<double>& mz,
                         const std::vector<float>& intensity,
                         const std::map<std::string, std::string>& file_kv);

/**
 * The minimal per-spectrum metadata row written to spectra_metadata.parquet.
 *
 * This is the smallest set the Rust reference reader needs to resolve a
 * spectrum by index and load its profile points: `index` (the primary key,
 * must match the data table) and the data-point/peak counts that gate array
 * loading.  All other reference metadata fields are omitted for now.
 */
struct SpectrumMetaRow {
  uint64_t index;
  std::string id;
  uint8_t ms_level;
  std::optional<double> retention_time;
  std::optional<int> polarity;
  uint64_t number_of_data_points;
  uint64_t number_of_peaks;
  /// MS_1000525 spectrum representation CURIE: "MS:1000128" (profile) or
  /// "MS:1000127" (centroid).  Lets a reader pick the data vs peaks table.
  std::string representation;
};

/**
 * Write the spectra metadata table to a Parquet file.
 *
 * The schema is a single top-level Arrow struct field named `spectrum`
 * whose FIRST child is `index : uint64` (the Rust reader accesses it
 * positionally and selects rows via its page index), followed by `id`
 * (large_utf8), `MS_1000511_ms_level` (uint8),
 * `MS_1003060_number_of_data_points` (uint64) and
 * `MS_1003059_number_of_peaks` (uint64).
 *
 * Written with ZSTD, statistics, a page index and a sorting column on
 * `spectrum.index`, and store_schema enabled — matching the data table so
 * the reference reader's index-based row selection works.
 *
 * @throws ParquetError on any Arrow/Parquet error.
 */
void write_spectra_metadata(const std::string& path,
                            const std::vector<SpectrumMetaRow>& rows);

/**
 * In-memory sibling of @ref write_spectra_metadata returning the Parquet
 * bytes (used by the ZIP archive writer).
 */
std::string spectra_metadata_bytes(const std::vector<SpectrumMetaRow>& rows);

/**
 * Write the per-facet spectrum metadata files (scans / precursors /
 * selected ions) that accompany spectra_metadata.parquet in the split layout.
 *
 * The reference reader requires all three members to be present even when a
 * writer has nothing to put in them, so the precursor and selected-ion tables
 * are emitted with their schema and zero rows.  The scan table carries one row
 * per spectrum so retention time is reachable from the scan facet as well.
 */
void write_spectra_metadata_facets(const std::string& dir,
                                   const std::vector<SpectrumMetaRow>& rows);

/// In-memory sibling of @ref write_spectra_metadata_facets: returns the Parquet
/// bytes for {scans, precursors, selected_ions} in that order.
std::array<std::string, 3>
spectra_metadata_facet_bytes(const std::vector<SpectrumMetaRow>& rows);

/**
 * One chromatogram's metadata row.
 */
struct ChromatogramMetaRow {
  uint64_t index;
  std::string id;
  /// MS:1000626 chromatogram type CURIE (e.g. "MS:1000235" total ion current).
  std::string chromatogram_type;
  std::optional<int> polarity;
  uint64_t number_of_data_points;
};

/**
 * One wavelength spectrum's metadata row.
 *
 * `time` is MINUTES, as stored; the public writer converts from the seconds its
 * callers use.  The observed-range and maximum fields are computed from the
 * data actually written -- the reference writer derives them from the unsorted
 * input after sorting a copy for output, and seeds its maximum at zero so an
 * all-negative absorbance spectrum records a maximum of 0.
 */
struct WavelengthMetaRow {
  uint64_t index;
  std::string id;
  std::optional<double> time;
  uint64_t number_of_data_points;
  std::optional<double> lowest_observed_wavelength;
  std::optional<double> highest_observed_wavelength;
  std::optional<double> lambda_max;
  std::optional<float> base_peak_intensity;
  std::optional<float> total_ion_current;
};

/**
 * Write a point-layout chromatograms data table.
 *
 * Schema: a `point` struct of {chromatogram_index: uint64, time: float64,
 * intensity: float32}.  @p time is in MINUTES, as the format stores it.
 */
void write_point_chromatograms_data(
    const std::string& path,
    const std::vector<uint64_t>& chromatogram_index,
    const std::vector<double>& time,
    const std::vector<float>& intensity,
    const std::map<std::string, std::string>& file_kv);

/// In-memory sibling of @ref write_point_chromatograms_data.
std::string
point_chromatograms_data_bytes(const std::vector<uint64_t>& chromatogram_index,
                               const std::vector<double>& time,
                               const std::vector<float>& intensity,
                               const std::map<std::string, std::string>& file_kv);

/**
 * Write a point-layout wavelength-spectra data table.
 *
 * Schema: a `point` struct of {wavelength_spectrum_index: uint64,
 * wavelength: float32, intensity: float32}.  Wavelength is float32 to match the
 * reference layout; nanometre axes do not need more.
 */
void write_point_wavelength_data(const std::string& path,
                                 const std::vector<uint64_t>& spectrum_index,
                                 const std::vector<float>& wavelength,
                                 const std::vector<float>& intensity,
                                 const std::map<std::string, std::string>& file_kv);

/// In-memory sibling of @ref write_point_wavelength_data.
std::string
point_wavelength_data_bytes(const std::vector<uint64_t>& spectrum_index,
                            const std::vector<float>& wavelength,
                            const std::vector<float>& intensity,
                            const std::map<std::string, std::string>& file_kv);

/// Write chromatograms_metadata.parquet (flat, split-layout columns).
void write_chromatograms_metadata(const std::string& path,
                                  const std::vector<ChromatogramMetaRow>& rows,
                                  const std::map<std::string, std::string>& file_kv);

/// In-memory sibling of @ref write_chromatograms_metadata.
std::string
chromatograms_metadata_bytes(const std::vector<ChromatogramMetaRow>& rows,
                             const std::map<std::string, std::string>& file_kv);

/// Write wavelength_spectra_metadata.parquet (flat, split-layout columns).
void write_wavelength_metadata(const std::string& path,
                               const std::vector<WavelengthMetaRow>& rows,
                               const std::map<std::string, std::string>& file_kv);

/// In-memory sibling of @ref write_wavelength_metadata.
std::string
wavelength_metadata_bytes(const std::vector<WavelengthMetaRow>& rows,
                          const std::map<std::string, std::string>& file_kv);

} // namespace MzPeak::Util
