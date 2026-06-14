/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "mzpeak/spectrum_metadata.h"
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

/**
 * Read the per-spectrum scalar descriptive metadata (top-level `spectrum`
 * struct fields) from a spectra_metadata Parquet table, keyed by
 * `spectrum.index`.
 *
 * RDR-10b: exposes spectrum_type (MS:1000559), lowest/highest observed m/z,
 * data_processing_ref, and the per-spectrum parameters CvParam list in
 * addition to the original scalar fields.  Nested facets (scan / precursor /
 * selected_ion) are read by plans 01-02/01-03.  Nullable source values map
 * to absent std::optional fields or empty strings/vectors as documented on
 * SpectrumMetadata.
 */
std::map<uint64_t, SpectrumMetadata> read_spectra_metadata(Parquet& metadata);

} // namespace MzPeak::Util
