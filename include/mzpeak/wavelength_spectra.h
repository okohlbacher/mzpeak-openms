/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "mzpeak/data/arrays.h"
#include "mzpeak/util/enumerable_proxy.h"
#include "mzpeak/wavelength_spectrum.h"

namespace MzPeak {

class WavelengthSpectrum;

/**
 * Access all wavelength spectra in a MzPeak file.
 */
class WavelengthSpectra final : public Util::EnumerableProxy<WavelengthSpectrum> {
public:
  /// Default constructor.
  WavelengthSpectra();

  /// Destructor.
  ~WavelengthSpectra() = default;

public:
  /// Low-level constructor.
  ///
  /// @param data   the wavelength spectrum `data arrays` table; drives the
  ///               count when @p count is not provided.
  /// @param count  the number of wavelength spectra (typically read from the
  ///               `wavelength_spectrum_count` key in the metadata table,
  ///               which the data table does not carry).  When absent, the
  ///               count falls back to the data table's record count.
  explicit WavelengthSpectra(std::unique_ptr<Util::Parquet> data,
                             std::optional<std::size_t> count = std::nullopt);

private:
  // The wavelength spectrum data table.
  std::shared_ptr<Data::Arrays> data_;

  // Function to fetch a specific wavelength spectrum.
  WavelengthSpectrum fetch(std::size_t);
};

} // namespace MzPeak
