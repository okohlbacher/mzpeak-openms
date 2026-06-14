/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include "mzpeak/io/archive.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/spectra.h"
#include "mzpeak/util/parquet.h"

namespace MzPeak {

// Internal implementation.

/**
 * Read-only access to the index inside a MzPeak archive.
 */
class Index {
public:
  /// Constructor.
  Index(std::unique_ptr<MzPeak::IO::Archive>);

  /// Destructor.
  ~Index();

  /**
   * Return a list of files found in the index.
   */
  const std::vector<Schema::File>& files() const;

  /**
   * The mzPeak format version from the index `metadata.version`, or an
   * empty string if the index does not declare one.
   */
  const std::string& version() const;

  /**
   * Access the spectra in the file.
   *
   * RDR-25: @p detail_level selects how much of each spectrum a fetch
   * materializes.  `Full` (the default) decodes the m/z + intensity arrays;
   * `MetadataOnly` returns per-spectrum scalar metadata with empty arrays and
   * reads no array data — a speedup for metadata-only scans.
   */
  Spectra spectra(DetailLevel detail_level = DetailLevel::Full) const;

  /**
   * Open a Parquet file directly.
   */
  std::unique_ptr<Util::Parquet> parquet(const Schema::File&) const;

protected:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace MzPeak
