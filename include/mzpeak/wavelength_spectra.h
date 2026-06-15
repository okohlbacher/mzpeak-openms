/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "mzpeak/data/arrays.h"
#include "mzpeak/exception.h"
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
  /// @param data        the wavelength spectrum `data arrays` table; drives
  ///                    the count when @p count is not provided.
  /// @param count       the number of wavelength spectra (typically read from
  ///                    the `wavelength_spectrum_count` key in the metadata
  ///                    table).  When absent, falls back to the data table's
  ///                    record count.
  /// @param id_to_index native-id → index map built from the wavelength
  ///                    spectrum metadata table; empty when no metadata table
  ///                    is present.
  explicit WavelengthSpectra(
      std::unique_ptr<Util::Parquet> data,
      std::optional<std::size_t> count = std::nullopt,
      std::unordered_map<std::string, std::size_t> id_to_index = {});

  /// Fetch the wavelength spectrum with the given native id.  Throws
  /// MzPeak::ParquetError if the id is unknown.
  WavelengthSpectrum by_id(const std::string& id) const;

private:
  // The wavelength spectrum data table.
  std::shared_ptr<Data::Arrays> data_;

  // Native id → wavelength-spectrum index, derived from the metadata table.
  std::unordered_map<std::string, std::size_t> id_to_index_;

  // Function to fetch a specific wavelength spectrum.
  WavelengthSpectrum fetch(std::size_t) const;
};

} // namespace MzPeak
