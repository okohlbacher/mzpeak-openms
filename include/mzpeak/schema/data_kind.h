/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <string>

namespace MzPeak::Schema {

/**
 * Indicates how data is encoded in a parquet file.
 */
enum class DataKind {
  /// Files that contain signal data.
  DataArray,

  /// Processed data (e.g. centroided).  It is expected that the
  /// unprocessed version of the data is present in the same MzPeak
  /// file as a DataArray.
  Peaks,

  /// Metadata relating to one of the other files.
  Metadata,

  /// Scan facet of an entity's metadata, in its own file.  Joined to the
  /// primary Metadata file by `source_index` VALUE.  (Newer writers split the
  /// metadata facets across files instead of nesting them as struct columns of
  /// a single table.)
  Scans,

  /// Precursor facet of an entity's metadata, in its own file.  See Scans.
  Precursors,

  /// Selected-ion facet of an entity's metadata, in its own file.  Joined to
  /// Precursors by (source_index, precursor_index).  See Scans.
  SelectedIons,

  /// Non-standard file that can't be decoded by this library.
  /// However, users of this library can access the raw bytes of
  /// this file.
  Proprietary,

  /// Non-standard file that can't be decoded by this library.
  /// However, users of this library can access the raw bytes of
  /// this file.
  Other
};

/**
 * Convert a DataKind to a string.
 */
std::string data_kind_to_string(DataKind);

/**
 * Parse a DataKind from a string view.
 */
DataKind data_kind_from_string(const std::string_view&);

} // namespace MzPeak::Schema
