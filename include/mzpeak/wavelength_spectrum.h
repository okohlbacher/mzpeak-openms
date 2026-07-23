/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <memory>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/encoding.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/util/slice.h"

namespace MzPeak {

// Forward declaration.
class WavelengthSpectra;

/**
 * Access to a single wavelength spectrum in an MzPeak file.
 *
 * A wavelength spectrum is structurally identical to a chromatogram: a point
 * table holding a primary axis array (the wavelength array, MS:1000617) and an
 * intensity array (MS:1000515).
 */
class WavelengthSpectrum final {
public:
  using wavelength_type = double;
  using intensity_type = float;

  // Special members are implicit (rule of zero): copyable AND movable.

  /**
   * Wavelength values (the MS:1000617 wavelength array).
   */
  const std::vector<wavelength_type>& wavelength() const;

  /**
   * Intensity values.
   */
  const std::vector<intensity_type>& intensity() const;

protected:
  friend class WavelengthSpectra;

  WavelengthSpectrum(uint64_t index,
                     std::shared_ptr<Data::Signals>,
                     const std::vector<Data::ArrayIndex::Dimension>&,
                     std::unique_ptr<Util::Slice>);

private:
  using decoder_type = Data::Encoding::Decoder<double>;

  uint64_t index_;
  decoder_type decoder_;
  std::vector<wavelength_type> wavelength_;
  std::vector<intensity_type> intensity_;
};

} // namespace MzPeak
