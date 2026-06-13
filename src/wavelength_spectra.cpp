/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include <functional>
#include <memory>

#include "mzpeak/wavelength_spectrum.h"
#include "mzpeak/wavelength_spectra.h"
#include "mzpeak/exception.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra() {}

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra(std::unique_ptr<Util::Parquet> data,
                                     std::optional<std::size_t> count)
    : EnumerableProxy(0, std::bind(std::mem_fn(&WavelengthSpectra::fetch), this,
                                   std::placeholders::_1))
    , data_(std::make_shared<Util::DataArrays>(std::move(data)))
{
  // Wavelength spectra currently only support the "point" layout.  The chunked
  // layout uses a different top-level node ("chunk") which would require a
  // separate decode path.
  if (data_->array_index().prefix() != "point") {
    throw ParquetError("chunked wavelength spectra not yet supported");
  }

  // The wavelength spectrum count lives in the metadata table's
  // `wavelength_spectrum_count` key; the data table does not carry it, so
  // prefer the supplied count and fall back to the data table's record count
  // otherwise.
  resize(count.value_or(data_->record_count()));
}

/******************************************************************************/
WavelengthSpectrum WavelengthSpectra::fetch(std::size_t index)
{
  auto array_index(data_->array_index());
  auto wavelength_spectrum_index_column = array_index.columns()[0];

  using enum Schema::PSI::DataType;
  Query query =
      Query::Predicate<Int64>::equal_to(wavelength_spectrum_index_column, index);

  auto map = data_->read_arrays(query, array_index.columns());

  return WavelengthSpectrum(array_index, std::move(map));
}

} // namespace MzPeak
