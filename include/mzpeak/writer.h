/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <filesystem>
#include <vector>

namespace MzPeak {

/**
 * The m/z and intensity arrays of a single spectrum to be written.
 *
 * NOTE: This is a minimal Phase-0 writer model.  It carries only the
 * two primary arrays; richer metadata and additional arrays will be
 * added as the writer matures.
 */
struct SpectrumData {
  std::vector<double> mz;
  std::vector<float> intensity;

  /// `true` for a centroid (peak) spectrum (written to the peaks table),
  /// `false` for a profile spectrum (written to the data-arrays table).
  bool centroid = false;
};

/**
 * Write a point-layout mzPeak file as an unpacked DIRECTORY.
 *
 * Produces, inside `dir`: `spectra_data.parquet` (point-layout profile
 * spectra), `spectra_peaks.parquet` (centroid spectra, only when any are
 * present), `spectra_metadata.parquet` (per-spectrum scalar metadata) and
 * `mzpeak_index.json`.  The result is readable by `MzPeak::open(dir)`.
 *
 * Still point-layout only: the chunked layout, numpress and other transforms,
 * and the run-level index metadata blocks are not yet emitted.
 *
 * @throws on I/O or encoding errors, or if any spectrum's mz and
 *         intensity arrays differ in length.
 */
void write_spectra_directory(const std::filesystem::path& dir,
                             const std::vector<SpectrumData>& spectra);

/**
 * Write a point-layout mzPeak file as a single ZIP ARCHIVE (`.mzpeak`).
 *
 * Produces an archive at `zip_path` containing `spectra_data.parquet`
 * (point layout) and `mzpeak_index.json` as STORED (uncompressed)
 * members, as the mzPeak spec mandates.  The result is readable by
 * `MzPeak::open(zip_path)`.
 *
 * The spectra are flattened, validated and per-spectrum m/z sorted
 * identically to @ref write_spectra_directory; only the container differs.
 * Any existing file at `zip_path` is truncated.
 *
 * @throws ParquetError on I/O, encoding or libzip errors, or if any
 *         spectrum's mz and intensity arrays differ in length.
 */
void write_spectra_archive(const std::filesystem::path& zip_path,
                           const std::vector<SpectrumData>& spectra);

} // namespace MzPeak
