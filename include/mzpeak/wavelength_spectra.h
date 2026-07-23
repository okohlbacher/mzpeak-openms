/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "mzpeak/util/enumerable_proxy.h"
#include "mzpeak/wavelength_spectrum.h"

namespace MzPeak::Data {
class Signals;
}

namespace MzPeak {

/**
 * Access all wavelength spectra in a MzPeak file.
 */
class WavelengthSpectra final : public Util::EnumerableProxy<WavelengthSpectrum> {
public:
  /// Default constructor — empty (no wavelength spectrum table present).
  WavelengthSpectra();

  /// Destructor.
  ~WavelengthSpectra() = default;

  /// Low-level constructor.
  explicit WavelengthSpectra(std::unique_ptr<Data::Signals>,
                              std::optional<std::size_t> count = std::nullopt);

private:
  std::shared_ptr<Data::Signals> data_;

  // Function to fetch a specific wavelength spectrum.
  WavelengthSpectrum fetch(uint64_t);
};

} // namespace MzPeak
