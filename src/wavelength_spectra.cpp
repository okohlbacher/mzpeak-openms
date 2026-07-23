/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/data/signals.h"
#include "mzpeak/schema/psi/array_type.h"
#include "mzpeak/util/enumerable_proxy.h"
#include "mzpeak/wavelength_spectra.h"
#include "mzpeak/wavelength_spectrum.h"

namespace MzPeak {

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra()
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&WavelengthSpectra::fetch),
                    this,
                    std::placeholders::_1))
{
}

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra(std::unique_ptr<Data::Signals> data,
                                      std::optional<std::size_t> count)
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&WavelengthSpectra::fetch),
                    this,
                    std::placeholders::_1))
    , data_(std::move(data))
{
  resize(count.value_or(data_->record_count()));
}

/******************************************************************************/
WavelengthSpectrum WavelengthSpectra::fetch(uint64_t index)
{
  using enum Schema::PSI::ArrayType;
  std::vector<Data::ArrayIndex::Dimension> dims =
      data_->array_index()->dimensions() |
      std::views::filter([](auto& d) {
        return d.array_type == ElectromagneticRadiation ||
               d.array_type == Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  std::unique_ptr<Util::Slice> slice = data_->select(dims, data_->index().eq(index));
  return WavelengthSpectrum(index, data_, dims, std::move(slice));
}

} // namespace MzPeak
