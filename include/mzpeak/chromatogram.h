/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <memory>
#include <vector>

#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/encoding.h"

namespace MzPeak {

// Forward declaration.
class Chromatograms;

/**
 * Access to a single chromatogram in an MzPeak file.
 */
class Chromatogram final {
public:
  /// Ensure types stay in sync.  The time array (MS:1000595) is stored as
  /// 64-bit floating point (MS:1000523).
  using time_type =
      Schema::PSI::data_type_traits<Schema::PSI::DataType::Float64>::value_type;

  /// Ensure types stay in sync.  The intensity array (MS:1000515) is stored
  /// as 32-bit floating point (MS:1000521).
  using intensity_type =
      Schema::PSI::data_type_traits<Schema::PSI::DataType::Float32>::value_type;

  // Special members are implicit (rule of zero): copyable AND movable, so
  // returning a Chromatogram by value moves its arrays rather than copying.

  /**
   * Time values (the MS:1000595 time array).
   */
  const std::vector<time_type>& time() const;

  /**
   * Intensity values.
   */
  const std::vector<intensity_type>& intensity() const;

  /**
   * Raw access to the remaining values stored in the MzPeak file for this
   * chromatogram.  These values need to be decoded using the Encoding class
   * (the array index will also be needed).
   *
   * NOTE: time and intensity values have been decoded already; use the
   * corresponding methods in this class to fetch those values instead.
   */
  const Util::array_map_type& raw_encoded_arrays() const;

  /**
   * The array index for this chromatogram.
   */
  const Schema::ArrayIndex& array_index() const;

protected:
  friend class Chromatograms;

  /// Internal constructor.
  Chromatogram(const Schema::ArrayIndex&, std::unique_ptr<Util::array_map_type>);

private:
  Schema::ArrayIndex array_index_;
  std::shared_ptr<Util::array_map_type> map_;
  std::vector<time_type> time_;
  std::vector<intensity_type> intensity_;
};

} // namespace MzPeak
