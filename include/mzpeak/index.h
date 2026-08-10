/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <memory>
#include <vector>

#include "mzpeak/chromatograms.h"
#include "mzpeak/ims_calibration.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/spectra.h"
#include "mzpeak/wavelength_spectra.h"

namespace MzPeak {

namespace IO {
class Archive;
}

namespace Schema {
class File;
}

namespace Util {
class Manager;
}

class Spectra;

/**
 * Read-only access to the index inside a MzPeak archive.
 */
class Index {
public:
  /// Constructor.
  Index(std::unique_ptr<MzPeak::IO::Archive>);

  /**
   * Return a list of files found in the index.
   */
  const std::vector<Schema::File>& files() const;

  /**
   * Find a file in the mzPeak archive with the given name.
   */
  std::vector<Schema::File>::const_iterator find(const std::string_view&) const;

  /**
   * The mzPeak format version from the index `metadata.version`, or an
   * empty string if the index does not declare one.
   */
  const std::string& version() const;

  /**
   * Access the spectra in the file.
   */
  Spectra spectra() const;

  /**
   * TOF -> m/z calibration declared by this archive, for the Bruker TDF
   * "ims-compact" layout.  `valid` is false when the archive declares none.
   */
  const ImsCalibration& ims_calibration() const;

  /**
   * Access the chromatograms in the file.
   */
  Chromatograms chromatograms() const;

  /**
   * Access the wavelength spectra in the file.
   */
  WavelengthSpectra wavelength_spectra() const;

  /**
   * Access the low-level MzPeak Manager object.
   */
  std::shared_ptr<Util::Manager> manager() const;

protected:
  std::shared_ptr<Util::Manager> manager_;
};

} // namespace MzPeak
