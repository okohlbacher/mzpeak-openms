/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <memory>

#include "mzpeak/ims_calibration.h"
#include "mzpeak/io/archive.h"
#include "mzpeak/run_metadata.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/util/parquet.h"

namespace MzPeak::Util {

/**
 * The archive itself: the parsed `mzpeak_index.json` plus the means to open any
 * file it names.
 *
 * Held by `shared_ptr` and handed to every reader built from one archive, so
 * that the index is parsed once and the archive outlives the readers that read
 * through it.
 */
class Manager final {
public:
  /// Constructor.
  Manager(std::unique_ptr<MzPeak::IO::Archive>);

  /**
   * Return a vector of files that are located in the mzPeak archive.
   */
  const std::vector<Schema::File>& files() const;

  /**
   * Find a file given its name.
   */
  std::vector<Schema::File>::const_iterator
  find_file(const std::string_view& name) const;

  /**
   * Open a Parquet file from the mzPeak archive.
   */
  std::unique_ptr<Util::Parquet> parquet(const Schema::File&) const;

  /**
   * The mzPeak format version from the index `metadata.version`, or an empty
   * string if the index does not declare one.
   */
  const std::string& version() const { return version_; }

  /**
   * TOF -> m/z calibration declared by this archive, for the Bruker TDF
   * "ims-compact" layout.  `valid` is false when the archive declares none.
   */
  const ImsCalibration& ims_calibration() const { return ims_; }

  /**
   * Run-level metadata from the index `metadata{}` block (run, file
   * description, software, instrument configurations, ...).  Empty when the
   * index carries none.
   */
  const RunMetadata& metadata() const { return metadata_; }

private:
  std::shared_ptr<MzPeak::IO::Archive> archive_;
  std::vector<Schema::File> files_;

  // The mzPeak format version from metadata.version (empty if absent).
  std::string version_;

  // TOF -> m/z calibration for the ims-compact layout (valid=false if absent).
  ImsCalibration ims_;

  // Typed run-level metadata blocks from metadata{} (empty if absent).
  RunMetadata metadata_;
};

} // namespace MzPeak::Util
