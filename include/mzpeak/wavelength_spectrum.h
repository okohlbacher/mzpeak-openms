/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <memory>
#include <vector>

#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/encoding.h"

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
  /// Ensure types stay in sync.  The wavelength array (MS:1000617) is stored
  /// as 64-bit floating point (MS:1000523).
  using wavelength_type =
      Schema::PSI::data_type_traits<Schema::PSI::DataType::Float64>::value_type;

  /// Ensure types stay in sync.  The intensity array (MS:1000515) is stored
  /// as 32-bit floating point (MS:1000521).
  using intensity_type =
      Schema::PSI::data_type_traits<Schema::PSI::DataType::Float32>::value_type;

  // Special members are implicit (rule of zero): copyable AND movable, so
  // returning a WavelengthSpectrum by value moves its arrays rather than
  // copying.

  /**
   * Wavelength values (the MS:1000617 wavelength array).
   */
  const std::vector<wavelength_type>& wavelength() const;

  /**
   * Intensity values.
   */
  const std::vector<intensity_type>& intensity() const;

  /**
   * Raw access to the remaining values stored in the MzPeak file for this
   * wavelength spectrum.  These values need to be decoded using the Encoding
   * class (the array index will also be needed).
   *
   * NOTE: wavelength and intensity values have been decoded already; use the
   * corresponding methods in this class to fetch those values instead.
   */
  const Util::array_map_type& raw_encoded_arrays() const;

  /**
   * The array index for this wavelength spectrum.
   */
  const Schema::ArrayIndex& array_index() const;

protected:
  friend class WavelengthSpectra;

  /// Internal constructor.
  WavelengthSpectrum(const Schema::ArrayIndex&,
                     std::unique_ptr<Util::array_map_type>);

private:
  Schema::ArrayIndex array_index_;
  std::shared_ptr<Util::array_map_type> map_;
  std::vector<wavelength_type> wavelength_;
  std::vector<intensity_type> intensity_;
};

} // namespace MzPeak
