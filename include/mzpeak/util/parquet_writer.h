/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
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
void write_point_spectra_data(
    const std::string& path,
    const std::vector<uint64_t>& spectrum_index,
    const std::vector<double>& mz,
    const std::vector<float>& intensity,
    const std::map<std::string, std::string>& file_kv);

} // namespace MzPeak::Util
