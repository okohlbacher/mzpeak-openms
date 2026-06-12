/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <string>
#include <vector>

namespace MzPeak::Util {

/**
 * Emit the file-level array index JSON for a point-layout spectra_data
 * table whose columns are point.spectrum_index (uint64), point.mz
 * (double) and point.intensity (float).
 *
 * The returned JSON mirrors the `spectrum_array_index` blob stored in
 * the Parquet key/value metadata: an object with a `prefix` and an
 * `entries` array. The index column itself is *not* serialized; it is
 * synthesized by the reader (see ArrayIndex parsing).
 */
std::string point_spectra_array_index_json();

/**
 * A single entry in the mzpeak_index.json `files` array.
 */
struct IndexFileEntry {
  std::string name;
  std::string entity_type;
  std::string data_kind;
};

/**
 * Emit the contents of mzpeak_index.json: a `files` array and a minimal
 * but valid `metadata` object carrying at least `version`.
 */
std::string mzpeak_index_json(const std::vector<IndexFileEntry>& files,
                              const std::string& version = "0.9.0");

} // namespace MzPeak::Util
