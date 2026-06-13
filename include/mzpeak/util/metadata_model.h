/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "mzpeak/util/parquet.h"

namespace MzPeak::Util {

/**
 * Read the per-spectrum `mz_delta_model` (the m/z null-marking delta model)
 * from a spectra_metadata Parquet table, keyed by `spectrum.index`.
 *
 * Spectra without a model are omitted.  Used to reconstruct null-marked
 * profile m/z values (see null_fill.h).
 */
std::map<uint64_t, std::vector<double>> read_mz_delta_models(Parquet& metadata);

} // namespace MzPeak::Util
