/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/wavelength_spectra.h"

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/data/signals.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/psi/array_type.h"
#include "mzpeak/util/enumerable_proxy.h"
#include "mzpeak/wavelength_spectrum.h"

namespace MzPeak {

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra()
    : EnumerableProxy(0,
                      std::bind(std::mem_fn(&WavelengthSpectra::fetch),
                                this,
                                std::placeholders::_1))
{
}

/******************************************************************************/
WavelengthSpectra::WavelengthSpectra(
    std::unique_ptr<Data::Signals> data,
    std::optional<std::size_t> count,
    std::map<uint64_t, WavelengthSpectrumMetadata> metadata)
    : EnumerableProxy(0,
                      std::bind(std::mem_fn(&WavelengthSpectra::fetch),
                                this,
                                std::placeholders::_1))
    , data_(std::move(data))
{
  // See Chromatograms: a declared count of zero is not evidence of an empty
  // run, and the reference writer does emit zero on files that have rows.
  const std::size_t derived = data_->record_count();
  const std::size_t declared = count.value_or(0);
  resize(declared == 0 ? derived : declared);

  md_map_ = std::make_shared<const std::map<uint64_t, WavelengthSpectrumMetadata>>(
      std::move(metadata));
  for (const auto& [index, md] : *md_map_) {
    if (md.id.empty()) continue;
    id_to_index_.emplace(md.id, static_cast<std::size_t>(index));
  }
}

/******************************************************************************/
std::optional<std::size_t>
WavelengthSpectra::index_for_id(const std::string& id) const
{
  auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) return std::nullopt;
  return it->second;
}

/******************************************************************************/
WavelengthSpectrum WavelengthSpectra::by_id(const std::string& id) const
{
  auto index = index_for_id(id);
  if (!index) throw ParquetError("no wavelength spectrum with id '" + id + "'");
  return fetch(static_cast<uint64_t>(*index));
}

/******************************************************************************/
WavelengthSpectrum WavelengthSpectra::fetch(uint64_t index) const
{
  using enum Schema::PSI::ArrayType;
  std::vector<Data::ArrayIndex::Dimension> dims =
      data_->array_index()->dimensions() | std::views::filter([](auto& d) {
        return d.array_type == ElectromagneticRadiation || d.array_type == Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  std::unique_ptr<Util::Slice> slice = data_->select(dims, data_->index().eq(index));
  return WavelengthSpectrum(index, data_, dims, std::move(slice), md_map_);
}

} // namespace MzPeak
