/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
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
 * Read per-spectrum descriptive metadata (top-level `spectrum` struct fields,
 * plus precursor / selected_ion / scan facets) from a spectra_metadata Parquet
 * table, keyed by `spectrum.index`.
 *
 * RDR-10b: exposes spectrum_type (MS:1000559), lowest/highest observed m/z,
 * data_processing_ref, and the per-spectrum parameters CvParam list.
 *
 * RDR-10c: reads the Parquet table ONCE and joins precursor, selected_ion, and
 * scan columns into each SpectrumMetadata entry by `source_index` VALUE (H1
 * fix — never by chunk-local/positional row index; chunk boundaries differ per
 * column).  Selected ions are attached to their owning PrecursorInfo by
 * explicit (source_index, precursor_index) matching (H2 fix — DIA-safe for
 * multiple precursors per spectrum and multiple selected ions per precursor).
 * Unmatched source_index values are logged to stderr (no silent loss).
 *
 * Ion-mobility fields (ion_mobility_value/type) are read from both the
 * selected_ion struct (SelectedIonInfo) and the scan struct (SpectrumMetadata::
 * ion_mobility/ion_mobility_type).  They are NULL in every bundled fixture, so
 * the decode is exercised for null-safety only; value-level correctness is
 * fixture-gated on a real diaPASEF/timsTOF run.
 *
 * Retention time (SpectrumMetadata::retention_time) is in SECONDS: sourced from
 * scan.MS_1000016_scan_start_time (UO_0000031 = minutes) ×60, with spectrum.time
 * as a fallback (also minutes).
 *
 * Nullable source values map to absent std::optional fields or empty
 * strings/vectors as documented on SpectrumMetadata.
 */
std::map<uint64_t, SpectrumMetadata> read_spectra_metadata(Parquet& metadata);

/**
 * Read the native-id → entity-index map from a metadata Parquet table whose
 * top-level column is named @p col_name (e.g. "chromatogram" or
 * "wavelength_spectrum").  Only rows with a non-null, non-empty `id` field are
 * inserted.  Used by Chromatograms::by_id() and WavelengthSpectra::by_id().
 */
std::unordered_map<std::string, std::size_t>
read_entity_id_map(Parquet& metadata, const std::string& col_name);

} // namespace MzPeak::Util
