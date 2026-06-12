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
};

/**
 * Write a point-layout mzPeak file as an unpacked DIRECTORY.
 *
 * Produces `spectra_data.parquet` (point layout) and `mzpeak_index.json`
 * inside `dir`.  The result is readable by `MzPeak::open(dir)`.
 *
 * This is the Phase-0 vertical slice: profile/centroid distinction,
 * metadata tables, peaks tables, chunked layout, transforms and zip
 * packaging are not yet emitted.
 *
 * @throws on I/O or encoding errors, or if any spectrum's mz and
 *         intensity arrays differ in length.
 */
void write_spectra_directory(const std::filesystem::path& dir,
                             const std::vector<SpectrumData>& spectra);

} // namespace MzPeak
